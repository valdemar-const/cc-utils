// cc-workbench — host UI for the node-graph platform.
//
// Step 1B+1C with metadata-driven rendering:
//   - Source/View nodes from cc-plugin-basic
//   - Property widgets driven by node_factory::property_schema()
//   - Pretty node rendering (colored headers, type-coloured pins, icons)
//   - Host-side text view renderer + View tab with dropdown

#include "cc/any_value.hpp"
#include "cc/host.hpp"
#include "cc/host_registry.hpp"
#include "cc/graph.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_loader.hpp"
#include "cc/runner.hpp"
#include "cc/view.hpp"

#include <cc/astit.hpp> // cc::ast::tl_program, visitor for AST view renderer
#include <cc/ir.hpp>    // cc::ir::module for IR view renderer

#include "hello_imgui/hello_imgui.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "imgui_stacklayout.h" // ImGui::Spring
#include "ImFileDialog/ImFileDialog.h"
#include "bundle_integration/ImFileDialogTextureHelper.h"
#include "ImGuiColorTextEdit/TextEditor.h"
#include "ne/builders.h"
#include "ne/widgets.h"
#include "pipeline_xml.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ed   = ax::NodeEditor;
namespace util = ax::NodeEditor::Utilities;
using ax::Drawing::IconType;
using ax::Widgets::Icon;

namespace
{

#if defined(__linux__)
#  include <unistd.h> // readlink
#elif defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h> // GetModuleFileNameA
#endif

// Directory containing the running executable, as a real filesystem path
// (resolves via /proc/self/exe on Linux, GetModuleFileNameA on Windows) so the
// assets/ dir and similar siblings are found regardless of the CWD. Falls back
// to "." on platforms without a known lookup.
std::filesystem::path
exe_dir()
{
#if defined(__linux__)
    char    buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf));
    if (n > 0)
    {
        return std::filesystem::path{std::string(buf, static_cast<size_t>(n))}.parent_path();
    }
#elif defined(_WIN32)
    char  buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    if (n > 0 && n < sizeof(buf))
    {
        return std::filesystem::path{std::string(buf, static_cast<size_t>(n))}.parent_path();
    }
#endif
    return std::filesystem::path{"."};
}

// ---------------------------------------------------------------------------
// Colour palettes
//
// Pin colours by value-type name; header colours by node category. Curated
// for the known types/categories; falls back to a deterministic hash colour
// for unknown ones so every type/category still gets a distinct, stable hue.
// ---------------------------------------------------------------------------
// Stable link id from an edge — so the same edge always renders as the same
// link across frames (needed for delete handling: imgui-node-editor reports
// the deleted LinkId and we map it back to the edge).
auto
stable_link_id(const cc::runtime::edge &e) -> int
{
    auto h  = std::hash<std::string> {}(e.src_node);
    h      ^= std::hash<std::string> {}(e.src_slot) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h      ^= std::hash<std::string> {}(e.dst_node) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h      ^= std::hash<std::string> {}(e.dst_slot) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return static_cast<int>(h & 0x7FFFFFFF);
}

auto
hash_color(std::string_view s) -> ImVec4
{
    // Spread hues around the wheel; keep S/L fixed for visual consistency.
    uint32_t h = 2166136261u;
    for (char c : s)
    {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    float   hue = (h % 360) / 360.0f;
    ImColor c;
    c.SetHSV(hue, 0.55f, 0.85f);
    return ImVec4(c);
}

auto
pin_color_for_type(std::string_view type_name) -> ImVec4
{
    static const std::unordered_map<std::string_view, ImVec4> known = {
            {"text", ImVec4(0.35f, 0.60f, 0.95f, 1.0f)},
            {"path", ImVec4(0.95f, 0.78f, 0.30f, 1.0f)}, // filesystem path — gold
            {"int", ImVec4(0.25f, 0.85f, 0.85f, 1.0f)},  // integer return code — cyan
            {"ast", ImVec4(0.62f, 0.40f, 0.78f, 1.0f)},
            {"ast.tl", ImVec4(0.62f, 0.40f, 0.78f, 1.0f)},
            {"ir", ImVec4(0.35f, 0.80f, 0.45f, 1.0f)},
            {"ir.module", ImVec4(0.35f, 0.80f, 0.45f, 1.0f)},
            {"tl.ast", ImVec4(0.62f, 0.40f, 0.78f, 1.0f)},
            {"bytes", ImVec4(0.95f, 0.60f, 0.25f, 1.0f)},
            {"any", ImVec4(0.55f, 0.55f, 0.55f, 1.0f)},
            {"", ImVec4(0.55f, 0.55f, 0.55f, 1.0f)}, // wildcard / unset
    };
    if (auto it = known.find(type_name); it != known.end())
    {
        return it->second;
    }
    return hash_color(type_name);
}

auto
icon_for_type(std::string_view type_name) -> IconType
{
    if (type_name == "path" || type_name == "bytes")
        return IconType::Diamond;
    if (type_name == "any" || type_name.empty())
        return IconType::Grid;
    if (type_name == "text" || type_name == "int")
        return IconType::Circle;
    return IconType::Square; // structured (ast/ir/...)
}

auto
header_color_for_category(std::string_view category) -> ImVec4
{
    static const std::unordered_map<std::string_view, ImVec4> known = {
            {"Basic", ImVec4(0.15f, 0.45f, 0.60f, 1.0f)},
            {"TL", ImVec4(0.45f, 0.25f, 0.62f, 1.0f)},
            {"Backend", ImVec4(0.64f, 0.26f, 0.26f, 1.0f)},
            {"I/O", ImVec4(0.20f, 0.55f, 0.30f, 1.0f)},
    };
    if (auto it = known.find(category); it != known.end())
    {
        return it->second;
    }
    return hash_color(category);
}

// ---------------------------------------------------------------------------
// Per-tab View state. Each View tab is an independent "television" that can
// tune to any basic.view node on the graph. Tabs are dynamic DockableWindows
// created/destroyed at runtime via HelloImGui::AddDockableWindow.
// ---------------------------------------------------------------------------
struct ViewTab
{
    int          seq = 0;               // monotonic id for unique window label
    std::string  window_label;          // "View###v<seq>" — stable ImGui ID
    std::string  source;                // instance_id of the selected view node

    // Per-tab cache + background pull (formerly global AppState fields).
    std::string                                                      cached_for;
    std::optional<cc::any_value>                                     cached_value;
    std::string                                                      cached_error;
    std::string                                                      cached_type_name;
    bool                                                             cache_stale     = true;
    std::chrono::steady_clock::time_point                            stale_since     = {};
    std::future<std::pair<std::string, std::optional<cc::any_value>>> pull_future;
    std::string                                                      pull_target;
    bool                                                             pull_running     = false;
    unsigned                                                         pull_started_gen = 0;
};

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
struct AppState
{
    std::unique_ptr<cc::host_registry> host;
    cc::runtime::plugin_loader         loader;
    cc::runtime::graph                 g;

    // imgui-node-editor uses int ids; runtime uses string instance_ids.
    int                                  next_editor_id = 1;
    std::unordered_map<int, std::string> ed2inst;
    std::unordered_map<std::string, int> inst2ed;

    // File-dialog target (which node + property key is being picked).
    struct
    {
        std::string instance;
        std::string key;
    } file_dialog_target;

    // Pending SetNodePosition requests — collected when a node is created
    // outside ed::Begin/End (e.g. from the context menu), applied next frame
    // inside the editor context where ScreenToCanvas works.
    struct pending_position
    {
        int    ed_id;
        ImVec2 canvas_pos;
    };

    std::vector<pending_position> pending_positions;

    // Last-known canvas positions, refreshed every frame inside ed::Begin/End.
    // We must not call ed::GetNodePosition outside the editor's active context
    // (it crashes when SetCurrentEditor(nullptr) has run, which happens after
    // every Pipeline tab frame). So Save reads from this cache instead.
    std::unordered_map<std::string, ImVec2> node_positions;

    // Status / menu state.
    std::size_t loaded_plugins          = 0;
    bool        about_open              = false;
    bool        canvas_navigate_content = false;

    // ---- Pipeline file state ----
    // The on-disk path of the pipeline currently shown in the canvas. Empty
    // means "untitled" — Save will prompt for a path via Save as.
    std::string pipeline_path;
    // True when there are unsaved changes. Bumped by every graph mutation
    // (see invalidate_view_cache); cleared by Save and by New/Open.
    bool pipeline_dirty = false;

    // Modals. Each *_open flag drives a small centered popup; the *_text field
    // carries the message to display. Keeping the data in g_state lets the
    // menu handlers and the Gui callback stay decoupled (no shared globals
    // beyond AppState itself).
    bool        load_error_open = false;
    std::string load_error_text;
    bool        load_warnings_open = false;
    std::string load_warnings_text;

    // Unsaved-changes confirmation. When the user requests New/Open/Quit while
    // pipeline_dirty is true we stash the pending action here and let the
    // modal drive it after the user picks Save / Don't Save / Cancel.
    enum class pending_action
    {
        none,
        new_pipeline,
        open_pipeline,
        quit
    };
    pending_action unsaved_pending = pending_action::none;

    // View tabs — dynamic DockableWindows, each with its own cache + pull.
    std::vector<ViewTab> view_tabs;
    std::string          last_view_source;      // last-used source (for new tabs)
    int                  next_view_seq     = 1;
    unsigned             view_invalidate_gen = 0;

    // Fonts
    ImFont *ui_font   = nullptr;
    ImFont *mono_font = nullptr;
};

class noop_view_context final : public cc::view_context
{
};

// Forward decl: log_view::render uses g_state.mono_font, but g_state is
// declared after log_view for order-of-init reasons.
extern AppState g_state;

// Read-only scrolling log view with click-drag selection + Ctrl+C.
// Uses ImGui::BeginChild + ImGuiListClipper so scroll (X/Y) is preserved
// across frames — no SetText that resets scroll state.
class log_view final
{
  public:

    void
    append(std::string_view line)
    {
        std::lock_guard lock(mutex_);
        buffer_.append(line);
        buffer_.append("\n");
        dirty_ = true;
    }

    void
    render()
    {
        // ---- Toolbar ----
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Checkbox("Follow", &follow_);
            ImGui::SameLine();
            if (ImGui::Button("Copy All"))
            {
                std::lock_guard lock(mutex_);
                ImGui::SetClipboardText(buffer_.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                std::lock_guard lock(mutex_);
                buffer_.clear();
                lines_.clear();
                dirty_    = false;
                sel_lo_   = sel_hi_ = -1;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu lines)", lines_.size());
        }

        std::lock_guard lock(mutex_);

        if (dirty_)
            rebuild_lines();

        if (g_state.mono_font)
            ImGui::PushFont(g_state.mono_font);

        ImGui::BeginChild("##log_text", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const float lh      = ImGui::GetTextLineHeightWithSpacing();
        const float top_y   = ImGui::GetCursorScreenPos().y;
        const bool  hovered = ImGui::IsWindowHovered();

        // ---- Mouse-based line selection ----
        if (hovered && !lines_.empty())
        {
            float my   = ImGui::GetMousePos().y;
            int   line = static_cast<int>((my - top_y) / lh);
            line       = std::clamp(line, 0, static_cast<int>(lines_.size()) - 1);

            if (ImGui::IsMouseClicked(0))
            {
                sel_lo_ = sel_hi_ = line;
            }
            else if (ImGui::IsMouseDown(0) && sel_lo_ >= 0)
            {
                sel_hi_ = line;
            }
        }

        // Ctrl+C copies selected lines
        if (hovered && sel_lo_ >= 0
            && ImGui::GetIO().KeyCtrl
            && ImGui::IsKeyPressed(ImGuiKey_C))
        {
            int lo = std::min(sel_lo_, sel_hi_);
            int hi = std::max(sel_lo_, sel_hi_);
            std::string text;
            for (int i = lo; i <= hi && i < static_cast<int>(lines_.size()); ++i)
            {
                text.append(lines_[i].data(), lines_[i].size());
                text.append("\n");
            }
            ImGui::SetClipboardText(text.c_str());
        }

        // ---- Render lines ----
        if (!lines_.empty())
        {
            int lo = (sel_lo_ >= 0) ? std::min(sel_lo_, sel_hi_) : -1;
            int hi = (sel_lo_ >= 0) ? std::max(sel_lo_, sel_hi_) : -1;

            auto *dl = ImGui::GetWindowDrawList();
            ImVec2 clip_min = dl->GetClipRectMin();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(lines_.size()), lh);
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    if (lo >= 0 && i >= lo && i <= hi)
                    {
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        dl->AddRectFilled(
                            ImVec2(clip_min.x, p.y),
                            ImVec2(p.x + 1e4f, p.y + lh),
                            IM_COL32(55, 95, 145, 130));
                    }
                    const auto &sv = lines_[i];
                    ImGui::TextUnformatted(sv.data(), sv.data() + sv.size());
                }
            }
            clipper.End();
        }

        // Sticky-to-bottom
        if (dirty_ && follow_ && was_at_bottom_)
            ImGui::SetScrollHereY(1.0f);

        was_at_bottom_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY()
                                 - 2.0f * ImGui::GetTextLineHeight();

        dirty_ = false;
        ImGui::EndChild();

        if (g_state.mono_font)
            ImGui::PopFont();
    }

  private:

    void
    rebuild_lines()
    {
        lines_.clear();
        size_t start = 0;
        for (size_t i = 0; i < buffer_.size(); ++i)
        {
            if (buffer_[i] == '\n')
            {
                lines_.emplace_back(buffer_.data() + start, i - start);
                start = i + 1;
            }
        }
        if (start < buffer_.size())
            lines_.emplace_back(buffer_.data() + start, buffer_.size() - start);
    }

    std::string              buffer_;
    std::vector<std::string_view> lines_;
    bool                     dirty_        = false;
    bool                     follow_       = true;
    bool                     was_at_bottom_ = true;
    int                      sel_lo_       = -1;
    int                      sel_hi_       = -1;
    std::mutex               mutex_;
};

AppState           g_state;
log_view           g_log;
ed::EditorContext *g_editor = nullptr;
noop_view_context  g_view_ctx;

// Drop the View-tab cache after any graph mutation. Cheap; called from every
// node/edge/property change site. Marks cache stale and stamps the time so
// the View tab can debounce auto-pull (avoid storming pulls on every keystroke
// in a multiline property editor).
//
// We also stamp `pipeline_dirty` here: every existing call site is a graph
// mutation (add/remove node, add/remove edge, property edit), so the file is
// out of sync with disk after this. Sites that aren't graph mutations (e.g.
// picking a View tab from the Combo) set the cache flags directly instead of
// calling this helper, so they don't bump pipeline_dirty.
inline void
invalidate_view_cache()
{
    for (auto &t : g_state.view_tabs)
    {
        t.cache_stale = true;
        t.stale_since = std::chrono::steady_clock::now();
    }
    ++g_state.view_invalidate_gen;
    g_state.pipeline_dirty = true;
}

void
log(std::string msg)
{
    g_log.append(msg);
}

auto
editor_id_for(const std::string &instance_id) -> int
{
    if (auto it = g_state.inst2ed.find(instance_id); it != g_state.inst2ed.end())
    {
        return it->second;
    }
    int id                       = g_state.next_editor_id++;
    g_state.inst2ed[instance_id] = id;
    g_state.ed2inst[id]          = instance_id;
    return id;
}

// ---------------------------------------------------------------------------
// Host-side view renderers (must live in the host to call ImGui directly
// without dragging ImGui into plugin .so)
// ---------------------------------------------------------------------------
class text_view_renderer final : public cc::view_renderer
{
  public:

    text_view_renderer()
    {
        editor_.SetPalette(TextEditor::GetDarkPalette());
        editor_.SetLanguage(TextEditor::Language::Cpp()); // .tl is C-like
        editor_.SetReadOnlyEnabled(true);
    }

    auto
    type_name() const -> std::string_view override
    {
        return "text";
    }

    auto
    render(const cc::any_value &value, cc::view_context &) -> void override
    {
        const auto *s = aa::any_cast<std::string>(&value);
        if (!s)
        {
            ImGui::TextDisabled("view: value is not text");
            return;
        }
        // Only push the text into the editor when it actually changes — preserves
        // scroll position / cursor across frames.
        if (*s != last_content_)
        {
            editor_.SetText(*s);
            last_content_ = *s;
        }
        if (g_state.mono_font)
        {
            ImGui::PushFont(g_state.mono_font);
        }
        editor_.Render("##view_text", ImVec2(0, 0), false);
        if (g_state.mono_font)
        {
            ImGui::PopFont();
        }
    }

  private:

    TextEditor  editor_;
    std::string last_content_;
};

// ---------------------------------------------------------------------------
// IR view renderer — renders cc::ir::module as a textual instruction listing.
// ---------------------------------------------------------------------------
class ir_view_renderer final : public cc::view_renderer
{
  public:

    ir_view_renderer()
    {
        editor_.SetPalette(TextEditor::GetDarkPalette());
        editor_.SetLanguage(TextEditor::Language::Cpp());
        editor_.SetReadOnlyEnabled(true);
    }

    auto
    type_name() const -> std::string_view override
    {
        return "ir.module";
    }

    auto
    render(const cc::any_value &value, cc::view_context &) -> void override
    {
        const auto *mod = aa::any_cast<cc::ir::module>(&value);
        if (!mod)
        {
            ImGui::TextDisabled("view: value is not ir.module");
            return;
        }
        std::string text;
        for (const auto &instr : mod->code)
        {
            switch (instr.op)
            {
            case cc::ir::opcode::ret:
                text += "ret " + std::to_string(instr.imm) + "\n";
                break;
            }
        }
        if (text.empty())
        {
            text = "(empty module)";
        }
        if (text != last_content_)
        {
            editor_.SetText(text);
            last_content_ = text;
        }
        if (g_state.mono_font)
        {
            ImGui::PushFont(g_state.mono_font);
        }
        editor_.Render("##view_ir", ImVec2(0, 0), false);
        if (g_state.mono_font)
        {
            ImGui::PopFont();
        }
    }

  private:

    TextEditor  editor_;
    std::string last_content_;
};

// ---------------------------------------------------------------------------
// AST view renderer — walks cc::ast::tl_program via visitor and renders it
// as an indented textual tree.
// ---------------------------------------------------------------------------
// Must match the producer plugin's spelling so aa::any_cast resolves the same
// typeinfo (anchored in libcc-astit, shared across DSOs).
using ast_value = std::shared_ptr<cc::ast::tl_program>;

namespace ast_view_detail
{

    // Builds a textual representation of the AST using the visitor double-dispatch.
    class stringifier final : public cc::ast::visitor
    {
      public:

        std::string text;

        void
        visit(const cc::ast::program &p) override
        {
            emit("program");
            ++depth_;
            for (const auto &s : p.body)
            {
                if (s)
                {
                    s->accept(*this);
                }
            }
            --depth_;
        }

        void
        visit(const cc::ast::return_stmt &r) override
        {
            emit("return");
            ++depth_;
            if (r.value)
            {
                r.value->accept(*this);
            }
            --depth_;
        }

        void
        visit(const cc::ast::int_literal &i) override
        {
            emit("int " + std::to_string(i.value));
        }

      private:

        void
        emit(std::string_view line)
        {
            if (depth_ > 0)
            {
                text += std::string(static_cast<size_t>(depth_) * 2, ' ');
            }
            text.append(line.data(), line.size());
            text += '\n';
        }

        int depth_ = 0;
    };

} // namespace ast_view_detail

class ast_view_renderer final : public cc::view_renderer
{
  public:

    ast_view_renderer()
    {
        editor_.SetPalette(TextEditor::GetDarkPalette());
        editor_.SetLanguage(TextEditor::Language::Cpp());
        editor_.SetReadOnlyEnabled(true);
    }

    auto
    type_name() const -> std::string_view override
    {
        return "tl.ast";
    }

    auto
    render(const cc::any_value &value, cc::view_context &) -> void override
    {
        const auto *ast = aa::any_cast<ast_value>(&value);
        if (!ast || !*ast || !(*ast)->root)
        {
            ImGui::TextDisabled("view: value is not tl.ast (or empty)");
            return;
        }
        ast_view_detail::stringifier s;
        (*ast)->root->accept(s);
        if (s.text != last_content_)
        {
            editor_.SetText(s.text);
            last_content_ = s.text;
        }
        if (g_state.mono_font)
        {
            ImGui::PushFont(g_state.mono_font);
        }
        editor_.Render("##view_ast", ImVec2(0, 0), false);
        if (g_state.mono_font)
        {
            ImGui::PopFont();
        }
    }

  private:

    TextEditor  editor_;
    std::string last_content_;
};

// ---------------------------------------------------------------------------
// int view renderer — shows a long value (typically exec's ret_code) as a
// readable "Exit code: N (0xHEX)" line + signed/unsigned interpretation.
// ---------------------------------------------------------------------------
class int_view_renderer final : public cc::view_renderer
{
  public:

    auto
    type_name() const -> std::string_view override
    {
        return "int";
    }

    auto
    render(const cc::any_value &value, cc::view_context &) -> void override
    {
        const auto *p = aa::any_cast<long>(&value);
        if (!p)
        {
            ImGui::TextDisabled("view: value is not int (long)");
            return;
        }
        long          v  = *p;
        unsigned long uv = static_cast<unsigned long>(v);

        ImGui::PushFont(g_state.mono_font);
        ImGui::TextDisabled("Exit code");
        ImGui::SameLine();
        if (v == 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.85f, 0.45f, 1.0f));
            ImGui::Text("%ld  (0x%lX)  ✓ success", v, uv);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.40f, 1.0f));
            ImGui::Text("%ld  (0x%lX)  ✗ failure", v, uv);
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
};

// ---------------------------------------------------------------------------
// path view renderer — shows a filesystem path with a Copy button so the
// user can paste it elsewhere.
// ---------------------------------------------------------------------------
class path_view_renderer final : public cc::view_renderer
{
  public:

    auto
    type_name() const -> std::string_view override
    {
        return "path";
    }

    auto
    render(const cc::any_value &value, cc::view_context &) -> void override
    {
        const auto *p = aa::any_cast<std::filesystem::path>(&value);
        if (!p)
        {
            ImGui::TextDisabled("view: value is not a path");
            return;
        }
        const std::string s = p->string();
        ImGui::PushFont(g_state.mono_font);
        ImGui::TextDisabled("Path");
        ImGui::SameLine();
        ImGui::TextUnformatted(s.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy"))
        {
            ImGui::SetClipboardText(s.c_str());
        }
        ImGui::SameLine();
        if (std::filesystem::exists(*p))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.85f, 0.45f, 1.0f));
            ImGui::TextDisabled("(exists)");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.40f, 1.0f));
            ImGui::TextDisabled("(missing)");
            ImGui::PopStyleColor();
        }
    }
};

// ---------------------------------------------------------------------------
// File-dialog poll + File-menu action openers
// ---------------------------------------------------------------------------
// Defined later in this file (after the do_open_pipeline / do_save_pipeline
// helpers they call). The "node_path" branch handles the property Browse
// button; "open_pipeline" and "save_pipeline" are the File → Open / Save as
// flows. Each is keyed by a string id so we can run several concurrently.
void poll_file_dialog();
void request_open_pipeline_dialog();
void request_save_as_dialog();
void do_save_or_save_as();

// ---------------------------------------------------------------------------
// Schema-driven property widgets
// ---------------------------------------------------------------------------
void
draw_property_widget(cc::node &n, const cc::property_desc &desc)
{
    ImGui::PushID(desc.key.data());
    std::string current {n.properties().get(desc.key)};

    switch (desc.kind)
    {
    case cc::property_kind::path:
        {
            char buf[512];
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            ImGui::TextUnformatted(desc.display_name.data());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180);
            if (ImGui::InputText("##v", buf, sizeof(buf)))
            {
                n.properties().set(desc.key, buf);
                invalidate_view_cache();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("..."))
            {
                g_state.file_dialog_target.instance = std::string {n.instance_id()};
                g_state.file_dialog_target.key      = std::string {desc.key};
                ifd::FileDialog::Instance().Open("node_path", "Open File", ".*", false, (exe_dir() / "assets").string());
            }
            break;
        }
    case cc::property_kind::multiline:
        {
            ImGui::TextUnformatted(desc.display_name.data());
            char buf[4096];
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            if (ImGui::InputTextMultiline("##v", buf, sizeof(buf), ImVec2(220, 80)))
            {
                n.properties().set(desc.key, buf);
                invalidate_view_cache();
            }
            break;
        }
    case cc::property_kind::integer:
        {
            ImGui::TextUnformatted(desc.display_name.data());
            ImGui::SameLine();
            int v = 0;
            try
            {
                v = std::stoi(current);
            }
            catch (...)
            {
            }
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("##v", &v, 1, 100))
            {
                n.properties().set(desc.key, std::to_string(v));
                invalidate_view_cache();
            }
            break;
        }
    case cc::property_kind::boolean:
        {
            bool v = (current == "1" || current == "true");
            if (ImGui::Checkbox(desc.display_name.data(), &v))
            {
                n.properties().set(desc.key, v ? "1" : "0");
                invalidate_view_cache();
            }
            break;
        }
    case cc::property_kind::text:
    default:
        {
            ImGui::TextUnformatted(desc.display_name.data());
            ImGui::SameLine();
            char buf[256];
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            ImGui::SetNextItemWidth(180);
            if (ImGui::InputText("##v", buf, sizeof(buf)))
            {
                n.properties().set(desc.key, buf);
                invalidate_view_cache();
            }
            break;
        }
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Context menu — a custom ImGui window, NOT a popup.
//
// Why not BeginPopup: ImGui popups are top-level windows that leak outside
// the canvas and conflict with imgui-node-editor's draw-channel state (the
// IM_ASSERT on Suspend). A plain child window with manual show/hide keeps
// the whole interaction contained inside the canvas host window.
// ---------------------------------------------------------------------------
struct create_menu_state
{
    bool   open = false;
    ImVec2 pos {};        // absolute screen coords — for SetNextWindowPos
    ImVec2 canvas_pos {}; // canvas-local coords — for ed::SetNodePosition
};

create_menu_state g_create_menu;

// Palette that opens when the user drags a link from a pin and releases in
// empty canvas space. Filters node factories to those that have at least one
// slot whose type is connectable to the dragged pin's type.
struct palette_drop_state
{
    bool                  open = false;
    ImVec2                canvas_pos {}; // where to place the new node
    ImVec2                screen_pos {}; // for SetNextWindowPos
    cc::type_descriptor_t src_type {};   // type of the dragged pin
    bool                  src_is_output = false;
    std::string           src_node;
    std::string           src_slot;
};

palette_drop_state g_palette;

void
draw_create_menu()
{
    if (!g_create_menu.open)
    {
        return;
    }

    ImGui::SetNextWindowPos(g_create_menu.pos);
    ImGui::SetNextWindowSize(ImVec2(240, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.14f, 0.17f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.17f, 0.98f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));

    constexpr int kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking;

    bool opened = ImGui::Begin("##create_menu", nullptr, kFlags);

    // Close on Escape or click outside any window (background click).
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        g_create_menu.open = false;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
    {
        g_create_menu.open = false;
    }

    if (opened && g_create_menu.open)
    {
        ImGui::SeparatorText("Create Node");

        // Group factories by category (alphabetical).
        std::map<std::string, std::vector<cc::node_factory *>> by_category;
        for (auto *f : g_state.host->node_factories())
        {
            by_category[std::string {f->category()}].push_back(f);
        }
        if (by_category.empty())
        {
            ImGui::TextDisabled("(no plugins loaded)");
        }

        for (auto &[category, factories] : by_category)
        {
            ImVec4 dot = header_color_for_category(category);
            ImGui::PushStyleColor(ImGuiCol_Text, dot);
            bool expanded = ImGui::TreeNodeEx(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleColor();
            if (!expanded)
            {
                continue;
            }

            for (auto *f : factories)
            {
                std::string label {f->display_name()};
                label += "##";
                label += std::string {f->type_id()};
                if (ImGui::Selectable(label.c_str()))
                {
                    auto        node = f->create();
                    std::string instance {node->instance_id()};
                    int         ed_id = editor_id_for(instance);
                    g_state.g.add_node(std::move(node));
                    invalidate_view_cache();
                    // Position is applied next frame inside ed::Begin/End —
                    // ed::SetNodePosition takes canvas-local coords.
                    g_state.pending_positions.push_back({ed_id, g_create_menu.canvas_pos});
                    log("created " + std::string {f->type_id()} + " (instance=" + instance + ")");
                    g_create_menu.open = false;
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------
// Palette drop — opens when the user drags a link from a pin and releases on
// empty canvas. Lists node factories filtered by type compatibility with the
// dragged pin. On select, spawns the node at the drop position and connects
// the dragged pin to the first compatible slot on the new node.
// ---------------------------------------------------------------------------
void
draw_palette_drop()
{
    if (!g_palette.open)
    {
        return;
    }

    ImGui::SetNextWindowPos(g_palette.screen_pos);
    ImGui::SetNextWindowSize(ImVec2(260, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.14f, 0.17f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.17f, 0.98f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));

    constexpr int kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking;

    bool opened = ImGui::Begin("##palette_drop", nullptr, kFlags);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        g_palette.open = false;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
    {
        g_palette.open = false;
    }

    if (opened && g_palette.open)
    {
        const char *direction = g_palette.src_is_output ? "input" : "output";
        ImGui::SeparatorText("Connect to new node");
        ImGui::TextDisabled("looking for %s pin of compatible type", direction);
        ImGui::Spacing();

        // For each factory, create a transient sample to introspect its slots.
        // If at least one slot is type-compatible with the dragged pin, list it.
        auto type_name = g_state.host->types().name_of(g_palette.src_type);
        if (!type_name.empty())
        {
            ImGui::TextDisabled("source type: %.*s", static_cast<int>(type_name.size()), type_name.data());
            ImGui::Spacing();
        }

        std::map<std::string, std::vector<cc::node_factory *>> by_category;
        for (auto *f : g_state.host->node_factories())
        {
            by_category[std::string {f->category()}].push_back(f);
        }

        bool any_compatible = false;

        for (auto &[category, factories] : by_category)
        {
            // Gather compatible factories for this category first so we can skip
            // the entire category header if none match.
            std::vector<cc::node_factory *> compatible;
            for (auto *f : factories)
            {
                auto sample = f->create();
                if (!sample)
                {
                    continue;
                }
                for (auto *s : sample->slots())
                {
                    if (g_palette.src_is_output)
                    {
                        // source was an output → need an input on the new node
                        if (s->dir() == cc::slot_dir::in && g_state.host->types().is_connectable(g_palette.src_type, s->type()))
                        {
                            compatible.push_back(f);
                            break;
                        }
                    }
                    else
                    {
                        // source was an input → need an output on the new node
                        if (s->dir() == cc::slot_dir::out && g_state.host->types().is_connectable(s->type(), g_palette.src_type))
                        {
                            compatible.push_back(f);
                            break;
                        }
                    }
                }
            }
            if (compatible.empty())
            {
                continue;
            }
            any_compatible = true;

            ImVec4 dot = header_color_for_category(category);
            ImGui::PushStyleColor(ImGuiCol_Text, dot);
            bool expanded = ImGui::TreeNodeEx(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleColor();
            if (!expanded)
            {
                continue;
            }

            for (auto *f : compatible)
            {
                std::string label {f->display_name()};
                label += "##";
                label += std::string {f->type_id()};
                if (ImGui::Selectable(label.c_str()))
                {
                    // Instantiate and find the first compatible slot to wire.
                    auto        new_node = f->create();
                    std::string new_id {new_node->instance_id()};
                    int         ed_id = editor_id_for(new_id);

                    std::string matched_slot;
                    for (auto *s : new_node->slots())
                    {
                        if (g_palette.src_is_output)
                        {
                            if (s->dir() == cc::slot_dir::in && g_state.host->types().is_connectable(g_palette.src_type, s->type()))
                            {
                                matched_slot = std::string {s->id()};
                                break;
                            }
                        }
                        else
                        {
                            if (s->dir() == cc::slot_dir::out && g_state.host->types().is_connectable(s->type(), g_palette.src_type))
                            {
                                matched_slot = std::string {s->id()};
                                break;
                            }
                        }
                    }

                    g_state.g.add_node(std::move(new_node));
                    g_state.pending_positions.push_back({ed_id, g_palette.canvas_pos});

                    if (!matched_slot.empty())
                    {
                        cc::runtime::edge e = g_palette.src_is_output
                                                    ? cc::runtime::edge {g_palette.src_node, g_palette.src_slot, new_id, matched_slot}
                                                    : cc::runtime::edge {new_id, matched_slot, g_palette.src_node, g_palette.src_slot};
                        g_state.g.add_edge(std::move(e));
                        log("palette: created " + std::string {f->type_id()} + " and linked " + g_palette.src_slot + " ↔ " + matched_slot);
                    }
                    else
                    {
                        log("palette: created " + std::string {f->type_id()} + " (no auto-link)");
                    }
                    invalidate_view_cache();
                    g_palette.open = false;
                }
            }
            ImGui::TreePop();
        }

        if (!any_compatible)
        {
            ImGui::TextDisabled("(no compatible nodes)");
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------
// Pretty node rendering
// ---------------------------------------------------------------------------
void
draw_node(cc::node &n)
{
    const int         ed_id   = editor_id_for(std::string {n.instance_id()});
    const auto        tid     = std::string {n.type_id()};
    const auto       *factory = g_state.host->find_node_factory(tid);
    const std::string display_name =
            factory ? std::string {factory->display_name()} : tid;
    const std::string category =
            factory ? std::string {factory->category()} : std::string {};
    const ImVec4 cat_color = header_color_for_category(category);

    util::BlueprintNodeBuilder b;
    b.Begin(ed_id);

    // ---- Header: subtle tinted band with title ----
    // Blender-style: faint category-tinted background, colored dot, title text.
    b.Header(ImVec4(
        cat_color.x * 0.12f + 0.14f,
        cat_color.y * 0.12f + 0.14f,
        cat_color.z * 0.12f + 0.18f,
        1.0f));

    // Category color dot before the title.
    {
        float cy = ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f;
        float cx = ImGui::GetCursorScreenPos().x + 4.0f;
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cx, cy), 4.0f, ImColor(cat_color), 10);
        ImGui::Dummy(ImVec2(12, ImGui::GetTextLineHeight()));
    }
    ImGui::TextUnformatted(display_name.c_str());
    ImGui::Spring(1);
    ImGui::TextDisabled("%s", tid.c_str());
    b.EndHeader();

    // ---- Two-column pin layout (no Middle) ----
    struct out_pin_t
    {
        int             id;
        const cc::slot *slot;
    };
    std::vector<out_pin_t> outputs;

    int slot_idx = 0;
    for (auto *slot : n.slots())
    {
        int pin_id = ed_id * 64 + slot_idx++;
        if (slot->dir() == cc::slot_dir::out)
        {
            outputs.push_back({pin_id, slot});
            continue;
        }
        auto   type_name = g_state.host->types().name_of(slot->type());
        if (type_name.empty())
            type_name = "any";
        ImVec4   pin_color = pin_color_for_type(type_name);
        IconType pin_icon  = icon_for_type(type_name);

        b.Input(pin_id);
        Icon(ImVec2(16, 16), pin_icon, true, pin_color, ImVec4(0, 0, 0, 0));
        ImGui::Spring(0);
        ImGui::TextUnformatted(slot->id().data());
        ImGui::Spring(0);
        ImGui::TextDisabled("%.*s", static_cast<int>(type_name.size()), type_name.data());
        b.EndInput();
    }

    for (const auto &op : outputs)
    {
        auto   type_name = g_state.host->types().name_of(op.slot->type());
        if (type_name.empty())
            type_name = "any";
        ImVec4   pin_color = pin_color_for_type(type_name);
        IconType pin_icon  = icon_for_type(type_name);

        b.Output(op.id);
        ImGui::TextDisabled("%.*s", static_cast<int>(type_name.size()), type_name.data());
        ImGui::Spring(0);
        ImGui::TextUnformatted(op.slot->id().data());
        ImGui::Spring(0);
        Icon(ImVec2(16, 16), pin_icon, true, pin_color, ImVec4(0, 0, 0, 0));
        b.EndOutput();
    }

    // ---- Footer: property editor (full width, below pins) ----
    if (factory && !factory->property_schema().empty())
    {
        b.Footer();
        for (const auto &desc : factory->property_schema())
        {
            draw_property_widget(n, desc);
        }
    }

    b.End();
}

// Drop every node and edge from the current graph. Canvas-scoped, invoked
// from the Pipeline tab toolbar.
void
clear_graph()
{
    std::vector<std::string> ids;
    ids.reserve(g_state.g.nodes().size());
    for (const auto &n : g_state.g.nodes())
    {
        ids.emplace_back(n->instance_id());
    }
    for (const auto &id : ids)
    {
        g_state.g.remove_edges_of(id);
        g_state.g.remove_node(id);
    }
    g_state.inst2ed.clear();
    g_state.ed2inst.clear();
    g_state.node_positions.clear();
    for (auto &t : g_state.view_tabs)
        t.source.clear();
    invalidate_view_cache();
    log("graph cleared");
}

// ---------------------------------------------------------------------------
// Pipeline file ops — New / Open / Save / Save as
// ---------------------------------------------------------------------------
// All four are funnelled through request_action_with_unsaved_check(): when
// the user is about to discard unsaved changes, a confirmation modal asks
// whether to Save / Don't Save / Cancel. The choice eventually reaches
// execute_pending_action(), which dispatches back into the do_* helpers.

// Last directory used by an ImFileDialog open/save. ImFileDialog itself is
// stateless across dialogs (we use distinct dialog ids), so we remember it
// ourselves and seed the next dialog. First call seeds from the current
// pipeline path's parent dir, or the working dir as a last resort.
std::string g_last_file_dir;

auto
last_file_dir() -> const std::string &
{
    if (!g_last_file_dir.empty())
    {
        return g_last_file_dir;
    }
    if (!g_state.pipeline_path.empty())
    {
        std::error_code ec;
        auto            parent = std::filesystem::path(g_state.pipeline_path).parent_path();
        if (!parent.empty())
        {
            g_last_file_dir = parent.string();
            return g_last_file_dir;
        }
    }
    g_last_file_dir = std::filesystem::current_path().string();
    return g_last_file_dir;
}

// Display name for the window title — basename of the pipeline path, or
// "untitled.pipeline" if no path is set yet.
auto
pipeline_display_name() -> std::string
{
    if (g_state.pipeline_path.empty())
    {
        return "untitled.pipeline";
    }
    return std::filesystem::path(g_state.pipeline_path).filename().string();
}

// Run the load: parse XML, populate the graph, restore positions, surface
// warnings/errors. Returns true on success.
auto
do_open_pipeline(const std::string &path) -> bool
{
    // Property values round-trip verbatim — relative paths stay relative on
    // disk and in the in-memory graph. Resolution against the pipeline's
    // directory happens at activate() time via the runner's pipeline_dir.
    auto result = cc::workbench::load_pipeline(*g_state.host, g_state.g, path);
    if (!result)
    {
        g_state.load_error_text = result.error();
        g_state.load_error_open = true;
        log("open failed: " + result.error());
        return false;
    }
    auto &lr = *result;

    // Apply restored positions: schedule ed::SetNodePosition for each instance
    // next frame inside ed::Begin/End (where the editor context is live).
    g_state.inst2ed.clear();
    g_state.ed2inst.clear();
    g_state.node_positions.clear(); // stale positions from previous graph
    for (const auto &n : g_state.g.nodes())
    {
        editor_id_for(std::string {n->instance_id()}); // primes both maps
    }
    for (const auto &[instance, p] : lr.positions)
    {
        int ed_id = editor_id_for(instance); // safe to re-call, idempotent
        g_state.pending_positions.push_back({ed_id, ImVec2(p.x, p.y)});
        // Seed the cache too so an immediate Save before the next frame picks
        // them up — draw_pipeline_canvas will overwrite with the live value.
        g_state.node_positions[instance] = ImVec2(p.x, p.y);
    }

    g_state.pipeline_path  = path;
    g_state.pipeline_dirty = false;
    for (auto &t : g_state.view_tabs)
        t.source.clear();

    // Auto Zoom-to-Fit so the user sees the restored graph immediately rather
    // than staring at the previously-empty viewport.
    g_state.canvas_navigate_content = true;

    // Surface warnings (missing plugins, skipped nodes/edges) without blocking
    // the load — the user can still inspect whatever survived.
    std::string warn_text;
    auto        append_group = [&](const char *title, const std::vector<std::string> &items)
    {
        if (items.empty())
        {
            return;
        }
        if (!warn_text.empty())
        {
            warn_text += "\n\n";
        }
        warn_text += title;
        for (const auto &it : items)
        {
            warn_text += "\n  • ";
            warn_text += it;
        }
    };
    append_group("Missing plugins (some node types may be unavailable):", lr.warnings.missing_plugins);
    append_group("Unknown node types (skipped):", lr.warnings.unknown_node_types);
    append_group("Edges to skipped nodes (skipped):", lr.warnings.skipped_edges);
    if (!warn_text.empty())
    {
        g_state.load_warnings_text = std::move(warn_text);
        g_state.load_warnings_open = true;
    }

    log("opened " + path);
    return true;
}

// Collect current canvas positions from the imgui-node-editor cache and
// serialise. We can't call ed::GetNodePosition here: do_save_pipeline runs
// in a menu handler outside ed::Begin/End, where no editor context is
// current (SetCurrentEditor(nullptr) runs at the end of each Pipeline tab
// frame). Reading from g_state.node_positions — populated every frame from
// inside ed::Begin/End — is the safe path.
auto
do_save_pipeline(const std::string &path) -> bool
{
    std::unordered_map<std::string, cc::workbench::pos> positions;
    for (const auto &n : g_state.g.nodes())
    {
        std::string id {n->instance_id()};
        auto        it = g_state.node_positions.find(id);
        if (it != g_state.node_positions.end())
        {
            positions[id] = {it->second.x, it->second.y};
        }
    }
    auto res = cc::workbench::save_pipeline(*g_state.host, g_state.g, positions, path);
    if (!res)
    {
        g_state.load_error_text = res.error();
        g_state.load_error_open = true;
        log("save failed: " + res.error());
        return false;
    }
    g_state.pipeline_path  = path;
    g_state.pipeline_dirty = false;
    g_last_file_dir        = std::filesystem::path(path).parent_path().string();
    log("saved " + path);
    return true;
}

// File → New: clear the canvas, drop the path, start fresh.
auto
do_new_pipeline() -> void
{
    clear_graph();
    g_state.pipeline_path.clear();
    g_state.pipeline_dirty = false;
    log("new pipeline");
}

// Forward decls — the modal handlers reach back into the menu actions.
void request_open_pipeline_dialog();
void request_save_as_dialog();

// Execute the action the user picked at the confirmation modal (or that was
// queued directly when there were no unsaved changes to confirm).
void
execute_pending_action()
{
    switch (g_state.unsaved_pending)
    {
    case AppState::pending_action::new_pipeline:
        do_new_pipeline();
        break;
    case AppState::pending_action::open_pipeline:
        request_open_pipeline_dialog();
        break;
    case AppState::pending_action::quit:
        HelloImGui::GetRunnerParams()->appShallExit = true;
        break;
    case AppState::pending_action::none:
        break;
    }
    g_state.unsaved_pending = AppState::pending_action::none;
}

// Gate an action that would discard unsaved changes. If the graph is dirty,
// stash the action and open the confirm modal; otherwise run it immediately.
auto
request_action_with_unsaved_check(AppState::pending_action action) -> void
{
    if (!g_state.pipeline_dirty)
    {
        g_state.unsaved_pending = action;
        execute_pending_action();
        return;
    }
    g_state.unsaved_pending = action;
}

// ---------------------------------------------------------------------------
// Definitions of the ImFileDialog poll + openers (forward-declared earlier
// because the property editor uses poll_file_dialog, which in turn needs the
// do_open_pipeline / do_save_pipeline helpers defined just above).
// ---------------------------------------------------------------------------
void
poll_file_dialog()
{
    // ---- node_path: property Browse button ----
    if (ifd::FileDialog::Instance().IsDone("node_path"))
    {
        if (ifd::FileDialog::Instance().HasResult())
        {
            std::string path = ifd::FileDialog::Instance().GetResult().string();
            if (!g_state.file_dialog_target.instance.empty())
            {
                auto *n = g_state.g.find_node(g_state.file_dialog_target.instance);
                if (n)
                {
                    n->properties().set(g_state.file_dialog_target.key, path);
                    invalidate_view_cache();
                    log("set " + g_state.file_dialog_target.key + " = " + path + " on " + g_state.file_dialog_target.instance);
                }
            }
            g_last_file_dir = std::filesystem::path(path).parent_path().string();
        }
        ifd::FileDialog::Instance().Close();
    }

    // ---- open_pipeline: File → Open ----
    if (ifd::FileDialog::Instance().IsDone("open_pipeline"))
    {
        if (ifd::FileDialog::Instance().HasResult())
        {
            std::string path = ifd::FileDialog::Instance().GetResult().string();
            do_open_pipeline(path);
            g_last_file_dir = std::filesystem::path(path).parent_path().string();
        }
        ifd::FileDialog::Instance().Close();
    }

    // ---- save_pipeline: File → Save as ----
    if (ifd::FileDialog::Instance().IsDone("save_pipeline"))
    {
        if (ifd::FileDialog::Instance().HasResult())
        {
            std::string path = ifd::FileDialog::Instance().GetResult().string();
            // If the user typed a name without the .pipeline suffix, add it — this
            // matches desktop-app behaviour where Save doesn't punish the user for
            // forgetting the extension.
            if (std::filesystem::path(path).extension() != ".pipeline")
            {
                path += ".pipeline";
            }
            do_save_pipeline(path);
        }
        ifd::FileDialog::Instance().Close();
    }
}

void
request_open_pipeline_dialog()
{
    ifd::FileDialog::Instance().Open("open_pipeline", "Open Pipeline", "*.pipeline", false, last_file_dir());
}

void
request_save_as_dialog()
{
    // ImFileDialog's Save() has no default-filename parameter, only a starting
    // dir; the user types the name in the dialog. That's acceptable — save-as
    // semantics are clear enough without prefilling.
    ifd::FileDialog::Instance().Save("save_pipeline", "Save Pipeline As", "*.pipeline", last_file_dir());
}

void
do_save_or_save_as()
{
    if (!g_state.pipeline_path.empty())
    {
        do_save_pipeline(g_state.pipeline_path);
        return;
    }
    request_save_as_dialog();
}

// Run the pipeline end-to-end: find an x86_64.assemble node, pull its "exe"
// output (which transitively activates every upstream node), then chmod the
// resulting path so it is directly executable.
void
run_pipeline()
{
    std::string target;
    for (const auto &n : g_state.g.nodes())
    {
        if (n->type_id() == "x86_64.assemble")
        {
            target = n->instance_id();
            break;
        }
    }
    if (target.empty())
    {
        log("run: no x86_64.assemble node in graph");
        return;
    }

    log("run: pulling output 'exe' of " + target + " ...");
    // Forward the pipeline's directory so nodes with path properties can
    // resolve relative entries against it (see activate_context).
    std::string pipeline_dir;
    if (!g_state.pipeline_path.empty())
    {
        pipeline_dir = std::filesystem::path(g_state.pipeline_path).parent_path().string();
    }
    cc::runtime::runner r {g_state.g, [](std::string_view msg)
                           {
                               ::log(std::string {msg});
                           },
                           pipeline_dir};
    auto                result = r.pull(target, "exe");
    if (!result)
    {
        log("run failed: " + result.error().what);
        return;
    }
    const cc::any_value *out = *result;
    if (!out || !out->has_value())
    {
        log("run: producer returned empty value");
        return;
    }
    const auto *exe_path = aa::any_cast<std::filesystem::path>(out);
    if (!exe_path)
    {
        log("run: 'exe' output is not a path");
        return;
    }
    const std::string exe_str = exe_path->string();
    log("run: built " + exe_str);
    if (std::system(("chmod +x " + exe_str).c_str()) == 0)
    {
        log("run: chmod +x ok — binary ready at " + exe_str);
    }
    else
    {
        log("run: warning, chmod returned non-zero");
    }
}

void
draw_pipeline_canvas()
{
    // ---- Canvas toolbar (per-tab actions) ----
    // These commands mutate the graph or the editor view — they belong to the
    // Pipeline tab, not the global menu. Navigate-style buttons set a flag
    // consumed inside ed::Begin/End below, where an editor context is current.
    const bool busy = std::any_of(g_state.view_tabs.begin(), g_state.view_tabs.end(),
                                  [](const ViewTab &t) { return t.pull_running; });
    if (busy)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Run"))
    {
        run_pipeline();
    }
    ImGui::SameLine();
    if (ImGui::Button("Zoom to Fit"))
    {
        g_state.canvas_navigate_content = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        clear_graph();
    }
    if (busy)
    {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (busy)
    {
        std::string pull_label;
        for (const auto &t : g_state.view_tabs)
            if (t.pull_running) { pull_label = t.pull_target; break; }
        ImGui::TextDisabled("|  COMPUTING: %s ...", pull_label.c_str());
    }
    else
    {
        ImGui::TextDisabled("|  nodes: %zu   edges: %zu", g_state.g.nodes().size(), g_state.g.edges().size());
    }
    ImGui::Separator();

    ed::SetCurrentEditor(g_editor);
    ed::Begin("Pipeline Graph", ImVec2(0, ImGui::GetContentRegionAvail().y));

    // Apply deferred SetNodePosition requests from context-menu creations.
    // ed::SetNodePosition takes canvas-local coords (same coordinate space as
    // ed::ScreenToCanvas output), and requires a current editor context.
    if (!g_state.pending_positions.empty())
    {
        for (const auto &pp : g_state.pending_positions)
        {
            ed::SetNodePosition(pp.ed_id, pp.canvas_pos);
        }
        g_state.pending_positions.clear();
    }

    if (g_state.canvas_navigate_content)
    {
        // Defer until after nodes/links are rendered this frame, so the editor
        // has up-to-date bounds to fit. Applied below, just before ed::End().
    }

    for (const auto &node_ptr : g_state.g.nodes())
    {
        draw_node(*node_ptr);
    }

    // Render links from graph edges. Each edge gets a stable hash-based link id
    // so delete handling can match a deleted LinkId back to its edge.
    for (const auto &e : g_state.g.edges())
    {
        auto *src_n = g_state.g.find_node(e.src_node);
        auto *dst_n = g_state.g.find_node(e.dst_node);
        if (!src_n || !dst_n)
        {
            continue;
        }
        int src_ed  = editor_id_for(e.src_node);
        int dst_ed  = editor_id_for(e.dst_node);
        int src_pin = -1, dst_pin = -1, idx = 0;
        for (auto *s : src_n->slots())
        {
            if (s->dir() == cc::slot_dir::out && s->id() == e.src_slot)
            {
                src_pin = src_ed * 64 + idx;
                break;
            }
            ++idx;
        }
        idx = 0;
        for (auto *s : dst_n->slots())
        {
            if (s->dir() == cc::slot_dir::in && s->id() == e.dst_slot)
            {
                dst_pin = dst_ed * 64 + idx;
                break;
            }
            ++idx;
        }
        if (src_pin < 0 || dst_pin < 0)
        {
            continue;
        }
        ed::Link(stable_link_id(e), src_pin, dst_pin, ImVec4(0.7f, 0.7f, 0.7f, 1.0f), 2.0f);
    }

    // ---- New-link creation (drag from output to input pin) ----
    // Two outcomes:
    //   1. dropped on another pin → create edge (QueryNewLink path)
    //   2. dropped on empty canvas → open palette to spawn a node with a
    //      type-compatible slot (QueryNewNode path)
    static ed::PinId pending_new_a, pending_new_b;
    static bool      pending_new = false;
    static ed::PinId pending_drop_pin;
    static bool      pending_drop = false;
    if (ed::BeginCreate(ImColor(120, 160, 220, 200), 2.0f))
    {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b))
        {
            if (ed::AcceptNewItem())
            {
                pending_new_a = a;
                pending_new_b = b;
                pending_new   = true;
            }
        }
        else if (ed::QueryNewNode(&a))
        {
            if (ed::AcceptNewItem())
            {
                pending_drop_pin = a;
                pending_drop     = true;
            }
        }
        ed::EndCreate();
    }
    if (pending_new)
    {
        auto decode = [](ed::PinId p) -> std::pair<int, int>
        {
            int v = static_cast<int>(reinterpret_cast<intptr_t>(p.AsPointer()));
            return {v / 64, v % 64};
        };
        auto [a_ed, a_idx] = decode(pending_new_a);
        auto [b_ed, b_idx] = decode(pending_new_b);
        auto ia            = g_state.ed2inst.find(a_ed);
        auto ib            = g_state.ed2inst.find(b_ed);
        if (ia != g_state.ed2inst.end() && ib != g_state.ed2inst.end())
        {
            auto *na      = g_state.g.find_node(ia->second);
            auto *nb      = g_state.g.find_node(ib->second);
            auto  slot_at = [](cc::node *n, int idx) -> const cc::slot *
            {
                int i = 0;
                for (auto *s : n->slots())
                {
                    if (i == idx)
                    {
                        return s;
                    }
                    ++i;
                }
                return nullptr;
            };
            auto *sa = slot_at(na, a_idx);
            auto *sb = slot_at(nb, b_idx);
            if (sa && sb && sa->dir() != sb->dir())
            { // one must be in, one out
                const cc::slot *out_s;
                const cc::node *out_n;
                const cc::slot *in_s;
                const cc::node *in_n;
                if (sa->dir() == cc::slot_dir::out)
                {
                    out_s = sa;
                    out_n = na;
                    in_s  = sb;
                    in_n  = nb;
                }
                else
                {
                    out_s = sb;
                    out_n = nb;
                    in_s  = sa;
                    in_n  = na;
                }
                cc::runtime::edge e {
                        std::string {out_n->instance_id()}, std::string {out_s->id()}, std::string {in_n->instance_id()}, std::string {in_s->id()}
                };
                g_state.g.add_edge(std::move(e));
                invalidate_view_cache();
                log("linked " + std::string {out_s->id()} + " → " + std::string {in_s->id()});
            }
        }
        pending_new = false;
    }
    if (pending_drop)
    {
        // Decode the dragged pin into (node_instance, slot*) so we can filter
        // palette candidates by connectable type.
        auto decode = [](ed::PinId p) -> std::pair<int, int>
        {
            int v = static_cast<int>(reinterpret_cast<intptr_t>(p.AsPointer()));
            return {v / 64, v % 64};
        };
        auto [ed_id, slot_idx] = decode(pending_drop_pin);
        auto it                = g_state.ed2inst.find(ed_id);
        if (it != g_state.ed2inst.end())
        {
            auto *n = g_state.g.find_node(it->second);
            if (n)
            {
                const cc::slot *s = nullptr;
                int             i = 0;
                for (auto *sp : n->slots())
                {
                    if (i == slot_idx)
                    {
                        s = sp;
                        break;
                    }
                    ++i;
                }
                if (s)
                {
                    // Use io.MousePos, NOT MouseClickedPos[Left]: this is a drag, so
                    // the press position sits on the source pin (where the link was
                    // grabbed), while MousePos is the current = release position — i.e.
                    // where the user actually dropped. Inside ed::Begin/End both are
                    // canvas-local (imgui-node-editor transforms io.MousePos);
                    // CanvasToScreen converts to absolute screen for SetNextWindowPos.
                    ImVec2 canvas_pos       = ImGui::GetIO().MousePos;
                    ImVec2 screen_pos       = ed::CanvasToScreen(canvas_pos);
                    g_palette.open          = true;
                    g_palette.canvas_pos    = canvas_pos;
                    g_palette.screen_pos    = screen_pos;
                    g_palette.src_type      = s->type();
                    g_palette.src_is_output = (s->dir() == cc::slot_dir::out);
                    g_palette.src_node      = it->second;
                    g_palette.src_slot      = std::string {s->id()};
                }
            }
        }
        pending_drop = false;
    }

    // ---- Delete: nodes and links ----
    if (ed::BeginDelete())
    {
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid))
        {
            if (ed::AcceptDeletedItem())
            {
                int  v  = static_cast<int>(reinterpret_cast<intptr_t>(nid.AsPointer()));
                auto it = g_state.ed2inst.find(v);
                if (it != g_state.ed2inst.end())
                {
                    std::string instance = it->second;
                    g_state.g.remove_edges_of(instance);
                    g_state.g.remove_node(instance);
                    for (auto &t : g_state.view_tabs)
                        if (t.source == instance)
                            t.source.clear();
                    g_state.inst2ed.erase(instance);
                    g_state.node_positions.erase(instance);
                    g_state.ed2inst.erase(it);
                    invalidate_view_cache();
                    log("deleted node " + instance);
                }
            }
        }
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid))
        {
            if (ed::AcceptDeletedItem())
            {
                int link_v = static_cast<int>(reinterpret_cast<intptr_t>(lid.AsPointer()));
                for (const auto &e : g_state.g.edges())
                {
                    if (stable_link_id(e) == link_v)
                    {
                        std::string src = e.src_node, src_s = e.src_slot;
                        std::string dst = e.dst_node, dst_s = e.dst_slot;
                        g_state.g.remove_edge(src, src_s, dst, dst_s);
                        invalidate_view_cache();
                        log("deleted link " + src_s + " → " + dst_s);
                        break;
                    }
                }
            }
        }
        ed::EndDelete();
    }

    // ---- Right-click → open custom menu window ----
    // ShowBackgroundContextMenu fires only on right-clicks inside the editor's
    // background — never on tabs or other windows. We capture the click pos
    // (screen coords) and let draw_create_menu() render a custom window at
    // that position. No Suspend/Resume, no ImGui::BeginPopup — keeps the
    // interaction contained inside the canvas.
    if (ed::ShowBackgroundContextMenu())
    {
        // Inside ed::Begin/End, ImGui::GetIO().MouseClickedPos[] returns
        // *canvas-local* coordinates (origin = canvas view center), not absolute
        // screen coords. We need both:
        //   - screen coords  → SetNextWindowPos for the menu window
        //   - canvas coords  → ed::SetNodePosition for newly created nodes
        // ed::CanvasToScreen does the conversion, but only while an editor
        // context is current — so we must call it here, not after ed::End.
        ImVec2 canvas_pos        = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Right];
        ImVec2 screen_pos        = ed::CanvasToScreen(canvas_pos);
        g_create_menu.open       = true;
        g_create_menu.pos        = screen_pos;
        g_create_menu.canvas_pos = canvas_pos;
        log("popup click canvas=(" + std::to_string(static_cast<int>(canvas_pos.x)) + "," + std::to_string(static_cast<int>(canvas_pos.y)) + ") screen=(" + std::to_string(static_cast<int>(screen_pos.x)) + "," + std::to_string(static_cast<int>(screen_pos.y)) + ")");
    }

    // NavigateToContent needs the editor to know current node bounds, which
    // only happens once nodes have been submitted this frame. Call it here,
    // just before ed::End() — still inside ed::Begin/End so context is live.
    if (g_state.canvas_navigate_content)
    {
        ed::NavigateToContent();
        g_state.canvas_navigate_content = false;
    }

    // Cache canvas positions every frame so Save (which runs outside
    // ed::Begin/End, with the editor context cleared) can read them. Done here
    // after every node has been submitted so GetNodePosition returns fresh,
    // stable coordinates for every existing node.
    for (const auto &n : g_state.g.nodes())
    {
        int ed_id = editor_id_for(std::string {n->instance_id()});
        g_state.node_positions[std::string {n->instance_id()}] =
                ed::GetNodePosition(ed_id);
    }

    ed::End();
    ed::SetCurrentEditor(nullptr);

    // Render the menu OUTSIDE ed::Begin/End so it's a plain ImGui window,
    // not affected by the editor's draw channels.
    draw_create_menu();
    draw_palette_drop();
}

// ---------------------------------------------------------------------------
// View tabs — per-tab rendering + lifecycle
// ---------------------------------------------------------------------------
void draw_view_window(ViewTab &tab);

void
open_view_tab(const std::string &source)
{
    int   seq = g_state.next_view_seq++;
    auto &tab = g_state.view_tabs.emplace_back();
    tab.seq          = seq;
    tab.window_label = "View###v" + std::to_string(seq);
    tab.source       = source;
    tab.cache_stale  = true;
    tab.stale_since  = std::chrono::steady_clock::now();

    HelloImGui::DockableWindow w;
    w.label         = tab.window_label;
    w.dockSpaceName = "BottomSpace";
    w.canBeClosed   = true;
    int cap_seq     = seq;
    w.GuiFunction   = [cap_seq]()
    {
        for (auto &t : g_state.view_tabs)
            if (t.seq == cap_seq)
            {
                draw_view_window(t);
                return;
            }
    };
    HelloImGui::AddDockableWindow(w);
}

// Detect View tabs closed via the X button — remove the DockableWindow through
// the proper HelloImGui API and erase the ViewTab entry. Only erase when the
// DockableWindow is actually present and invisible; newly-opened tabs whose
// window is still in the AddDockableWindow queue are skipped.
void
poll_view_tabs()
{
    auto &docks = HelloImGui::GetRunnerParams()->dockingParams.dockableWindows;
    for (auto it = g_state.view_tabs.begin(); it != g_state.view_tabs.end();)
    {
        auto dw_it = std::find_if(
            docks.begin(), docks.end(),
            [&](const HelloImGui::DockableWindow &dw) { return dw.label == it->window_label; });

        if (dw_it != docks.end() && !dw_it->isVisible)
        {
            HelloImGui::RemoveDockableWindow(it->window_label);
            it = g_state.view_tabs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void
draw_view_window(ViewTab &tab)
{
    struct view_entry
    {
        std::string instance;
        std::string label;
    };

    std::vector<view_entry> views;

    std::unordered_map<std::string, int> name_counts;
    for (const auto &n : g_state.g.nodes())
    {
        if (n->type_id() != "basic.view")
        {
            continue;
        }
        name_counts[std::string {n->properties().get("name")}]++;
    }
    for (const auto &n : g_state.g.nodes())
    {
        if (n->type_id() != "basic.view")
        {
            continue;
        }
        std::string name {n->properties().get("name")};
        if (name.empty())
        {
            name = "(unnamed)";
        }
        std::string label = name;
        if (name_counts[name] > 1)
        {
            label += "  [" + std::string {n->instance_id().substr(0, 16)} + "...]";
        }
        views.push_back({std::string {n->instance_id()}, std::move(label)});
    }

    if (views.empty())
    {
        ImGui::TextDisabled("no view nodes in the pipeline.");
        ImGui::TextDisabled("right-click the canvas → Basic → View to add one,");
        ImGui::TextDisabled("then drag from a Source output to its input.");
        return;
    }

    // Validate / auto-switch source if the node was deleted or pipeline changed.
    auto found = std::find_if(
        views.begin(), views.end(),
        [&](const view_entry &v) { return v.instance == tab.source; });
    if (found == views.end())
    {
        tab.source               = views.front().instance;
        g_state.last_view_source = tab.source;
        tab.cache_stale          = true;
        found                    = views.begin();
    }
    int current_idx = static_cast<int>(std::distance(views.begin(), found));

    auto getter = [](void *data, int idx) -> const char *
    {
        const auto *v = static_cast<const std::vector<view_entry> *>(data);
        return (*v)[idx].label.c_str();
    };
    ImGui::SetNextItemWidth(320);
    if (ImGui::Combo("##view_select", &current_idx, getter, &views, static_cast<int>(views.size())))
    {
        tab.source               = views[current_idx].instance;
        g_state.last_view_source = tab.source;
        // Per-tab UI change — invalidate only this tab, not pipeline_dirty.
        tab.cache_stale = true;
        tab.stale_since = std::chrono::steady_clock::now();
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
    {
        invalidate_view_cache();
    }

    // Trigger a background pull when the cache has been stale longer than the
    // debounce window.
    auto trigger_pull = [&]()
    {
        tab.pull_target      = tab.source;
        tab.pull_running     = true;
        tab.pull_started_gen = g_state.view_invalidate_gen;
        tab.cached_error.clear();
        std::string target = tab.source;
        tab.pull_future     = std::async(
                std::launch::async,
                [target]() -> std::pair<std::string, std::optional<cc::any_value>>
                {
                    std::string pipeline_dir;
                    if (!g_state.pipeline_path.empty())
                    {
                        pipeline_dir = std::filesystem::path(g_state.pipeline_path)
                                               .parent_path()
                                               .string();
                    }
                    cc::runtime::runner r {g_state.g, [](std::string_view msg)
                                           {
                                               ::log(std::string {msg});
                                           },
                                           pipeline_dir};
                    auto                result = r.pull(target, "in");
                    if (!result)
                    {
                        return {result.error().what, std::nullopt};
                    }
                    const cc::any_value *v = *result;
                    if (!v || !v->has_value())
                    {
                        return {std::string {"(no value — input is empty)"}, std::nullopt};
                    }
                    return {std::string {}, std::optional<cc::any_value> {*v}};
                }
        );
    };

    if (tab.pull_running)
    {
        using namespace std::chrono_literals;
        if (tab.pull_future.wait_for(0s) == std::future_status::ready)
        {
            auto [error_or_type, value_opt] = tab.pull_future.get();
            tab.pull_running = false;
            tab.cached_for   = tab.pull_target;
            // Only mark fresh if no graph edit invalidated during this pull AND
            // the source hasn't changed (user switched channel mid-pull).
            if (tab.pull_started_gen == g_state.view_invalidate_gen && tab.pull_target == tab.source)
            {
                tab.cache_stale = false;
            }
            if (!error_or_type.empty())
            {
                tab.cached_error = std::move(error_or_type);
                tab.cached_value.reset();
            }
            else
            {
                tab.cached_error.clear();
                tab.cached_value = std::move(value_opt);
            }
        }
    }
    else if (!tab.source.empty())
    {
        constexpr auto kDebounce = std::chrono::milliseconds(150);
        const bool     should_pull =
                tab.cache_stale && (std::chrono::steady_clock::now() - tab.stale_since >= kDebounce);
        if (should_pull)
        {
            trigger_pull();
        }
    }

    if (tab.pull_running)
    {
        ImGui::TextDisabled("(pulling %s ...)", tab.pull_target.c_str());
    }
    else
    {
        ImGui::TextDisabled("(reactive, %ums debounce)", 150u);
    }

    ImGui::Separator();

    if (!tab.cached_error.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.50f, 0.40f, 1.0f));
        ImGui::TextWrapped("error: %s", tab.cached_error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (!tab.cached_value.has_value())
    {
        ImGui::TextDisabled("(no value yet — waiting for upstream)");
        return;
    }

    auto  type_desc = tab.cached_value->type_descriptor();
    auto *renderer  = g_state.host->renderers().get_for_type(type_desc);
    if (!renderer)
    {
        ImGui::TextDisabled("(no renderer registered for type '%s')", tab.cached_type_name.c_str());
        return;
    }
    static noop_view_context ctx;
    renderer->render(*tab.cached_value, ctx);
}

// ---------------------------------------------------------------------------
// Main menu, About popup, status bar
// ---------------------------------------------------------------------------
#ifndef CC_VERSION_STRING
#define CC_VERSION_STRING "0.0.0.0"
#endif

void
draw_main_menu(HelloImGui::DockingParams &docking)
{
    // ---- File ----
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New", "Ctrl+N"))
        {
            request_action_with_unsaved_check(AppState::pending_action::new_pipeline);
        }
        if (ImGui::MenuItem("Open...", "Ctrl+O"))
        {
            request_action_with_unsaved_check(AppState::pending_action::open_pipeline);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S"))
        {
            do_save_or_save_as();
        }
        if (ImGui::MenuItem("Save as..."))
        {
            request_save_as_dialog();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4"))
        {
            request_action_with_unsaved_check(AppState::pending_action::quit);
        }
        ImGui::EndMenu();
    }

    // ---- View ----
    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::BeginMenu("Open tab"))
        {
            if (ImGui::MenuItem("View"))
            {
                open_view_tab(g_state.last_view_source);
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();

        // Generic visibility toggles for any window that opts into the View menu
        // via includeInViewMenu=true.
        for (auto &w : docking.dockableWindows)
        {
            if (w.includeInViewMenu)
            {
                ImGui::MenuItem(w.label.c_str(), nullptr, &w.isVisible);
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Reset layout", nullptr))
        {
            HelloImGui::GetRunnerParams()->dockingParams.layoutReset = true;
        }
        if (ImGui::BeginMenu("Switch layout"))
        {
            if (ImGui::MenuItem("default"))
            {
                // Only one named layout exists today — selecting it is the same as a
                // Reset. The submenu exists so the discoverable entry matches the
                // user story; additional named layouts will slot in alongside.
                HelloImGui::GetRunnerParams()->dockingParams.layoutReset = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    // ---- Help ----
    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("About cc-workbench"))
        {
            g_state.about_open = true;
        }
        ImGui::EndMenu();
    }
}

// ---------------------------------------------------------------------------
// Global hotkeys: Ctrl+N / Ctrl+O / Ctrl+S.
// Routed to the same handlers as the menu items so the menu and the keyboard
// shortcut always agree, even when a modal is reinterpreting the action.
// ---------------------------------------------------------------------------
void
process_global_hotkeys()
{
    // Refresh the OS window title to reflect the current file + dirty mark.
    // HelloImGui reads this field every frame.
    std::string title = pipeline_display_name();
    if (g_state.pipeline_dirty)
    {
        title += " *";
    }
    title                                                      += " — cc-workbench";
    HelloImGui::GetRunnerParams()->appWindowParams.windowTitle  = title;

    // Skip hotkeys while an ImGui text input has active focus — Ctrl+S inside
    // an InputText is "save this buffer" semantically; we shouldn't hijack it.
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput)
    {
        return;
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))
    {
        request_action_with_unsaved_check(AppState::pending_action::new_pipeline);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O))
    {
        request_action_with_unsaved_check(AppState::pending_action::open_pipeline);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
    {
        do_save_or_save_as();
    }
}

// ---------------------------------------------------------------------------
// Modals
// ---------------------------------------------------------------------------
void
draw_load_error_modal()
{
    static bool should_open = false;
    if (g_state.load_error_open)
    {
        should_open             = true;
        g_state.load_error_open = false;
    }
    if (should_open)
    {
        ImGui::OpenPopup("Pipeline Error");
        should_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Pipeline Error", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextDisabled("Could not complete the operation:");
        ImGui::Spacing();
        // Wrap the message to the modal width.
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(g_state.load_error_text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        float bw = 120.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - bw + ImGui::GetCursorPosX());
        if (ImGui::Button("OK", ImVec2(bw, 0)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void
draw_load_warnings_modal()
{
    static bool should_open = false;
    if (g_state.load_warnings_open)
    {
        should_open                = true;
        g_state.load_warnings_open = false;
    }
    if (should_open)
    {
        ImGui::OpenPopup("Pipeline Loaded with Warnings");
        should_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Pipeline Loaded with Warnings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(g_state.load_warnings_text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        float bw = 120.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - bw + ImGui::GetCursorPosX());
        if (ImGui::Button("OK", ImVec2(bw, 0)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Three-button unsaved-changes confirm modal: Save / Don't Save / Cancel.
// Mirrors desktop-app behaviour — the user has three distinct intents:
//   Save        — write the current graph to disk, then proceed with the action
//   Don't Save  — discard changes, proceed with the action
//   Cancel      — abort the action, return to editing
void
draw_unsaved_confirm_modal()
{
    static bool should_open = false;
    if (g_state.unsaved_pending != AppState::pending_action::none)
    {
        // Only open when a real pending action exists AND a modal isn't already
        // showing (re-entry guard). We detect the latter by tracking the previous
        // state through the should_open latch.
        if (!should_open)
        {
            should_open = true;
        }
    }
    if (should_open && g_state.unsaved_pending != AppState::pending_action::none)
    {
        ImGui::OpenPopup("Unsaved Changes");
        should_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(
                "The current pipeline has unsaved changes.\n"
                "Save before proceeding?"
        );
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        float bw    = 110.0f;
        float total = bw * 3 + ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - total) * 0.5f);
        auto action = g_state.unsaved_pending;
        if (ImGui::Button("Save", ImVec2(bw, 0)))
        {
            g_state.unsaved_pending = AppState::pending_action::none;
            ImGui::CloseCurrentPopup();
            // If we already have a path, save in place; otherwise the Save as
            // dialog takes over and the user can cancel there without losing the
            // pending action — but the simplest correct behaviour is: save-as
            // opens, the user picks a path, and after successful save we re-fire
            // the original action.
            if (!g_state.pipeline_path.empty())
            {
                if (do_save_pipeline(g_state.pipeline_path))
                {
                    g_state.unsaved_pending = action;
                    execute_pending_action();
                }
            }
            else
            {
                // Defer the action until after Save as completes. We rely on the
                // Save-as poll handler not touching unsaved_pending, so it stays
                // = none here; we re-arm by remembering the action locally.
                // To keep the round-trip simple, we cancel: the user can press
                // New/Open again after Save-as finishes. Trade-off documented.
                request_save_as_dialog();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(bw, 0)))
        {
            ImGui::CloseCurrentPopup();
            execute_pending_action(); // pending is still set, dispatch + clear
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(bw, 0)))
        {
            g_state.unsaved_pending = AppState::pending_action::none;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void
draw_about_popup()
{
    // OpenPopup must be called from outside the modal popup itself, on the
    // same frame; we trigger it off the about_open flag set by the menu item.
    static bool should_open = false;
    if (g_state.about_open)
    {
        should_open        = true;
        g_state.about_open = false;
    }
    if (should_open)
    {
        ImGui::OpenPopup("About cc-workbench");
        should_open = false;
    }
    ImGui::SetNextWindowSize(ImVec2(360, 200), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("About cc-workbench", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextDisabled("cc-workbench");
        ImGui::Spacing();
        ImGui::Text("Version:   v%s", CC_VERSION_STRING);
        ImGui::Text("Plugins:   %zu loaded", g_state.loaded_plugins);
        ImGui::Text("Node types: %zu registered", g_state.host->node_factories().size());
        ImGui::Separator();
        ImGui::TextDisabled(
                "Node-graph platform host.\nBuilt with Dear ImGui, "
                "imgui-node-editor, AnyAny."
        );
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void
draw_status_bar()
{
    // Left: live counts.
    ImGui::Text("Plugins: %zu   Nodes: %zu   Edges: %zu", g_state.loaded_plugins, g_state.g.nodes().size(), g_state.g.edges().size());

    // Right: semver vMaj.Min.Rev.Patch (Blender-style).
    std::string version = "v" + std::string {CC_VERSION_STRING};
    float       avail   = ImGui::GetContentRegionAvail().x;
    float       text_w  = ImGui::CalcTextSize(version.c_str()).x;
    ImGui::SameLine(avail - text_w);
    ImGui::TextDisabled("%s", version.c_str());
}

} // namespace

int
main()
{
    g_state.host = cc::runtime::make_host_registry();
    g_state.host->renderers().register_renderer(std::make_unique<text_view_renderer>());
    g_state.host->renderers().register_renderer(std::make_unique<ir_view_renderer>());
    g_state.host->renderers().register_renderer(std::make_unique<ast_view_renderer>());
    g_state.host->renderers().register_renderer(std::make_unique<int_view_renderer>());
    g_state.host->renderers().register_renderer(std::make_unique<path_view_renderer>());

    std::size_t loaded     = g_state.loader.load_all(*g_state.host);
    g_state.loaded_plugins = loaded;
    log(std::string {"cc-workbench ready. plugins loaded: "} + std::to_string(loaded));
    log(std::string {"node types registered: "} + std::to_string(g_state.host->node_factories().size()));

    HelloImGui::RunnerParams params;
    params.appWindowParams.windowTitle         = "cc-workbench";
    params.appWindowParams.windowGeometry.size = {1480, 820};
    params.imGuiWindowParams.defaultImGuiWindowType =
            HelloImGui::DefaultImGuiWindowType::ProvideFullScreenDockSpace;

    // Main menu + status bar.
    params.imGuiWindowParams.showMenuBar    = true;
    params.imGuiWindowParams.showMenu_App   = false; // we provide our own File/Help
    params.imGuiWindowParams.showMenu_View  = false; // we provide our own View
    params.imGuiWindowParams.showStatusBar  = true;
    params.imGuiWindowParams.showStatus_Fps = false; // we put version there
    params.callbacks.ShowMenus              = [&docking = params.dockingParams]()
    {
        draw_main_menu(docking);
    };
    params.callbacks.ShowStatus = []()
    {
        draw_status_bar();
    };
    params.callbacks.ShowGui = []()
    {
        process_global_hotkeys();

        // Ensure at least one View tab exists.
        if (g_state.view_tabs.empty())
            open_view_tab(g_state.last_view_source);

        poll_view_tabs();

        draw_about_popup();
        draw_load_error_modal();
        draw_load_warnings_modal();
        draw_unsaved_confirm_modal();
    };

    // Bottom dock for View + Logger (~40%).
    {
        HelloImGui::DockingSplit split;
        split.initialDock = "MainDockSpace";
        split.newDock     = "BottomSpace";
        split.direction   = ImGuiDir_Down;
        split.ratio       = 0.40f;
        params.dockingParams.dockingSplits.push_back(split);
    }
    // Split bottom horizontally: View left, Logger right.
    {
        HelloImGui::DockingSplit split;
        split.initialDock = "BottomSpace";
        split.newDock     = "LoggerSpace";
        split.direction   = ImGuiDir_Right;
        split.ratio       = 0.45f;
        params.dockingParams.dockingSplits.push_back(split);
    }

    params.callbacks.PostInit = []()
    {
        ed::Config cfg;
        cfg.SettingsFile = "";
        g_editor         = ed::CreateEditor(&cfg);
        ed::SetCurrentEditor(g_editor);

        auto &s                                = ed::GetStyle();
        s.NodeRounding                         = 8.0f;
        s.PinRounding                          = 6.0f;
        s.NodePadding                          = ImVec4(8, 4, 8, 4);
        s.Colors[ed::StyleColor_Bg]            = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        s.Colors[ed::StyleColor_NodeBg]        = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
        s.Colors[ed::StyleColor_NodeBorder]    = ImVec4(0.30f, 0.30f, 0.36f, 0.80f);
        s.Colors[ed::StyleColor_HovNodeBorder] = ImVec4(0.55f, 0.55f, 0.62f, 1.00f);
        s.Colors[ed::StyleColor_SelNodeBorder] = ImVec4(0.95f, 0.75f, 0.30f, 1.00f);

        ImFileDialogSetupTextureLoader();
    };
    params.callbacks.BeforeExit = []()
    {
        if (g_editor)
        {
            ed::DestroyEditor(g_editor);
            g_editor = nullptr;
        }
    };

    params.callbacks.LoadAdditionalFonts = []()
    {
        ImGuiIO                      &io = ImGui::GetIO();
        HelloImGui::FontLoadingParams ui;
        ui.fontConfig.GlyphRanges = io.Fonts->GetGlyphRangesCyrillic();
        ui.fontConfig.RasterizerDensity = 2.0f;
        g_state.ui_font           = HelloImGui::LoadFont("fonts/UI-Regular.ttf", 20.0f, ui);

        if (HelloImGui::AssetExists("fonts/NotoColorEmoji.ttf"))
        {
#if defined(IMGUI_USE_WCHAR32)
            static const ImWchar          emoji_ranges[] = {0x1F600, 0x1F64F, 0x2764, 0x2764, 0, 0};
            HelloImGui::FontLoadingParams em;
            em.mergeToLastFont        = true;
            em.loadColor              = true;
            em.fontConfig.GlyphRanges = emoji_ranges;
            HelloImGui::LoadFont("fonts/NotoColorEmoji.ttf", 20.0f, em);
#endif
        }

        if (HelloImGui::AssetExists("fonts/IBMPlexMono-Regular.ttf"))
        {
            HelloImGui::FontLoadingParams mono;
            mono.fontConfig.GlyphRanges = io.Fonts->GetGlyphRangesCyrillic();
            mono.fontConfig.RasterizerDensity = 2.0f;
            g_state.mono_font           = HelloImGui::LoadFont("fonts/IBMPlexMono-Regular.ttf", 16.0f, mono);
        }
    };

    // --- Pipeline window ---
    {
        HelloImGui::DockableWindow w;
        w.label             = "Pipeline";
        w.dockSpaceName     = "MainDockSpace";
        w.canBeClosed       = false;
        w.includeInViewMenu = false;
        w.GuiFunction       = []()
        {
            poll_file_dialog();
            draw_pipeline_canvas();
        };
        params.dockingParams.dockableWindows.push_back(w);
    }

    // --- Logger window ---
    {
        HelloImGui::DockableWindow w;
        w.label             = "Logger";
        w.dockSpaceName     = "LoggerSpace";
        w.canBeClosed       = false;
        w.includeInViewMenu = false;
        w.GuiFunction       = []()
        {
            g_log.render();
        };
        params.dockingParams.dockableWindows.push_back(w);
    }

    HelloImGui::Run(params);
    return 0;
}
