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

#include <cc/astit.hpp>  // cc::ast::tl_program, visitor for AST view renderer
#include <cc/ir.hpp>     // cc::ir::module for IR view renderer

#include "hello_imgui/hello_imgui.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "imgui_stacklayout.h"          // ImGui::Spring
#include "ImFileDialog/ImFileDialog.h"
#include "bundle_integration/ImFileDialogTextureHelper.h"
#include "ImGuiColorTextEdit/TextEditor.h"
#include "ne/builders.h"
#include "ne/widgets.h"

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

namespace {

#ifdef __linux__
#include <unistd.h>
std::string exe_dir() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf));
  if (n > 0) {
    std::string p(buf, static_cast<size_t>(n));
    auto slash = p.find_last_of('/');
    if (slash != std::string::npos) return p.substr(0, slash);
  }
  return ".";
}
#else
std::string exe_dir() { return "."; }
#endif

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
auto stable_link_id(const cc::runtime::edge& e) -> int {
  auto h = std::hash<std::string>{}(e.src_node);
  h ^= std::hash<std::string>{}(e.src_slot) + 0x9e3779b9 + (h << 6) + (h >> 2);
  h ^= std::hash<std::string>{}(e.dst_node) + 0x9e3779b9 + (h << 6) + (h >> 2);
  h ^= std::hash<std::string>{}(e.dst_slot) + 0x9e3779b9 + (h << 6) + (h >> 2);
  return static_cast<int>(h & 0x7FFFFFFF);
}

auto hash_color(std::string_view s) -> ImVec4 {
  // Spread hues around the wheel; keep S/L fixed for visual consistency.
  uint32_t h = 2166136261u;
  for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
  float hue = (h % 360) / 360.0f;
  ImColor c;
  c.SetHSV(hue, 0.55f, 0.85f);
  return ImVec4(c);
}

auto pin_color_for_type(std::string_view type_name) -> ImVec4 {
  static const std::unordered_map<std::string_view, ImVec4> known = {
    {"text",     ImVec4(0.35f, 0.60f, 0.95f, 1.0f)},
    {"path",     ImVec4(0.95f, 0.78f, 0.30f, 1.0f)},  // filesystem path — gold
    {"int",      ImVec4(0.25f, 0.85f, 0.85f, 1.0f)},  // integer return code — cyan
    {"ast",      ImVec4(0.62f, 0.40f, 0.78f, 1.0f)},
    {"ast.tl",   ImVec4(0.62f, 0.40f, 0.78f, 1.0f)},
    {"ir",       ImVec4(0.35f, 0.80f, 0.45f, 1.0f)},
    {"ir.module",ImVec4(0.35f, 0.80f, 0.45f, 1.0f)},
    {"tl.ast",   ImVec4(0.62f, 0.40f, 0.78f, 1.0f)},
    {"bytes",    ImVec4(0.95f, 0.60f, 0.25f, 1.0f)},
    {"any",      ImVec4(0.55f, 0.55f, 0.55f, 1.0f)},
    {"",         ImVec4(0.55f, 0.55f, 0.55f, 1.0f)},  // wildcard / unset
  };
  if (auto it = known.find(type_name); it != known.end()) return it->second;
  return hash_color(type_name);
}

auto icon_for_type(std::string_view type_name) -> IconType {
  if (type_name == "text")  return IconType::Circle;
  if (type_name == "path")  return IconType::Diamond;
  if (type_name == "int")   return IconType::Circle;
  if (type_name == "bytes") return IconType::Diamond;
  if (type_name == "any" || type_name.empty()) return IconType::Grid;
  return IconType::Square;  // structured (ast/ir/...)
}

auto header_color_for_category(std::string_view category) -> ImVec4 {
  static const std::unordered_map<std::string_view, ImVec4> known = {
    {"Basic",   ImVec4(0.15f, 0.45f, 0.60f, 1.0f)},
    {"TL",      ImVec4(0.45f, 0.25f, 0.62f, 1.0f)},
    {"Backend", ImVec4(0.64f, 0.26f, 0.26f, 1.0f)},
    {"I/O",     ImVec4(0.20f, 0.55f, 0.30f, 1.0f)},
  };
  if (auto it = known.find(category); it != known.end()) return it->second;
  return hash_color(category);
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
struct AppState {
  std::unique_ptr<cc::host_registry> host;
  cc::runtime::plugin_loader loader;
  cc::runtime::graph         g;

  // imgui-node-editor uses int ids; runtime uses string instance_ids.
  int next_editor_id = 1;
  std::unordered_map<int, std::string>  ed2inst;
  std::unordered_map<std::string, int>  inst2ed;

  // File-dialog target (which node + property key is being picked).
  struct {
    std::string instance;
    std::string key;
  } file_dialog_target;

  // View tab selection (instance_id of the view node being shown).
  std::string view_selected;

  // Pending SetNodePosition requests — collected when a node is created
  // outside ed::Begin/End (e.g. from the context menu), applied next frame
  // inside the editor context where ScreenToCanvas works.
  struct pending_position { int ed_id; ImVec2 canvas_pos; };
  std::vector<pending_position> pending_positions;

  // Status / menu state.
  std::size_t loaded_plugins      = 0;
  bool        about_open          = false;
  bool        canvas_navigate_content = false;

  // View tab cache. Pulling is expensive (Exec spawns a child, Assemble runs
  // nasm+ld, ...) and doing it synchronously in the UI thread would freeze
  // the host. Instead, pull runs in a background std::async; the UI polls
  // its status every frame and renders whatever the cache last held.
  std::string                  view_cached_for;
  std::optional<cc::any_value> view_cached_value;
  std::string                  view_cached_error;
  std::string                  view_cached_type_name;
  bool                         view_cache_stale     = true;   // invalidation flag
  std::chrono::steady_clock::time_point view_stale_since = {};  // for debounce
  // Background pull state.
  std::future<std::pair<std::string, std::optional<cc::any_value>>> view_pull_future;
  std::string                  view_pull_target;               // node being pulled
  bool                         view_pull_running   = false;

  // Fonts
  ImFont* ui_font   = nullptr;
  ImFont* mono_font = nullptr;
};

class noop_view_context final : public cc::view_context {};

// Forward decl: log_view::render uses g_state.mono_font, but g_state is
// declared after log_view for order-of-init reasons.
extern AppState g_state;

// Read-only TextEditor backing the Logger window. Plain-text palette (no
// language definition); gives free selection + Ctrl+C.
// On update: preserves scroll unless the user was at the bottom (sticky).
class log_view final {
 public:
  log_view() {
    editor_.SetPalette(TextEditor::GetDarkPalette());
    editor_.SetReadOnlyEnabled(true);
  }
  // Thread-safe append — activate() runs on a worker thread (async pull).
  void append(std::string_view line) {
    std::lock_guard lock(mutex_);
    buffer_.append(line);
    buffer_.append("\n");
    dirty_ = true;
  }
  // Render is called only from the UI thread.
  void render() {
    {
      std::lock_guard lock(mutex_);
      if (dirty_) {
        // SetText resets scroll. If user was near the bottom, stick to bottom
        // (usual terminal behaviour); otherwise restore the first visible line.
        size_t first  = editor_.GetFirstVisibleRow();
        size_t total  = editor_.GetLineCount();
        bool   at_end = (first + 3 >= total) || (total < 5);

        editor_.SetText(buffer_);
        size_t new_total = editor_.GetLineCount();
        if (at_end && new_total > 0) {
          editor_.ScrollToLine(new_total, TextEditor::Scroll::alignBottom);
        } else if (first < new_total) {
          editor_.ScrollToLine(first, TextEditor::Scroll::alignTop);
        }
        dirty_ = false;
      }
    }
    if (g_state.mono_font) ImGui::PushFont(g_state.mono_font);
    editor_.Render("##log_text", ImVec2(0, 0), false);
    if (g_state.mono_font) ImGui::PopFont();
  }
 private:
  TextEditor    editor_;
  std::string   buffer_;
  bool          dirty_ = false;
  std::mutex    mutex_;
};

AppState g_state;
log_view g_log;
ed::EditorContext* g_editor = nullptr;
noop_view_context g_view_ctx;

// Drop the View-tab cache after any graph mutation. Cheap; called from every
// node/edge/property change site. Marks cache stale and stamps the time so
// the View tab can debounce auto-pull (avoid storming pulls on every keystroke
// in a multiline property editor).
inline void invalidate_view_cache() {
  g_state.view_cache_stale  = true;
  g_state.view_stale_since  = std::chrono::steady_clock::now();
}

void log(std::string msg) {
  g_log.append(msg);
}

auto editor_id_for(const std::string& instance_id) -> int {
  if (auto it = g_state.inst2ed.find(instance_id); it != g_state.inst2ed.end()) {
    return it->second;
  }
  int id = g_state.next_editor_id++;
  g_state.inst2ed[instance_id] = id;
  g_state.ed2inst[id] = instance_id;
  return id;
}

// ---------------------------------------------------------------------------
// Host-side view renderers (must live in the host to call ImGui directly
// without dragging ImGui into plugin .so)
// ---------------------------------------------------------------------------
class text_view_renderer final : public cc::view_renderer {
 public:
  text_view_renderer() {
    editor_.SetPalette(TextEditor::GetDarkPalette());
    editor_.SetLanguage(TextEditor::Language::Cpp());  // .tl is C-like
    editor_.SetReadOnlyEnabled(true);
  }

  auto type_name() const -> std::string_view override { return "text"; }
  auto render(const cc::any_value& value, cc::view_context&) -> void override {
    const auto* s = aa::any_cast<std::string>(&value);
    if (!s) {
      ImGui::TextDisabled("view: value is not text");
      return;
    }
    // Only push the text into the editor when it actually changes — preserves
    // scroll position / cursor across frames.
    if (*s != last_content_) {
      editor_.SetText(*s);
      last_content_ = *s;
    }
    if (g_state.mono_font) ImGui::PushFont(g_state.mono_font);
    editor_.Render("##view_text", ImVec2(0, 0), false);
    if (g_state.mono_font) ImGui::PopFont();
  }

 private:
  TextEditor  editor_;
  std::string last_content_;
};

// ---------------------------------------------------------------------------
// IR view renderer — renders cc::ir::module as a textual instruction listing.
// ---------------------------------------------------------------------------
class ir_view_renderer final : public cc::view_renderer {
 public:
  ir_view_renderer() {
    editor_.SetPalette(TextEditor::GetDarkPalette());
    editor_.SetLanguage(TextEditor::Language::Cpp());
    editor_.SetReadOnlyEnabled(true);
  }

  auto type_name() const -> std::string_view override { return "ir.module"; }

  auto render(const cc::any_value& value, cc::view_context&) -> void override {
    const auto* mod = aa::any_cast<cc::ir::module>(&value);
    if (!mod) {
      ImGui::TextDisabled("view: value is not ir.module");
      return;
    }
    std::string text;
    for (auto const& instr : mod->code) {
      switch (instr.op) {
        case cc::ir::opcode::ret:
          text += "ret " + std::to_string(instr.imm) + "\n";
          break;
      }
    }
    if (text.empty()) text = "(empty module)";
    if (text != last_content_) {
      editor_.SetText(text);
      last_content_ = text;
    }
    if (g_state.mono_font) ImGui::PushFont(g_state.mono_font);
    editor_.Render("##view_ir", ImVec2(0, 0), false);
    if (g_state.mono_font) ImGui::PopFont();
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

namespace ast_view_detail {

// Builds a textual representation of the AST using the visitor double-dispatch.
class stringifier final : public cc::ast::visitor {
 public:
  std::string text;

  void visit(const cc::ast::program& p) override {
    emit("program");
    ++depth_;
    for (auto const& s : p.body) {
      if (s) s->accept(*this);
    }
    --depth_;
  }
  void visit(const cc::ast::return_stmt& r) override {
    emit("return");
    ++depth_;
    if (r.value) r.value->accept(*this);
    --depth_;
  }
  void visit(const cc::ast::int_literal& i) override {
    emit("int " + std::to_string(i.value));
  }

 private:
  void emit(std::string_view line) {
    if (depth_ > 0) text += std::string(static_cast<size_t>(depth_) * 2, ' ');
    text.append(line.data(), line.size());
    text += '\n';
  }
  int depth_ = 0;
};

}  // namespace ast_view_detail

class ast_view_renderer final : public cc::view_renderer {
 public:
  ast_view_renderer() {
    editor_.SetPalette(TextEditor::GetDarkPalette());
    editor_.SetLanguage(TextEditor::Language::Cpp());
    editor_.SetReadOnlyEnabled(true);
  }

  auto type_name() const -> std::string_view override { return "tl.ast"; }

  auto render(const cc::any_value& value, cc::view_context&) -> void override {
    const auto* ast = aa::any_cast<ast_value>(&value);
    if (!ast || !*ast || !(*ast)->root) {
      ImGui::TextDisabled("view: value is not tl.ast (or empty)");
      return;
    }
    ast_view_detail::stringifier s;
    (*ast)->root->accept(s);
    if (s.text != last_content_) {
      editor_.SetText(s.text);
      last_content_ = s.text;
    }
    if (g_state.mono_font) ImGui::PushFont(g_state.mono_font);
    editor_.Render("##view_ast", ImVec2(0, 0), false);
    if (g_state.mono_font) ImGui::PopFont();
  }

 private:
  TextEditor  editor_;
  std::string last_content_;
};

// ---------------------------------------------------------------------------
// int view renderer — shows a long value (typically exec's ret_code) as a
// readable "Exit code: N (0xHEX)" line + signed/unsigned interpretation.
// ---------------------------------------------------------------------------
class int_view_renderer final : public cc::view_renderer {
 public:
  auto type_name() const -> std::string_view override { return "int"; }

  auto render(const cc::any_value& value, cc::view_context&) -> void override {
    const auto* p = aa::any_cast<long>(&value);
    if (!p) {
      ImGui::TextDisabled("view: value is not int (long)");
      return;
    }
    long v = *p;
    unsigned long uv = static_cast<unsigned long>(v);

    ImGui::PushFont(g_state.mono_font);
    ImGui::TextDisabled("Exit code");
    ImGui::SameLine();
    if (v == 0) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.85f, 0.45f, 1.0f));
      ImGui::Text("%ld  (0x%lX)  ✓ success", v, uv);
    } else {
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
class path_view_renderer final : public cc::view_renderer {
 public:
  auto type_name() const -> std::string_view override { return "path"; }

  auto render(const cc::any_value& value, cc::view_context&) -> void override {
    const auto* p = aa::any_cast<std::filesystem::path>(&value);
    if (!p) {
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
    if (ImGui::SmallButton("Copy")) {
      ImGui::SetClipboardText(s.c_str());
    }
    ImGui::SameLine();
    if (std::filesystem::exists(*p)) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.85f, 0.45f, 1.0f));
      ImGui::TextDisabled("(exists)");
      ImGui::PopStyleColor();
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.40f, 1.0f));
      ImGui::TextDisabled("(missing)");
      ImGui::PopStyleColor();
    }
  }
};

// ---------------------------------------------------------------------------
// File-dialog poll
// ---------------------------------------------------------------------------
void poll_file_dialog() {
  constexpr const char* kDlg = "node_path";
  if (!ifd::FileDialog::Instance().IsDone(kDlg)) return;
  if (ifd::FileDialog::Instance().HasResult()) {
    std::string path = ifd::FileDialog::Instance().GetResult().string();
    if (!g_state.file_dialog_target.instance.empty()) {
      auto* n = g_state.g.find_node(g_state.file_dialog_target.instance);
      if (n) {
        n->properties().set(g_state.file_dialog_target.key, path);
        invalidate_view_cache();
        log("set " + g_state.file_dialog_target.key + " = " + path +
            " on " + g_state.file_dialog_target.instance);
      }
    }
  }
  ifd::FileDialog::Instance().Close();
}

// ---------------------------------------------------------------------------
// Schema-driven property widgets
// ---------------------------------------------------------------------------
void draw_property_widget(cc::node& n, const cc::property_desc& desc) {
  ImGui::PushID(desc.key.data());
  std::string current{n.properties().get(desc.key)};

  switch (desc.kind) {
    case cc::property_kind::path: {
      char buf[512];
      std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = 0;
      ImGui::TextUnformatted(desc.display_name.data());
      ImGui::SameLine();
      ImGui::SetNextItemWidth(180);
      if (ImGui::InputText("##v", buf, sizeof(buf))) {
        n.properties().set(desc.key, buf);
        invalidate_view_cache();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("...")) {
        g_state.file_dialog_target.instance = std::string{n.instance_id()};
        g_state.file_dialog_target.key      = std::string{desc.key};
        ifd::FileDialog::Instance().Open("node_path", "Open File", ".*", false,
                                         exe_dir() + "/assets");
      }
      break;
    }
    case cc::property_kind::multiline: {
      ImGui::TextUnformatted(desc.display_name.data());
      char buf[4096];
      std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = 0;
      if (ImGui::InputTextMultiline("##v", buf, sizeof(buf), ImVec2(220, 80))) {
        n.properties().set(desc.key, buf);
        invalidate_view_cache();
      }
      break;
    }
    case cc::property_kind::integer: {
      ImGui::TextUnformatted(desc.display_name.data());
      ImGui::SameLine();
      int v = 0;
      try { v = std::stoi(current); } catch (...) {}
      ImGui::SetNextItemWidth(120);
      if (ImGui::InputInt("##v", &v, 1, 100)) {
        n.properties().set(desc.key, std::to_string(v));
        invalidate_view_cache();
      }
      break;
    }
    case cc::property_kind::boolean: {
      bool v = (current == "1" || current == "true");
      if (ImGui::Checkbox(desc.display_name.data(), &v)) {
        n.properties().set(desc.key, v ? "1" : "0");
        invalidate_view_cache();
      }
      break;
    }
    case cc::property_kind::text:
    default: {
      ImGui::TextUnformatted(desc.display_name.data());
      ImGui::SameLine();
      char buf[256];
      std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = 0;
      ImGui::SetNextItemWidth(180);
      if (ImGui::InputText("##v", buf, sizeof(buf))) {
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
struct create_menu_state {
  bool   open = false;
  ImVec2 pos{};         // absolute screen coords — for SetNextWindowPos
  ImVec2 canvas_pos{};  // canvas-local coords — for ed::SetNodePosition
};
create_menu_state g_create_menu;

// Palette that opens when the user drags a link from a pin and releases in
// empty canvas space. Filters node factories to those that have at least one
// slot whose type is connectable to the dragged pin's type.
struct palette_drop_state {
  bool                 open          = false;
  ImVec2               canvas_pos{};      // where to place the new node
  ImVec2               screen_pos{};      // for SetNextWindowPos
  cc::type_descriptor_t src_type{};       // type of the dragged pin
  bool                 src_is_output = false;
  std::string          src_node;
  std::string          src_slot;
};
palette_drop_state g_palette;

void draw_create_menu() {
  if (!g_create_menu.open) return;

  ImGui::SetNextWindowPos(g_create_menu.pos);
  ImGui::SetNextWindowSize(ImVec2(240, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg,        ImVec4(0.13f, 0.14f, 0.17f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg,         ImVec4(0.13f, 0.14f, 0.17f, 0.98f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6, 4));

  constexpr int kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoDocking;

  bool opened = ImGui::Begin("##create_menu", nullptr, kFlags);

  // Close on Escape or click outside any window (background click).
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    g_create_menu.open = false;
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
    g_create_menu.open = false;
  }

  if (opened && g_create_menu.open) {
    ImGui::SeparatorText("Create Node");

    // Group factories by category (alphabetical).
    std::map<std::string, std::vector<cc::node_factory*>> by_category;
    for (auto* f : g_state.host->node_factories()) {
      by_category[std::string{f->category()}].push_back(f);
    }
    if (by_category.empty()) {
      ImGui::TextDisabled("(no plugins loaded)");
    }

    for (auto& [category, factories] : by_category) {
      ImVec4 dot = header_color_for_category(category);
      ImGui::PushStyleColor(ImGuiCol_Text, dot);
      bool expanded = ImGui::TreeNodeEx(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
      ImGui::PopStyleColor();
      if (!expanded) continue;

      for (auto* f : factories) {
        std::string label{f->display_name()};
        label += "##";
        label += std::string{f->type_id()};
        if (ImGui::Selectable(label.c_str())) {
          auto node = f->create();
          std::string instance{node->instance_id()};
          int ed_id = editor_id_for(instance);
          g_state.g.add_node(std::move(node));
          invalidate_view_cache();
          // Position is applied next frame inside ed::Begin/End —
          // ed::SetNodePosition takes canvas-local coords.
          g_state.pending_positions.push_back({ed_id, g_create_menu.canvas_pos});
          log("created " + std::string{f->type_id()} + " (instance=" + instance + ")");
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
void draw_palette_drop() {
  if (!g_palette.open) return;

  ImGui::SetNextWindowPos(g_palette.screen_pos);
  ImGui::SetNextWindowSize(ImVec2(260, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg,        ImVec4(0.13f, 0.14f, 0.17f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg,         ImVec4(0.13f, 0.14f, 0.17f, 0.98f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6, 4));

  constexpr int kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoDocking;

  bool opened = ImGui::Begin("##palette_drop", nullptr, kFlags);

  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    g_palette.open = false;
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
    g_palette.open = false;
  }

  if (opened && g_palette.open) {
    const char* direction = g_palette.src_is_output ? "input" : "output";
    ImGui::SeparatorText("Connect to new node");
    ImGui::TextDisabled("looking for %s pin of compatible type", direction);
    ImGui::Spacing();

    // For each factory, create a transient sample to introspect its slots.
    // If at least one slot is type-compatible with the dragged pin, list it.
    auto type_name = g_state.host->types().name_of(g_palette.src_type);
    if (!type_name.empty()) {
      ImGui::TextDisabled("source type: %.*s",
                          static_cast<int>(type_name.size()), type_name.data());
      ImGui::Spacing();
    }

    std::map<std::string, std::vector<cc::node_factory*>> by_category;
    for (auto* f : g_state.host->node_factories()) {
      by_category[std::string{f->category()}].push_back(f);
    }

    bool any_compatible = false;

    for (auto& [category, factories] : by_category) {
      // Gather compatible factories for this category first so we can skip
      // the entire category header if none match.
      std::vector<cc::node_factory*> compatible;
      for (auto* f : factories) {
        auto sample = f->create();
        if (!sample) continue;
        for (auto* s : sample->slots()) {
          if (g_palette.src_is_output) {
            // source was an output → need an input on the new node
            if (s->dir() == cc::slot_dir::in &&
                g_state.host->types().is_connectable(g_palette.src_type, s->type())) {
              compatible.push_back(f);
              break;
            }
          } else {
            // source was an input → need an output on the new node
            if (s->dir() == cc::slot_dir::out &&
                g_state.host->types().is_connectable(s->type(), g_palette.src_type)) {
              compatible.push_back(f);
              break;
            }
          }
        }
      }
      if (compatible.empty()) continue;
      any_compatible = true;

      ImVec4 dot = header_color_for_category(category);
      ImGui::PushStyleColor(ImGuiCol_Text, dot);
      bool expanded = ImGui::TreeNodeEx(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
      ImGui::PopStyleColor();
      if (!expanded) continue;

      for (auto* f : compatible) {
        std::string label{f->display_name()};
        label += "##";
        label += std::string{f->type_id()};
        if (ImGui::Selectable(label.c_str())) {
          // Instantiate and find the first compatible slot to wire.
          auto new_node = f->create();
          std::string new_id{new_node->instance_id()};
          int ed_id = editor_id_for(new_id);

          std::string matched_slot;
          for (auto* s : new_node->slots()) {
            if (g_palette.src_is_output) {
              if (s->dir() == cc::slot_dir::in &&
                  g_state.host->types().is_connectable(g_palette.src_type, s->type())) {
                matched_slot = std::string{s->id()};
                break;
              }
            } else {
              if (s->dir() == cc::slot_dir::out &&
                  g_state.host->types().is_connectable(s->type(), g_palette.src_type)) {
                matched_slot = std::string{s->id()};
                break;
              }
            }
          }

          g_state.g.add_node(std::move(new_node));
          g_state.pending_positions.push_back({ed_id, g_palette.canvas_pos});

          if (!matched_slot.empty()) {
            cc::runtime::edge e = g_palette.src_is_output
                ? cc::runtime::edge{g_palette.src_node, g_palette.src_slot,
                                     new_id,             matched_slot}
                : cc::runtime::edge{new_id,             matched_slot,
                                     g_palette.src_node, g_palette.src_slot};
            g_state.g.add_edge(std::move(e));
            log("palette: created " + std::string{f->type_id()} +
                " and linked " + g_palette.src_slot + " ↔ " + matched_slot);
          } else {
            log("palette: created " + std::string{f->type_id()} + " (no auto-link)");
          }
          invalidate_view_cache();
          g_palette.open = false;
        }
      }
      ImGui::TreePop();
    }

    if (!any_compatible) {
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
void draw_node(cc::node& n) {
  const int ed_id = editor_id_for(std::string{n.instance_id()});
  const auto tid  = std::string{n.type_id()};
  const auto* factory = g_state.host->find_node_factory(tid);
  const std::string display_name =
      factory ? std::string{factory->display_name()} : tid;
  const std::string category =
      factory ? std::string{factory->category()} : std::string{};
  const ImVec4 header_color = header_color_for_category(category);

  util::BlueprintNodeBuilder b;
  b.Begin(ed_id);

  // ---- Header: coloured band with display name ----
  b.Header(header_color);
  ImGui::TextUnformatted(display_name.c_str());
  ImGui::Spring(1);
  ImGui::TextDisabled("%s", tid.c_str());
  b.EndHeader();

  // ---- Input pins (left side) ----
  // Collect outputs to render after Middle; BlueprintNodeBuilder requires
  // the order Header → Inputs → Middle → Outputs → End.
  struct out_pin_t { int id; const cc::slot* slot; };
  std::vector<out_pin_t> outputs;

  int slot_idx = 0;
  for (auto* slot : n.slots()) {
    int pin_id = ed_id * 64 + slot_idx++;
    if (slot->dir() == cc::slot_dir::out) {
      outputs.push_back({pin_id, slot});
      continue;
    }
    auto type_name = g_state.host->types().name_of(slot->type());
    if (type_name.empty()) type_name = "any";  // wildcard slots
    ImVec4 pin_color = pin_color_for_type(type_name);
    IconType pin_icon = icon_for_type(type_name);

    b.Input(pin_id);
    Icon(ImVec2(20, 20), pin_icon, true, pin_color, ImVec4(0, 0, 0, 0));
    ImGui::Spring(0);
    ImGui::TextUnformatted(slot->id().data());
    ImGui::Spring(0);
    ImGui::TextDisabled("%.*s", static_cast<int>(type_name.size()),
                        type_name.data());
    b.EndInput();
  }

  // ---- Middle: property editor (under input pins) ----
  b.Middle();
  if (factory) {
    for (auto const& desc : factory->property_schema()) {
      draw_property_widget(n, desc);
    }
  }

  // ---- Output pins (right side) ----
  for (auto const& op : outputs) {
    auto type_name = g_state.host->types().name_of(op.slot->type());
    if (type_name.empty()) type_name = "any";
    ImVec4 pin_color = pin_color_for_type(type_name);
    IconType pin_icon = icon_for_type(type_name);

    b.Output(op.id);
    ImGui::TextDisabled("%.*s", static_cast<int>(type_name.size()),
                        type_name.data());
    ImGui::Spring(0);
    ImGui::TextUnformatted(op.slot->id().data());
    ImGui::Spring(0);
    Icon(ImVec2(20, 20), pin_icon, true, pin_color, ImVec4(0, 0, 0, 0));
    b.EndOutput();
  }

  b.End();
}

// Drop every node and edge from the current graph. Canvas-scoped, invoked
// from the Pipeline tab toolbar.
void clear_graph() {
  std::vector<std::string> ids;
  ids.reserve(g_state.g.nodes().size());
  for (auto const& n : g_state.g.nodes()) ids.emplace_back(n->instance_id());
  for (auto const& id : ids) {
    g_state.g.remove_edges_of(id);
    g_state.g.remove_node(id);
  }
  g_state.inst2ed.clear();
  g_state.ed2inst.clear();
  g_state.view_selected.clear();
  invalidate_view_cache();
  log("graph cleared");
}

// Run the pipeline end-to-end: find an x86_64.assemble node, pull its "exe"
// output (which transitively activates every upstream node), then chmod the
// resulting path so it is directly executable.
void run_pipeline() {
  std::string target;
  for (auto const& n : g_state.g.nodes()) {
    if (n->type_id() == "x86_64.assemble") {
      target = n->instance_id();
      break;
    }
  }
  if (target.empty()) {
    log("run: no x86_64.assemble node in graph");
    return;
  }

  log("run: pulling output 'exe' of " + target + " ...");
  cc::runtime::runner r{g_state.g, [](std::string_view msg) {
    ::log(std::string{msg});
  }};
  auto result = r.pull(target, "exe");
  if (!result) {
    log("run failed: " + result.error().what);
    return;
  }
  const cc::any_value* out = *result;
  if (!out || !out->has_value()) {
    log("run: producer returned empty value");
    return;
  }
  const auto* exe_path = aa::any_cast<std::filesystem::path>(out);
  if (!exe_path) {
    log("run: 'exe' output is not a path");
    return;
  }
  const std::string exe_str = exe_path->string();
  log("run: built " + exe_str);
  if (std::system(("chmod +x " + exe_str).c_str()) == 0) {
    log("run: chmod +x ok — binary ready at " + exe_str);
  } else {
    log("run: warning, chmod returned non-zero");
  }
}

void draw_pipeline_canvas() {
  // ---- Canvas toolbar (per-tab actions) ----
  // These commands mutate the graph or the editor view — they belong to the
  // Pipeline tab, not the global menu. Navigate-style buttons set a flag
  // consumed inside ed::Begin/End below, where an editor context is current.
  const bool busy = g_state.view_pull_running;  // freeze UI mutations during pull
  if (busy) ImGui::BeginDisabled();
  if (ImGui::Button("Run"))           run_pipeline();
  ImGui::SameLine();
  if (ImGui::Button("Zoom to Fit"))   g_state.canvas_navigate_content = true;
  ImGui::SameLine();
  if (ImGui::Button("Clear"))         clear_graph();
  if (busy) ImGui::EndDisabled();
  ImGui::SameLine();
  if (busy) {
    ImGui::TextDisabled("|  COMPUTING: %s ...",
                        g_state.view_pull_target.c_str());
  } else {
    ImGui::TextDisabled("|  nodes: %zu   edges: %zu",
                        g_state.g.nodes().size(), g_state.g.edges().size());
  }
  ImGui::Separator();

  ed::SetCurrentEditor(g_editor);
  ed::Begin("Pipeline Graph", ImVec2(0, ImGui::GetContentRegionAvail().y));

  // Apply deferred SetNodePosition requests from context-menu creations.
  // ed::SetNodePosition takes canvas-local coords (same coordinate space as
  // ed::ScreenToCanvas output), and requires a current editor context.
  if (!g_state.pending_positions.empty()) {
    for (auto const& pp : g_state.pending_positions) {
      ed::SetNodePosition(pp.ed_id, pp.canvas_pos);
    }
    g_state.pending_positions.clear();
  }

  if (g_state.canvas_navigate_content) {
    // Defer until after nodes/links are rendered this frame, so the editor
    // has up-to-date bounds to fit. Applied below, just before ed::End().
  }

  for (auto const& node_ptr : g_state.g.nodes()) {
    draw_node(*node_ptr);
  }

  // Render links from graph edges. Each edge gets a stable hash-based link id
  // so delete handling can match a deleted LinkId back to its edge.
  for (auto const& e : g_state.g.edges()) {
    auto* src_n = g_state.g.find_node(e.src_node);
    auto* dst_n = g_state.g.find_node(e.dst_node);
    if (!src_n || !dst_n) continue;
    int src_ed = editor_id_for(e.src_node);
    int dst_ed = editor_id_for(e.dst_node);
    int src_pin = -1, dst_pin = -1, idx = 0;
    for (auto* s : src_n->slots()) {
      if (s->dir() == cc::slot_dir::out && s->id() == e.src_slot) {
        src_pin = src_ed * 64 + idx;
        break;
      }
      ++idx;
    }
    idx = 0;
    for (auto* s : dst_n->slots()) {
      if (s->dir() == cc::slot_dir::in && s->id() == e.dst_slot) {
        dst_pin = dst_ed * 64 + idx;
        break;
      }
      ++idx;
    }
    if (src_pin < 0 || dst_pin < 0) continue;
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
  if (ed::BeginCreate(ImColor(120, 160, 220, 200), 2.0f)) {
    ed::PinId a, b;
    if (ed::QueryNewLink(&a, &b)) {
      if (ed::AcceptNewItem()) {
        pending_new_a = a;
        pending_new_b = b;
        pending_new   = true;
      }
    } else if (ed::QueryNewNode(&a)) {
      if (ed::AcceptNewItem()) {
        pending_drop_pin = a;
        pending_drop     = true;
      }
    }
    ed::EndCreate();
  }
  if (pending_new) {
    auto decode = [](ed::PinId p) -> std::pair<int, int> {
      int v = static_cast<int>(reinterpret_cast<intptr_t>(p.AsPointer()));
      return {v / 64, v % 64};
    };
    auto [a_ed, a_idx] = decode(pending_new_a);
    auto [b_ed, b_idx] = decode(pending_new_b);
    auto ia = g_state.ed2inst.find(a_ed);
    auto ib = g_state.ed2inst.find(b_ed);
    if (ia != g_state.ed2inst.end() && ib != g_state.ed2inst.end()) {
      auto* na = g_state.g.find_node(ia->second);
      auto* nb = g_state.g.find_node(ib->second);
      auto slot_at = [](cc::node* n, int idx) -> const cc::slot* {
        int i = 0;
        for (auto* s : n->slots()) { if (i == idx) return s; ++i; }
        return nullptr;
      };
      auto* sa = slot_at(na, a_idx);
      auto* sb = slot_at(nb, b_idx);
      if (sa && sb && sa->dir() != sb->dir()) {  // one must be in, one out
        const cc::slot* out_s; const cc::node* out_n;
        const cc::slot* in_s;  const cc::node* in_n;
        if (sa->dir() == cc::slot_dir::out) {
          out_s = sa; out_n = na; in_s = sb; in_n = nb;
        } else {
          out_s = sb; out_n = nb; in_s = sa; in_n = na;
        }
        cc::runtime::edge e{
            std::string{out_n->instance_id()}, std::string{out_s->id()},
            std::string{in_n->instance_id()},  std::string{in_s->id()}};
        g_state.g.add_edge(std::move(e));
        invalidate_view_cache();
        log("linked " + std::string{out_s->id()} + " → " + std::string{in_s->id()});
      }
    }
    pending_new = false;
  }
  if (pending_drop) {
    // Decode the dragged pin into (node_instance, slot*) so we can filter
    // palette candidates by connectable type.
    auto decode = [](ed::PinId p) -> std::pair<int, int> {
      int v = static_cast<int>(reinterpret_cast<intptr_t>(p.AsPointer()));
      return {v / 64, v % 64};
    };
    auto [ed_id, slot_idx] = decode(pending_drop_pin);
    auto it = g_state.ed2inst.find(ed_id);
    if (it != g_state.ed2inst.end()) {
      auto* n = g_state.g.find_node(it->second);
      if (n) {
        const cc::slot* s = nullptr;
        int i = 0;
        for (auto* sp : n->slots()) { if (i == slot_idx) { s = sp; break; } ++i; }
        if (s) {
          // Use io.MousePos, NOT MouseClickedPos[Left]: this is a drag, so
          // the press position sits on the source pin (where the link was
          // grabbed), while MousePos is the current = release position — i.e.
          // where the user actually dropped. Inside ed::Begin/End both are
          // canvas-local (imgui-node-editor transforms io.MousePos);
          // CanvasToScreen converts to absolute screen for SetNextWindowPos.
          ImVec2 canvas_pos = ImGui::GetIO().MousePos;
          ImVec2 screen_pos = ed::CanvasToScreen(canvas_pos);
          g_palette.open          = true;
          g_palette.canvas_pos    = canvas_pos;
          g_palette.screen_pos    = screen_pos;
          g_palette.src_type      = s->type();
          g_palette.src_is_output = (s->dir() == cc::slot_dir::out);
          g_palette.src_node      = it->second;
          g_palette.src_slot      = std::string{s->id()};
        }
      }
    }
    pending_drop = false;
  }

  // ---- Delete: nodes and links ----
  if (ed::BeginDelete()) {
    ed::NodeId nid;
    while (ed::QueryDeletedNode(&nid)) {
      if (ed::AcceptDeletedItem()) {
        int v = static_cast<int>(reinterpret_cast<intptr_t>(nid.AsPointer()));
        auto it = g_state.ed2inst.find(v);
        if (it != g_state.ed2inst.end()) {
          std::string instance = it->second;
          g_state.g.remove_edges_of(instance);
          g_state.g.remove_node(instance);
          if (g_state.view_selected == instance) g_state.view_selected.clear();
            g_state.inst2ed.erase(instance);
            g_state.ed2inst.erase(it);
            invalidate_view_cache();
            log("deleted node " + instance);
        }
      }
    }
    ed::LinkId lid;
    while (ed::QueryDeletedLink(&lid)) {
      if (ed::AcceptDeletedItem()) {
        int link_v = static_cast<int>(reinterpret_cast<intptr_t>(lid.AsPointer()));
        for (auto const& e : g_state.g.edges()) {
          if (stable_link_id(e) == link_v) {
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
  if (ed::ShowBackgroundContextMenu()) {
    // Inside ed::Begin/End, ImGui::GetIO().MouseClickedPos[] returns
    // *canvas-local* coordinates (origin = canvas view center), not absolute
    // screen coords. We need both:
    //   - screen coords  → SetNextWindowPos for the menu window
    //   - canvas coords  → ed::SetNodePosition for newly created nodes
    // ed::CanvasToScreen does the conversion, but only while an editor
    // context is current — so we must call it here, not after ed::End.
    ImVec2 canvas_pos = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Right];
    ImVec2 screen_pos = ed::CanvasToScreen(canvas_pos);
    g_create_menu.open       = true;
    g_create_menu.pos        = screen_pos;
    g_create_menu.canvas_pos = canvas_pos;
    log("popup click canvas=(" +
        std::to_string(static_cast<int>(canvas_pos.x)) + "," +
        std::to_string(static_cast<int>(canvas_pos.y)) + ") screen=(" +
        std::to_string(static_cast<int>(screen_pos.x)) + "," +
        std::to_string(static_cast<int>(screen_pos.y)) + ")");
  }

  // NavigateToContent needs the editor to know current node bounds, which
  // only happens once nodes have been submitted this frame. Call it here,
  // just before ed::End() — still inside ed::Begin/End so context is live.
  if (g_state.canvas_navigate_content) {
    ed::NavigateToContent();
    g_state.canvas_navigate_content = false;
  }

  ed::End();
  ed::SetCurrentEditor(nullptr);

  // Render the menu OUTSIDE ed::Begin/End so it's a plain ImGui window,
  // not affected by the editor's draw channels.
  draw_create_menu();
  draw_palette_drop();
}

// ---------------------------------------------------------------------------
// View tab
// ---------------------------------------------------------------------------
void draw_view_window() {
  struct view_entry { std::string instance; std::string label; };
  std::vector<view_entry> views;

  std::unordered_map<std::string, int> name_counts;
  for (auto const& n : g_state.g.nodes()) {
    if (n->type_id() != "basic.view") continue;
    name_counts[std::string{n->properties().get("name")}]++;
  }
  for (auto const& n : g_state.g.nodes()) {
    if (n->type_id() != "basic.view") continue;
    std::string name{n->properties().get("name")};
    if (name.empty()) name = "(unnamed)";
    std::string label = name;
    if (name_counts[name] > 1) {
      label += "  [" + std::string{n->instance_id().substr(0, 16)} + "...]";
    }
    views.push_back({std::string{n->instance_id()}, std::move(label)});
  }

  if (views.empty()) {
    ImGui::TextDisabled("no view nodes in the pipeline.");
    ImGui::TextDisabled("right-click the canvas → Basic → View to add one,");
    ImGui::TextDisabled("then drag from a Source output to its input.");
    return;
  }

  auto found = std::find_if(views.begin(), views.end(),
                            [](const view_entry& v) {
                              return v.instance == g_state.view_selected;
                            });
  if (found == views.end()) g_state.view_selected = views.front().instance;
  found = std::find_if(views.begin(), views.end(),
                       [](const view_entry& v) {
                         return v.instance == g_state.view_selected;
                       });
  int current_idx = static_cast<int>(std::distance(views.begin(), found));

  auto getter = [](void* data, int idx) -> const char* {
    const auto* v = static_cast<const std::vector<view_entry>*>(data);
    return (*v)[idx].label.c_str();
  };
  ImGui::SetNextItemWidth(320);
  if (ImGui::Combo("##view_select", &current_idx, getter, &views,
                   static_cast<int>(views.size()))) {
    g_state.view_selected = views[current_idx].instance;
    invalidate_view_cache();
  }

  ImGui::SameLine();

  // Trigger a background pull either because the user clicked Refresh or
  // because the cache has been stale longer than the debounce window.
  auto trigger_pull = [&]() {
    g_state.view_pull_target  = g_state.view_selected;
    g_state.view_pull_running = true;
    g_state.view_cached_error.clear();
    std::string target = g_state.view_selected;
    g_state.view_pull_future = std::async(
        std::launch::async,
        [target]() -> std::pair<std::string, std::optional<cc::any_value>> {
          cc::runtime::runner r{g_state.g, [](std::string_view msg) {
            ::log(std::string{msg});
          }};
          auto result = r.pull(target, "in");
          if (!result) {
            return {result.error().what, std::nullopt};
          }
          const cc::any_value* v = *result;
          if (!v || !v->has_value()) {
            return {std::string{"(no value — input is empty)"}, std::nullopt};
          }
          // any_value is copyable (aa::copy) — copy into the optional.
          return {std::string{}, std::optional<cc::any_value>{*v}};
        });
  };

  // If a background pull is running, poll its status. Otherwise decide
  // whether to trigger a new pull (auto-react with debounce, or manual).
  if (g_state.view_pull_running) {
    using namespace std::chrono_literals;
    if (g_state.view_pull_future.wait_for(0s) == std::future_status::ready) {
      auto [error_or_type, value_opt] = g_state.view_pull_future.get();
      g_state.view_pull_running = false;
      g_state.view_cached_for    = g_state.view_pull_target;
      g_state.view_cache_stale   = false;
      if (!error_or_type.empty()) {
        g_state.view_cached_error = std::move(error_or_type);
        g_state.view_cached_value.reset();
      } else {
        g_state.view_cached_error.clear();
        g_state.view_cached_value = std::move(value_opt);
      }
    }
  } else if (!g_state.view_selected.empty()) {
    // Reactive: if the cache is stale and no pull is running, wait out the
    // debounce window (so rapid edits don't storm pulls) then trigger
    // automatically. The user can also click Refresh to skip the wait.
    constexpr auto kDebounce = std::chrono::milliseconds(150);
    const bool debounce_elapsed =
        !g_state.view_cache_stale ||
        (std::chrono::steady_clock::now() - g_state.view_stale_since >= kDebounce);
    if (debounce_elapsed) {
      trigger_pull();
    }
  }

  // Manual Refresh button (instant — skips debounce).
  const bool can_refresh = !g_state.view_pull_running;
  const char* label = g_state.view_pull_running
                          ? "Working..."
                          : (g_state.view_cache_stale ? "Refresh*" : "Refresh");
  if (!can_refresh) { ImGui::BeginDisabled(); }
  if (ImGui::Button(label) && can_refresh) {
    trigger_pull();
  }
  if (!can_refresh) { ImGui::EndDisabled(); }

  ImGui::SameLine();
  if (g_state.view_pull_running) {
    ImGui::TextDisabled("(pulling %s ...)", g_state.view_pull_target.c_str());
  } else {
    ImGui::TextDisabled("(reactive, %ums debounce)", 150u);
  }

  ImGui::Separator();

  if (!g_state.view_cached_error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.50f, 0.40f, 1.0f));
    ImGui::TextWrapped("error: %s", g_state.view_cached_error.c_str());
    ImGui::PopStyleColor();
    return;
  }
  if (!g_state.view_cached_value.has_value()) {
    ImGui::TextDisabled("(no value yet — click Refresh)");
    return;
  }

  auto type_desc = g_state.view_cached_value->type_descriptor();
  auto* renderer = g_state.host->renderers().get_for_type(type_desc);
  if (!renderer) {
    ImGui::TextDisabled("(no renderer registered for type '%s')",
                        g_state.view_cached_type_name.c_str());
    return;
  }
  static noop_view_context ctx;
  renderer->render(*g_state.view_cached_value, ctx);
}

// ---------------------------------------------------------------------------
// Main menu, About popup, status bar
// ---------------------------------------------------------------------------
#ifndef CC_VERSION_STRING
#define CC_VERSION_STRING "0.0.0.0"
#endif

void draw_main_menu(HelloImGui::DockingParams& docking) {
  // ---- File ----
  // Only truly global commands live here: tab creation (future), app quit.
  // Canvas-specific actions (Clear, Run, ...) belong to the canvas toolbar.
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Quit", "Alt+F4")) {
      HelloImGui::GetRunnerParams()->appShallExit = true;
    }
    ImGui::EndMenu();
  }

  // ---- View (window visibility toggles) ----
  if (ImGui::BeginMenu("View")) {
    for (auto& w : docking.dockableWindows) {
      if (w.includeInViewMenu) {
        ImGui::MenuItem(w.label.c_str(), nullptr, &w.isVisible);
      }
    }
    ImGui::EndMenu();
  }

  // ---- Help ----
  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("About cc-workbench")) {
      g_state.about_open = true;
    }
    ImGui::EndMenu();
  }
}

void draw_about_popup() {
  // OpenPopup must be called from outside the modal popup itself, on the
  // same frame; we trigger it off the about_open flag set by the menu item.
  static bool should_open = false;
  if (g_state.about_open) { should_open = true; g_state.about_open = false; }
  if (should_open) {
    ImGui::OpenPopup("About cc-workbench");
    should_open = false;
  }
  ImGui::SetNextWindowSize(ImVec2(360, 200), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("About cc-workbench", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextDisabled("cc-workbench");
    ImGui::Spacing();
    ImGui::Text("Version:   v%s", CC_VERSION_STRING);
    ImGui::Text("Plugins:   %zu loaded", g_state.loaded_plugins);
    ImGui::Text("Node types: %zu registered",
                g_state.host->node_factories().size());
    ImGui::Separator();
    ImGui::TextDisabled("Node-graph platform host.\nBuilt with Dear ImGui, "
                        "imgui-node-editor, AnyAny.");
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void draw_status_bar() {
  // Left: live counts.
  ImGui::Text("Plugins: %zu   Nodes: %zu   Edges: %zu",
              g_state.loaded_plugins,
              g_state.g.nodes().size(),
              g_state.g.edges().size());

  // Right: semver vMaj.Min.Rev.Patch (Blender-style).
  std::string version = "v" + std::string{CC_VERSION_STRING};
  float avail = ImGui::GetContentRegionAvail().x;
  float text_w = ImGui::CalcTextSize(version.c_str()).x;
  ImGui::SameLine(avail - text_w);
  ImGui::TextDisabled("%s", version.c_str());
}

}  // namespace

int main() {
  g_state.host = cc::runtime::make_host_registry();
  g_state.host->renderers().register_renderer(std::make_unique<text_view_renderer>());
  g_state.host->renderers().register_renderer(std::make_unique<ir_view_renderer>());
  g_state.host->renderers().register_renderer(std::make_unique<ast_view_renderer>());
  g_state.host->renderers().register_renderer(std::make_unique<int_view_renderer>());
  g_state.host->renderers().register_renderer(std::make_unique<path_view_renderer>());

  std::size_t loaded = g_state.loader.load_all(*g_state.host);
  g_state.loaded_plugins = loaded;
  log(std::string{"cc-workbench ready. plugins loaded: "} + std::to_string(loaded));
  log(std::string{"node types registered: "} +
      std::to_string(g_state.host->node_factories().size()));

  HelloImGui::RunnerParams params;
  params.appWindowParams.windowTitle = "cc-workbench";
  params.appWindowParams.windowGeometry.size = {1480, 820};
  params.imGuiWindowParams.defaultImGuiWindowType =
      HelloImGui::DefaultImGuiWindowType::ProvideFullScreenDockSpace;

  // Main menu + status bar.
  params.imGuiWindowParams.showMenuBar     = true;
  params.imGuiWindowParams.showMenu_App    = false;  // we provide our own File/Help
  params.imGuiWindowParams.showMenu_View   = false;  // we provide our own View
  params.imGuiWindowParams.showStatusBar   = true;
  params.imGuiWindowParams.showStatus_Fps  = false;  // we put version there
  params.callbacks.ShowMenus = [&docking = params.dockingParams]() {
    draw_main_menu(docking);
  };
  params.callbacks.ShowStatus = []() { draw_status_bar(); };
  params.callbacks.ShowGui    = []() { draw_about_popup(); };

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

  params.callbacks.PostInit = []() {
    ed::Config cfg;
    cfg.SettingsFile = "";
    g_editor = ed::CreateEditor(&cfg);
    ed::SetCurrentEditor(g_editor);

    auto& s = ed::GetStyle();
    s.NodeRounding = 6.0f;
    s.PinRounding  = 8.0f;
    s.Colors[ed::StyleColor_Bg]            = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    s.Colors[ed::StyleColor_NodeBg]        = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    s.Colors[ed::StyleColor_NodeBorder]    = ImVec4(0.35f, 0.35f, 0.42f, 0.90f);
    s.Colors[ed::StyleColor_HovNodeBorder] = ImVec4(0.60f, 0.60f, 0.68f, 1.00f);
    s.Colors[ed::StyleColor_SelNodeBorder] = ImVec4(0.95f, 0.75f, 0.30f, 1.00f);

    ImFileDialogSetupTextureLoader();
  };
  params.callbacks.BeforeExit = []() {
    if (g_editor) { ed::DestroyEditor(g_editor); g_editor = nullptr; }
  };

  params.callbacks.LoadAdditionalFonts = []() {
    ImGuiIO& io = ImGui::GetIO();
    HelloImGui::FontLoadingParams ui;
    ui.fontConfig.GlyphRanges = io.Fonts->GetGlyphRangesCyrillic();
    g_state.ui_font = HelloImGui::LoadFont("fonts/UI-Regular.ttf", 20.0f, ui);

    if (HelloImGui::AssetExists("fonts/NotoColorEmoji.ttf")) {
      static const ImWchar emoji_ranges[] = {0x1F600, 0x1F64F, 0x2764, 0x2764, 0, 0};
      HelloImGui::FontLoadingParams em;
      em.mergeToLastFont = true;
      em.loadColor       = true;
      em.fontConfig.GlyphRanges = emoji_ranges;
      HelloImGui::LoadFont("fonts/NotoColorEmoji.ttf", 20.0f, em);
    }

    if (HelloImGui::AssetExists("fonts/IBMPlexMono-Regular.ttf")) {
      HelloImGui::FontLoadingParams mono;
      mono.fontConfig.GlyphRanges = io.Fonts->GetGlyphRangesCyrillic();
      g_state.mono_font = HelloImGui::LoadFont("fonts/IBMPlexMono-Regular.ttf", 16.0f, mono);
    }
  };

  // --- Pipeline window ---
  {
    HelloImGui::DockableWindow w;
    w.label          = "Pipeline";
    w.dockSpaceName  = "MainDockSpace";
    w.canBeClosed    = false;
    w.includeInViewMenu = false;
    w.GuiFunction    = []() {
      poll_file_dialog();
      draw_pipeline_canvas();
    };
    params.dockingParams.dockableWindows.push_back(w);
  }

  // --- View window ---
  {
    HelloImGui::DockableWindow w;
    w.label          = "View";
    w.dockSpaceName  = "BottomSpace";
    w.canBeClosed    = false;
    w.includeInViewMenu = false;
    w.GuiFunction    = []() { draw_view_window(); };
    params.dockingParams.dockableWindows.push_back(w);
  }

  // --- Logger window ---
  {
    HelloImGui::DockableWindow w;
    w.label          = "Logger";
    w.dockSpaceName  = "LoggerSpace";
    w.canBeClosed    = false;
    w.includeInViewMenu = false;
    w.GuiFunction    = []() { g_log.render(); };
    params.dockingParams.dockableWindows.push_back(w);
  }

  HelloImGui::Run(params);
  return 0;
}
