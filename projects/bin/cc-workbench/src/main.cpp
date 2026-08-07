#include <cc/pipeline.hpp>

#include "hello_imgui/hello_imgui.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "imgui_stacklayout.h"  // ImGui::Spring / BeginVertical (vendored in imgui_bundle)
#include "ne/builders.h"        // util::BlueprintNodeBuilder
#include "ne/widgets.h"         // ax::Widgets::Icon

#include <sys/wait.h>
#include <unistd.h>  // readlink (/proc/self/exe)
#include <ImFileDialog/ImFileDialog.h>                       // bundled in imgui_bundle (target im_file_dialog)
#include <bundle_integration/ImFileDialogTextureHelper.h>    // ImFileDialogSetupTextureLoader (GL icon textures)
#include "ImGuiColorTextEdit/TextEditor.h"                   // rich code editor (target imgui_color_text_edit)

#include <algorithm>  // std::min / std::max
#include <cstdlib>    // std::system
#include <cstring>    // std::strncpy
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace ed = ax::NodeEditor;
namespace util = ax::NodeEditor::Utilities;
using ax::Drawing::IconType;

static ed::EditorContext* g_editor = nullptr;

// Typed ports model the pipeline stages. source(text) -> ast(lang-specific)
// -> ir(neutral narrow waist) -> exe. The language link is dropped at IR.
struct PinType {
  const char* name;
  ImVec4 color;
  IconType icon;
};
static const PinType T_src{"source", ImVec4(0.35f, 0.60f, 0.95f, 1.0f), IconType::Circle};
static const PinType T_ast{"ast", ImVec4(0.62f, 0.40f, 0.78f, 1.0f), IconType::Square};
static const PinType T_ir{"ir", ImVec4(0.35f, 0.80f, 0.45f, 1.0f), IconType::Square};
static const PinType T_exe{"exe", ImVec4(0.95f, 0.60f, 0.25f, 1.0f), IconType::Diamond};

// Editable state.
static char g_source_path[512] = "";
static char g_output_path[256] = "/tmp/ccp_wb_out";
static std::string g_log = "(select a .tl source file, then Compile)";
static const char* kSourceDlg = "ccSourceOpen";  // ImFileDialog key

// Rich editors (bottom, tabbed). Source is editable; Output is a read-only log.
static TextEditor g_code_editor;
static TextEditor g_out_editor;
static std::string g_out_text;
static ImFont* g_code_font = nullptr;  // IBM Plex Mono — used in the bottom editors

static void DrawPin(const PinType& t, bool filled) {
  ax::Widgets::Icon(ImVec2(24, 24), t.icon, filled, t.color, ImVec4(0, 0, 0, 0));
}

static std::string read_file(const char* path) {
  std::ifstream in(path);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Directory of the running executable — assets/ (fonts, examples, test.tl) live
// next to it, so the file dialog can open there directly.
static std::string exe_dir() {
#ifdef __linux__
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf));
  if (n > 0) {
    std::string p(buf, static_cast<size_t>(n));
    auto slash = p.find_last_of('/');
    if (slash != std::string::npos) return p.substr(0, slash);
  }
#endif
  return ".";
}

static std::string basename_of(const char* path) {
  std::string p{path};
  auto slash = p.find_last_of('/');
  return slash != std::string::npos ? p.substr(slash + 1) : p;
}

// imgui-node-editor has no built-in auto-layout, so we measure each node's
// rendered size and lay the pipeline out left-to-right along the data flow
// (Source -> Frontend -> IR-gen -> Backend). Adaptive to DPI and content width.
static void auto_arrange_nodes() {
  constexpr float kGap = 60.0f;
  constexpr float kY = 120.0f;
  float x = 40.0f;
  for (const int id : {1, 2, 3, 4}) {
    const ImVec2 sz = ed::GetNodeSize(id);
    ed::SetNodePosition(id, ImVec2(x, kY));
    x += (sz.x > 1.0f ? sz.x : 220.0f) + kGap;
  }
}

static void append_out(const std::string& msg) {
  g_out_text += msg + "\n";
  g_out_editor.SetText(g_out_text);
}

int main() {
  HelloImGui::RunnerParams params;
  params.appWindowParams.windowTitle = "cc-workbench";
  params.appWindowParams.windowGeometry.size = {1480, 820};

  // Full-screen dockspace: top = node canvas, bottom = tabbed editors.
  params.imGuiWindowParams.defaultImGuiWindowType =
      HelloImGui::DefaultImGuiWindowType::ProvideFullScreenDockSpace;

  params.callbacks.PostInit = []() {
    ed::Config cfg;
    cfg.SettingsFile = "";  // persistence off: deterministic auto-layout each launch
    g_editor = ed::CreateEditor(&cfg);
    ed::SetCurrentEditor(g_editor);

    auto& s = ed::GetStyle();
    s.NodeRounding = 6.0f;
    s.PinRounding = 8.0f;
    s.Colors[ed::StyleColor_Bg] = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    s.Colors[ed::StyleColor_NodeBg] = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    s.Colors[ed::StyleColor_NodeBorder] = ImVec4(0.35f, 0.35f, 0.42f, 0.90f);
    s.Colors[ed::StyleColor_HovNodeBorder] = ImVec4(0.60f, 0.60f, 0.68f, 1.00f);
    s.Colors[ed::StyleColor_SelNodeBorder] = ImVec4(0.95f, 0.75f, 0.30f, 1.00f);

    ImFileDialogSetupTextureLoader();

    g_code_editor.SetPalette(TextEditor::GetDarkPalette());
    g_code_editor.SetLanguage(TextEditor::Language::Cpp());
    g_out_editor.SetPalette(TextEditor::GetDarkPalette());
    g_out_editor.SetReadOnlyEnabled(true);  // plain text compile log
    append_out("cc-workbench ready. Browse a .tl file, then Compile.");
  };
  params.callbacks.BeforeExit = []() {
    if (g_editor) {
      ed::DestroyEditor(g_editor);
      g_editor = nullptr;
    }
  };

  // Default font: Fira Sans (Latin + Cyrillic), DPI-autoadjusted for crispness,
  // + a small range of color emoji merged as a fallback.
  params.callbacks.LoadAdditionalFonts = []() {
    ImGuiIO& io = ImGui::GetIO();

    HelloImGui::FontLoadingParams ui;
    ui.fontConfig.GlyphRanges = io.Fonts->GetGlyphRangesCyrillic();
    HelloImGui::LoadFont("fonts/UI-Regular.ttf", 20.0f, ui);

    if (HelloImGui::AssetExists("fonts/NotoColorEmoji.ttf")) {
      static const ImWchar emoji_ranges[] = {
          0x1F600, 0x1F64F,  // emoticons
          0x2764, 0x2764,    // heart
          0, 0,
      };
      HelloImGui::FontLoadingParams em;
      em.mergeToLastFont = true;
      em.loadColor = true;
      em.fontConfig.GlyphRanges = emoji_ranges;
      HelloImGui::LoadFont("fonts/NotoColorEmoji.ttf", 20.0f, em);
    }

    // Monospace editor font (IBM Plex Mono) for the bottom code tabs.
    if (HelloImGui::AssetExists("fonts/IBMPlexMono-Regular.ttf")) {
      HelloImGui::FontLoadingParams mono;
      mono.fontConfig.GlyphRanges = io.Fonts->GetGlyphRangesCyrillic();
      g_code_font = HelloImGui::LoadFont("fonts/IBMPlexMono-Regular.ttf", 18.0f, mono);
    }
  };

  // Layout: split the screen into MainDockSpace (top, node canvas) and
  // EditorSpace (bottom, ~40% — tabbed code editors).
  {
    HelloImGui::DockingSplit split;
    split.initialDock = "MainDockSpace";
    split.newDock = "EditorSpace";
    split.direction = ImGuiDir_Down;
    split.ratio = 0.40f;
    params.dockingParams.dockingSplits.push_back(split);
  }

  // --- "Pipeline" window (top): toolbar + node canvas ---
  {
    HelloImGui::DockableWindow w;
    w.label = "Pipeline";
    w.dockSpaceName = "MainDockSpace";
    w.canBeClosed = false;
    w.includeInViewMenu = false;
    w.GuiFunction = [&params]() {
      // ImFileDialog: IsDone() also renders the modal popup, so poll every frame.
      if (ifd::FileDialog::Instance().IsDone(kSourceDlg)) {
        if (ifd::FileDialog::Instance().HasResult()) {
          std::string p = ifd::FileDialog::Instance().GetResult().string();
          std::strncpy(g_source_path, p.c_str(), sizeof(g_source_path) - 1);
          g_source_path[sizeof(g_source_path) - 1] = '\0';
          g_code_editor.SetText(read_file(g_source_path));
          append_out(std::string{"loaded "} + g_source_path);
          params.dockingParams.focusDockableWindow("Source");
        }
        ifd::FileDialog::Instance().Close();
      }

      // --- Toolbar: green Compile button + status line ---
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.62f, 0.33f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.78f, 0.40f, 1.0f));
      bool pressed = ImGui::Button("Compile", ImVec2(120, 32));
      ImGui::PopStyleColor(2);
      if (pressed) {
        std::string src = g_code_editor.GetText();  // live buffer
        if (src.empty()) {
          g_log = "no source: Browse a .tl file or type in the Source tab";
          append_out(g_log);
        } else {
          auto built = cc::pipeit::pipeline_builder{}
                           .front("tl")
                           .irgen("tl-ir")
                           .back("x86_64")
                           .build();
          if (!built) {
            g_log = std::string("build failed: ") + built.error();
            append_out(g_log);
          } else {
            auto pipe = std::move(*built);
            std::string out = g_output_path;
            if (!pipe.run(src, out)) {
              g_log = "run failed";
              append_out(g_log);
            } else {
              int status = std::system(out.c_str());
              int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
              const char* name = g_source_path[0] ? basename_of(g_source_path).c_str() : "editor";
              g_log = std::string{"compiled '"} + name + "' -> exit=" + std::to_string(code);
              append_out(g_log);
            }
          }
        }
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(g_log.c_str());
      ImGui::Separator();

      // --- Canvas (fills the rest of this window) ---
      ed::SetCurrentEditor(g_editor);
      ed::Begin("Pipeline Graph", ImVec2(0, ImGui::GetContentRegionAvail().y));

      static int s_frame = 0;
      if (s_frame == 0) {
        // Frame 0: sizes unknown — spread generously to avoid a stack at origin.
        ed::SetNodePosition(1, ImVec2(40, 120));
        ed::SetNodePosition(2, ImVec2(500, 120));
        ed::SetNodePosition(3, ImVec2(960, 120));
        ed::SetNodePosition(4, ImVec2(1420, 120));
      } else if (s_frame == 1) {
        // Frame 1: sizes valid — measure and lay out along the data flow.
        auto_arrange_nodes();
      }

      util::BlueprintNodeBuilder b;

      // Node 1 — Source: points at a source file. "Edit…" opens the Source tab.
      b.Begin(1);
      b.Header(ImVec4(0.15f, 0.35f, 0.60f, 1.0f));
      ImGui::TextUnformatted("Source");
      ImGui::Spring(1);
      b.EndHeader();
      b.Middle();
      ImGui::SetNextItemWidth(150);
      ImGui::InputText("file", g_source_path, sizeof(g_source_path));
      ImGui::SameLine();
      if (ImGui::SmallButton("Browse##src")) {
        ifd::FileDialog::Instance().Open(kSourceDlg, "Open cc source",
                                         "Source (*.tl){.tl},.*", false,
                                         exe_dir() + "/assets");
      }
      if (ImGui::SmallButton("Edit…##src")) {
        params.dockingParams.focusDockableWindow("Source");
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%d lines", static_cast<int>(g_code_editor.GetLineCount()));
      b.Output(10);
      ImGui::Spring(0);
      ImGui::TextUnformatted(T_src.name);
      ImGui::Spring(0);
      DrawPin(T_src, g_code_editor.GetLineCount() > 0);
      b.EndOutput();
      b.End();

      // Node 2 — Frontend [tl]: source -> ast.
      b.Begin(2);
      b.Header(ImVec4(0.45f, 0.25f, 0.62f, 1.0f));
      ImGui::TextUnformatted("Frontend  [tl]");
      ImGui::Spring(1);
      b.EndHeader();
      b.Input(20);
      DrawPin(T_src, true);
      ImGui::Spring(0);
      ImGui::TextUnformatted(T_src.name);
      b.EndInput();
      b.Middle();
      ImGui::TextDisabled("parse");
      b.Output(21);
      ImGui::Spring(0);
      ImGui::TextUnformatted(T_ast.name);
      ImGui::Spring(0);
      DrawPin(T_ast, true);
      b.EndOutput();
      b.End();

      // Node 3 — IR generator [tl-ir]: ast -> ir. Language-specific AST enters,
      // neutral IR leaves; the frontend link is dropped here.
      b.Begin(3);
      b.Header(ImVec4(0.30f, 0.55f, 0.55f, 1.0f));
      ImGui::TextUnformatted("IR-gen  [tl-ir]");
      ImGui::Spring(1);
      b.EndHeader();
      b.Input(30);
      DrawPin(T_ast, true);
      ImGui::Spring(0);
      ImGui::TextUnformatted(T_ast.name);
      b.EndInput();
      b.Middle();
      ImGui::TextDisabled("lower");
      b.Output(31);
      ImGui::Spring(0);
      ImGui::TextUnformatted(T_ir.name);
      ImGui::Spring(0);
      DrawPin(T_ir, true);
      b.EndOutput();
      b.End();

      // Node 4 — Backend [x86_64]: ir -> exe.
      b.Begin(4);
      b.Header(ImVec4(0.64f, 0.26f, 0.26f, 1.0f));
      ImGui::TextUnformatted("Backend  [x86_64]");
      ImGui::Spring(1);
      b.EndHeader();
      b.Input(40);
      DrawPin(T_ir, true);
      ImGui::Spring(0);
      ImGui::TextUnformatted(T_ir.name);
      b.EndInput();
      b.Middle();
      ImGui::SetNextItemWidth(180);
      ImGui::InputText("output", g_output_path, sizeof(g_output_path));
      b.Output(41);
      ImGui::Spring(0);
      ImGui::TextUnformatted(T_exe.name);
      ImGui::Spring(0);
      DrawPin(T_exe, true);
      b.EndOutput();
      b.End();

      ed::Link(100, 10, 20);  // source -> tl
      ed::Link(101, 21, 30);  // tl.ast -> tl-ir
      ed::Link(102, 31, 40);  // tl-ir.ir -> x86_64

      ed::End();
      if (s_frame == 1) ed::NavigateToContent(0.0f);
      ++s_frame;
      ed::SetCurrentEditor(nullptr);
    };
    params.dockingParams.dockableWindows.push_back(w);
  }

  // --- "Source" tab (bottom): rich editor for the selected file. ---
  {
    HelloImGui::DockableWindow w;
    w.label = "Source";
    w.dockSpaceName = "EditorSpace";
    w.GuiFunction = []() {
      std::string cap = "Source: " + (g_source_path[0] ? basename_of(g_source_path) : std::string("(untitled)"));
      ImGui::TextDisabled("%s", cap.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("(%s)", g_code_editor.GetLanguageName().c_str());
      ImVec2 sz(0, ImGui::GetContentRegionAvail().y);
      if (g_code_font) ImGui::PushFont(g_code_font);
      g_code_editor.Render("##code_src", sz, false);
      if (g_code_font) ImGui::PopFont();
    };
    params.dockingParams.dockableWindows.push_back(w);
  }

  // --- "Output" tab (bottom, read-only): compile log. ---
  {
    HelloImGui::DockableWindow w;
    w.label = "Output";
    w.dockSpaceName = "EditorSpace";
    w.GuiFunction = []() {
      ImVec2 sz(0, ImGui::GetContentRegionAvail().y);
      if (g_code_font) ImGui::PushFont(g_code_font);
      g_out_editor.Render("##code_out", sz, false);
      if (g_code_font) ImGui::PopFont();
    };
    params.dockingParams.dockableWindows.push_back(w);
  }

  HelloImGui::Run(params);
  return 0;
}
