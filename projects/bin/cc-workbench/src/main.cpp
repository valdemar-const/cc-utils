#include <cc/pipeline.hpp>

#include "hello_imgui/hello_imgui.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "imgui_stacklayout.h"  // ImGui::Spring / BeginVertical (vendored in imgui_bundle)
#include "ne/builders.h"        // util::BlueprintNodeBuilder
#include "ne/widgets.h"         // ax::Widgets::Icon

#include <sys/wait.h>
#include <cstdlib>
#include <string>
#include <utility>

namespace ed = ax::NodeEditor;
namespace util = ax::NodeEditor::Utilities;
using ax::Drawing::IconType;

static ed::EditorContext* g_editor = nullptr;

// Typed ports: name + color + pin shape.
struct PinType {
  const char* name;
  ImVec4 color;
  IconType icon;
};
static const PinType T_str{"str", ImVec4(0.35f, 0.60f, 0.95f, 1.0f), IconType::Circle};
static const PinType T_ir{"ir", ImVec4(0.35f, 0.80f, 0.45f, 1.0f), IconType::Square};
static const PinType T_exe{"exe", ImVec4(0.95f, 0.60f, 0.25f, 1.0f), IconType::Diamond};

// Editable state shown as widgets inside the nodes.
static int g_return_value = 0;
static char g_output_path[256] = "/tmp/ccp_wb_out";
static std::string g_log = "(nothing compiled yet)";

static void DrawPin(const PinType& t, bool filled) {
  ax::Widgets::Icon(ImVec2(24, 24), t.icon, filled, t.color, ImVec4(0, 0, 0, 0));
}

int main() {
  HelloImGui::RunnerParams params;
  params.appWindowParams.windowTitle = "cc-workbench";
  params.appWindowParams.windowGeometry.size = {1280, 800};

  params.callbacks.PostInit = []() {
    ed::Config cfg;
    cfg.SettingsFile = "cc_workbench_nodes.json";
    g_editor = ed::CreateEditor(&cfg);
    ed::SetCurrentEditor(g_editor);

    // Dark "blueprint" theme.
    auto& s = ed::GetStyle();
    s.NodeRounding = 6.0f;
    s.PinRounding = 8.0f;
    s.Colors[ed::StyleColor_Bg] = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    s.Colors[ed::StyleColor_NodeBg] = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    s.Colors[ed::StyleColor_NodeBorder] = ImVec4(0.35f, 0.35f, 0.42f, 0.90f);
    s.Colors[ed::StyleColor_HovNodeBorder] = ImVec4(0.60f, 0.60f, 0.68f, 1.00f);
    s.Colors[ed::StyleColor_SelNodeBorder] = ImVec4(0.95f, 0.75f, 0.30f, 1.00f);
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
          0x1F600, 0x1F64F,  // emoticons 😀..🙃
          0x2764, 0x2764,    // ❤
          0, 0,
      };
      HelloImGui::FontLoadingParams em;
      em.mergeToLastFont = true;
      em.loadColor = true;
      em.fontConfig.GlyphRanges = emoji_ranges;
      HelloImGui::LoadFont("fonts/NotoColorEmoji.ttf", 20.0f, em);
    }
  };

  params.callbacks.ShowGui = []() {
    // --- Toolbar: green Compile button + status line ---
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.62f, 0.33f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.78f, 0.40f, 1.0f));
    bool pressed = ImGui::Button("Compile", ImVec2(120, 32));
    ImGui::PopStyleColor(2);
    if (pressed) {
      std::string src = "return " + std::to_string(g_return_value) + ";";
      auto built = cc::pipeit::pipeline_builder{}.front("tl").back("x86_64").build();
      if (!built) {
        g_log = std::string("build failed: ") + built.error();
      } else {
        auto pipe = std::move(*built);
        std::string out = g_output_path;
        if (!pipe.run(src, out)) {
          g_log = "run failed";
        } else {
          int status = std::system(out.c_str());
          g_log = "compiled '" + src + "' -> exit=" +
                  std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
      }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(g_log.c_str());
    ImGui::Separator();

    // --- Canvas ---
    ed::SetCurrentEditor(g_editor);
    ed::Begin("Pipeline Graph", ImVec2(0, 0));

    static bool first_frame = true;
    if (first_frame) {
      ed::SetNodePosition(1, ImVec2(40, 120));
      ed::SetNodePosition(2, ImVec2(400, 120));
      ed::SetNodePosition(3, ImVec2(780, 120));
    }

    util::BlueprintNodeBuilder b;

    // Node 1 — Source (no input; one str output; editable return value)
    b.Begin(1);
    b.Header(ImVec4(0.15f, 0.35f, 0.60f, 1.0f));
    ImGui::TextUnformatted("Source  test.tl");
    ImGui::Spring(1);
    b.EndHeader();
    b.Middle();
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("return", &g_return_value);
    b.Output(10);
    ImGui::Spring(0);
    ImGui::TextUnformatted("str");
    ImGui::Spring(0);
    DrawPin(T_str, true);
    b.EndOutput();
    b.End();

    // Node 2 — Frontend [tl] (str -> ir)
    b.Begin(2);
    b.Header(ImVec4(0.45f, 0.25f, 0.62f, 1.0f));
    ImGui::TextUnformatted("Frontend  [tl]");
    ImGui::Spring(1);
    b.EndHeader();
    b.Input(20);
    DrawPin(T_str, true);
    ImGui::Spring(0);
    ImGui::TextUnformatted("str");
    b.EndInput();
    b.Middle();
    ImGui::TextDisabled("parse");
    b.Output(21);
    ImGui::Spring(0);
    ImGui::TextUnformatted("ir");
    ImGui::Spring(0);
    DrawPin(T_ir, true);
    b.EndOutput();
    b.End();

    // Node 3 — Backend [x86_64] (ir -> exe; editable output path)
    b.Begin(3);
    b.Header(ImVec4(0.64f, 0.26f, 0.26f, 1.0f));
    ImGui::TextUnformatted("Backend  [x86_64]");
    ImGui::Spring(1);
    b.EndHeader();
    b.Input(30);
    DrawPin(T_ir, true);
    ImGui::Spring(0);
    ImGui::TextUnformatted("ir");
    b.EndInput();
    b.Middle();
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("output", g_output_path, sizeof(g_output_path));
    b.End();

    ed::Link(100, 10, 20);  // source.str -> tl.in
    ed::Link(101, 21, 30);  // tl.ir -> x86_64.in

    ed::End();
    if (first_frame) {
      ed::NavigateToContent(0.0f);
      first_frame = false;
    }
    ed::SetCurrentEditor(nullptr);
  };

  HelloImGui::Run(params);
  return 0;
}
