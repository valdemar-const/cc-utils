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

#include "hello_imgui/hello_imgui.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "imgui_stacklayout.h"          // ImGui::Spring
#include "ImFileDialog/ImFileDialog.h"
#include "bundle_integration/ImFileDialogTextureHelper.h"
#include "ImGuiColorTextEdit/TextEditor.h"
#include "ne/builders.h"
#include "ne/widgets.h"

#include <algorithm>
#include <cstring>
#include <functional>
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
    {"text",  ImVec4(0.35f, 0.60f, 0.95f, 1.0f)},
    {"ast",   ImVec4(0.62f, 0.40f, 0.78f, 1.0f)},
    {"ast.tl",ImVec4(0.62f, 0.40f, 0.78f, 1.0f)},
    {"ir",    ImVec4(0.35f, 0.80f, 0.45f, 1.0f)},
    {"bytes", ImVec4(0.95f, 0.60f, 0.25f, 1.0f)},
    {"any",   ImVec4(0.55f, 0.55f, 0.55f, 1.0f)},
    {"",      ImVec4(0.55f, 0.55f, 0.55f, 1.0f)},  // wildcard / unset
  };
  if (auto it = known.find(type_name); it != known.end()) return it->second;
  return hash_color(type_name);
}

auto icon_for_type(std::string_view type_name) -> IconType {
  if (type_name == "text")  return IconType::Circle;
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
  struct pending_position { int ed_id; ImVec2 screen_pos; };
  std::vector<pending_position> pending_positions;

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
  void append(std::string_view line) {
    buffer_.append(line);
    buffer_.append("\n");
    dirty_ = true;
  }
  void render() {
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
    if (g_state.mono_font) ImGui::PushFont(g_state.mono_font);
    editor_.Render("##log_text", ImVec2(0, 0), false);
    if (g_state.mono_font) ImGui::PopFont();
  }
 private:
  TextEditor  editor_;
  std::string buffer_;
  bool        dirty_ = false;
};

AppState g_state;
log_view g_log;
ed::EditorContext* g_editor = nullptr;
noop_view_context g_view_ctx;

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
      ImGui::InputTextMultiline("##v", buf, sizeof(buf), ImVec2(220, 80));
      n.properties().set(desc.key, buf);
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
      }
      break;
    }
    case cc::property_kind::boolean: {
      bool v = (current == "1" || current == "true");
      if (ImGui::Checkbox(desc.display_name.data(), &v)) {
        n.properties().set(desc.key, v ? "1" : "0");
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
  ImVec2 pos{};  // screen coords at right-click
};
create_menu_state g_create_menu;

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
          // Position is applied next frame inside ed::Begin/End — ScreenToCanvas
          // needs a current editor context, which we don't have here.
          g_state.pending_positions.push_back({ed_id, g_create_menu.pos});
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

void draw_pipeline_canvas() {
  ed::SetCurrentEditor(g_editor);
  ed::Begin("Pipeline Graph", ImVec2(0, ImGui::GetContentRegionAvail().y));

  // Apply deferred SetNodePosition requests from context-menu creations.
  // ScreenToCanvas / SetNodePosition need a current editor.
  if (!g_state.pending_positions.empty()) {
    for (auto const& pp : g_state.pending_positions) {
      ed::SetNodePosition(pp.ed_id, ed::ScreenToCanvas(pp.screen_pos));
    }
    g_state.pending_positions.clear();
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
  static ed::PinId pending_new_a, pending_new_b;
  static bool      pending_new = false;
  if (ed::BeginCreate(ImColor(120, 160, 220, 200), 2.0f)) {
    ed::PinId a, b;
    if (ed::QueryNewLink(&a, &b)) {
      if (ed::AcceptNewItem()) {
        pending_new_a = a;
        pending_new_b = b;
        pending_new   = true;
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
        log("linked " + std::string{out_s->id()} + " → " + std::string{in_s->id()});
      }
    }
    pending_new = false;
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
    g_create_menu.open = true;
    g_create_menu.pos  = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Right];
    log("popup click screen=(" +
        std::to_string(static_cast<int>(g_create_menu.pos.x)) + "," +
        std::to_string(static_cast<int>(g_create_menu.pos.y)) + ")");
  }

  ed::End();
  ed::SetCurrentEditor(nullptr);

  // Render the menu OUTSIDE ed::Begin/End so it's a plain ImGui window,
  // not affected by the editor's draw channels.
  draw_create_menu();
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
  }

  ImGui::SameLine();
  ImGui::TextDisabled("(pulled every frame)");

  ImGui::Separator();

  cc::runtime::runner r{g_state.g};
  auto result = r.pull(g_state.view_selected, "in");
  if (!result) {
    ImGui::TextDisabled("error: %s", result.error().what.c_str());
    return;
  }
  const cc::any_value* value = *result;
  if (!value || !value->has_value()) {
    ImGui::TextDisabled("(no value — connect a Source output to this View node)");
    return;
  }

  auto type_desc = value->type_descriptor();
  auto* renderer = g_state.host->renderers().get_for_type(type_desc);
  if (!renderer) {
    auto name = g_state.host->types().name_of(type_desc);
    ImGui::TextDisabled("(no renderer registered for type '%.*s')",
                        static_cast<int>(name.size()), name.data());
    return;
  }
  static noop_view_context ctx;
  renderer->render(*value, ctx);
}

}  // namespace

int main() {
  g_state.host = cc::runtime::make_host_registry();
  g_state.host->renderers().register_renderer(std::make_unique<text_view_renderer>());

  std::size_t loaded = g_state.loader.load_all(*g_state.host);
  log(std::string{"cc-workbench ready. plugins loaded: "} + std::to_string(loaded));
  log(std::string{"node types registered: "} +
      std::to_string(g_state.host->node_factories().size()));

  HelloImGui::RunnerParams params;
  params.appWindowParams.windowTitle = "cc-workbench";
  params.appWindowParams.windowGeometry.size = {1480, 820};
  params.imGuiWindowParams.defaultImGuiWindowType =
      HelloImGui::DefaultImGuiWindowType::ProvideFullScreenDockSpace;

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
