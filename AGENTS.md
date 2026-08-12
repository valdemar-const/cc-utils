# Code Output Guidelines for AI Assistants

When helping users with coding tasks, please follow these guidelines to ensure high-quality, maintainable code.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.
- If a solution becomes complex (multiple cascading changes, need for workarounds), STOP and explain the difficulty. Present options rather than plowing ahead.

**If you encounter an API in the codebase which is awkward to use:**
- Do not circumvent it with a hack. Instead, surface the issue and ask for clarification or improvement.
- The same goes for code smells or patterns that seem out of place. Don't just "make it work" — stop implementing, then communicate the underlying problem so it can be addressed properly.

**Before implementing a solution, wait for the user to finish evaluating alternatives.**

**No whack-a-mole loops.** When hitting a second unexpected failure in a row on a hard problem: STOP fixing. Present the full picture and ask to examine the difficulties together before writing more code.

## 1b. Interaction Style

**Wait before acting.** Do NOT start implementing before the user has finished evaluating alternatives or describing the problem. Wait for explicit go-ahead. When asked to analyze or review, produce analysis ONLY — not code changes.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

---

# cc-utils: Project Guide for AI Assistants

cc-utils is a **node-graph platform** for building compiler and tool pipelines with a visual dataflow editor. Plugins provide node types (sources, transforms, sinks); the host application (cc-workbench) renders an interactive graph editor where users wire nodes together and preview results.

## Repository Structure

```
cc-utils/
├── CMakeLists.txt              # Top-level wrapper → build/cmake/main.cmake
├── build/
│   └── cmake/
│       ├── main.cmake          # Includes options, configure, dependencies, targets
│       ├── options.cmake       # Build options (CC_BUILD_SHARED, CC_BUILD_STATIC, ...)
│       ├── configure.cmake     # C++23, output dirs, plugin output dir
│       ├── dependencies.cmake  # CPM dependency resolution
│       ├── targets.cmake       # cc_add_library / cc_add_executable / cc_add_plugin
│       └── modules/3rdparty/
│           └── deps.toml       # CPM dependency manifest (Boost, ImGui Bundle, ...)
├── projects/
│   ├── lib/                    # Libraries (linked at build time)
│   │   ├── cc-core/            # Abstract interfaces: node, host_registry, view, any_value
│   │   ├── cc-runtime/         # Concrete runtime: graph, runner, plugin_loader, host_registry impl
│   │   ├── cc-astit/           # AST infrastructure (visitors, traversals)
│   │   ├── cc-astq/            # AST queries
│   │   ├── cc-ir/              # Intermediate representation
│   │   ├── cc-parseit/         # Parser combinators
│   │   └── cc-gen/             # Code generation utilities
│   ├── plugin/                 # Runtime-loaded plugins (.so/.dll in <runtime>/plugins/)
│   │   ├── cc-plugin-basic/    # text.from_file, view, exec, ...
│   │   ├── cc-plugin-tl/       # TL language parser
│   │   ├── cc-plugin-tl-ir/    # TL → IR lowering
│   │   └── cc-plugin-x86_64/   # x86_64 backend (assembler via nasm+ld)
│   └── bin/
│       └── cc-workbench/       # GUI host application (Dear ImGui + node-editor)
├── tests/                      # Per-target test suites (GoogleTest, gated by BUILD_TESTING)
├── plans/                      # Specs, progress notes, user stories
└── format_sources.sh           # Runs clang-format on all projects/**/*.cpp|.hpp
```

### Auto-discovery

CMake auto-discovers subdirectories under `projects/{lib,bin,plugin}/` and `tests/` via `file(GLOB ... CONFIGURE_DEPENDS)`. Adding a new directory with a `CMakeLists.txt` is sufficient — no manual registration needed.

## Build System

```bash
# Configure (first time)
cmake -B out/Debug -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build out/Debug

# Build with tests
cmake -B out/Debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build out/Debug

# Run tests
cd out/Debug && ctest --output-on-failure
```

### CMake Helper Functions (`build/cmake/targets.cmake`)

- **`cc_add_library(name SOURCES ... PUBLIC_DEPS ... PRIVATE_DEPS ...)`** — Creates shared (`lib<name>.so`) and/or static (`lib<name>-static.a`) variants with alias targets `name::shared`, `name::static`, `name::name`. Generates export header `<NAME>_API`.
- **`cc_add_executable(name SOURCES ... PRIVATE_DEPS ...)`** — Standard executable.
- **`cc_add_plugin(name SOURCES ... PRIVATE_DEPS ...)`** — MODULE library (`.so`/`.dll` without `lib` prefix). Output goes to `<runtime>/plugins/`. Never linked at build time — loaded via `dlopen`/`LoadLibrary` by the host.

### Dependencies (`build/cmake/modules/3rdparty/deps.toml`)

Managed via CPM. Current dependencies:
- **Boost** 1.90.0 — cobalt/fiber async, process v2, filesystem
- **ImGui Bundle** 1.92.801 — Dear ImGui + HelloImGui + node-editor + ImFileDialog + ImGuiColorTextEdit + imgui_stacklayout
- **AnyAny** 1.2.1 — type erasure for `cc::any_value`
- **pugixml** 1.16.0 — XML pipeline file parsing
- **nlohmann_json** 3.12.0 — JSON serialization
- **tomlplusplus** 3.4.0 — TOML config
- **GTest** 1.18.0 — unit tests (optional, gated by `BUILD_TESTING`)

## Architecture

### Plugin System

Plugins are runtime-loaded MODULE libraries. Each plugin implements `cc_plugin_register(host_registry&)` (see `cc/plugin_entry.hpp`), which registers node factories and pin types. The host (`cc-workbench`) discovers plugins in `<exe_dir>/plugins/` via `cc::runtime::plugin_loader`.

**Node factory** (`cc/node_factory.hpp`): produces node instances. Each factory declares:
- `type_id()` — unique string identifier (e.g. `"basic.text.from_file"`)
- `display_name()`, `category()` — for the canvas context menu
- `input_slots()` / `output_slots()` — pin descriptors (name, type)
- `property_schema()` — property definitions (key, label, type, default)
- `create()` — returns a `cc::node` instance

**Node** (`cc/node.hpp`): the runtime unit. Has `activate(context)` which reads inputs, does work, writes outputs via `cc::any_value`.

**Graph + Runner** (`cc-runtime`): `cc::runtime::graph` holds nodes + edges. `cc::runtime::runner` walks the graph backward from a target node's input slot, activating upstream nodes and propagating values.

### View System

View renderers display `cc::any_value` results in the workbench's View panels. Each renderer handles a specific type (text, IR, AST, int, path). Registered on the host at startup via `view_renderer_provider::register_renderer()`.

### cc-workbench (GUI Host)

Built on HelloImGui + imgui-node-editor. Single-file application (`projects/bin/cc-workbench/src/main.cpp`, ~2800 lines). Key components:
- **Node canvas** — imgui-node-editor with custom `BlueprintNodeBuilder` (Blender-style: header + two-column pins + footer properties)
- **View tabs** — dynamic DockableWindows (HelloImGui `AddDockableWindow`/`RemoveDockableWindow`), each with per-tab cache and background pull
- **Logger** — `BeginChild` + `ImGuiListClipper` with click-drag selection and Ctrl+C
- **Pipeline I/O** — XML format via pugixml (`.pipeline` files)

## Code Conventions

- **C++23**, `-Wall -Wextra -Wpedantic`
- **Allman brace style** (enforced by `.clang-format`)
- **`clang-format`** — run `./format_sources.sh` to format all sources. The config uses:
  - 4-space indent, no tabs
  - `PointerAlignment: Right` (`Type *foo`)
  - `BreakBeforeBraces: Allman`
  - `ColumnLimit: 0` (no hard wrap)
  - `SortIncludes: Never`
- **No comments** unless explicitly asked by the user
- **Namespace**: `cc` for library code, anonymous namespace for file-local symbols in the workbench
- **Export macros**: `CC_CORE_API`, `CC_RUNTIME_API`, etc. — generated by `generate_export_header`
- **File naming**: `snake_case.cpp` / `snake_case.hpp` for library sources; headers in `include/cc/`
- **Plugin naming**: `cc-plugin-<name>` directory, plugin binary is `<name>.so`/`.dll`

## ImGui Bundle Notes

The workbench uses ImGui Bundle extensively. Key things to know:

- **HelloImGui docking**: `DockableWindow` entries in `runnerParams.dockingParams.dockableWindows`. Dynamic windows via `HelloImGui::AddDockableWindow()` / `RemoveDockableWindow()` (do NOT mutate `dockableWindows` vector directly — HelloImGui iterates it every frame).
- **Window labels**: must be unique. Use `"Display Title###unique_id"` so the visible title can change without breaking ImGui's internal ID.
- **Fonts**: loaded in `LoadAdditionalFonts` callback. `HelloImGui::LoadFont()` wraps `AddFontFromMemoryTTF`. Use `mergeToLastFont=true` to merge icon/emoji fonts into the main font. `io.FontDefault` sets the global default (used for window titles, tab labels).
- **NotoEmoji-Regular.ttf** (monochrome vector) works reliably for emoji in tab titles. NotoColorEmoji.ttf (bitmap/SVG) does NOT render via FreeType — avoid it.
- **FontAwesome** icon headers: `icons_font_awesome_4.h` / `icons_font_awesome_6.h` in the HelloImGui source tree. Macros like `ICON_FA_EYE` map to UTF-8 byte sequences.
- **imgui_stacklayout**: patched ImGui with `BeginHorizontal`/`EndHorizontal`, `BeginVertical`/`EndVertical`, `Spring()`. Use for toolbars and aligned layouts instead of manual `SameLine()`.
- **imgui-node-editor**: namespace `ed` / `ax::NodeEditor`. Uses integer IDs for nodes/links/pins. Draw channels split editor content from ImGui content.

## Dependency Cache Locations

CPM caches sources under `/home/novikov_vk/cmake/cpm_cache/`. Key paths:
- `imgui_bundle/<hash>/` — ImGui Bundle + all external submodules (hello_imgui, imgui, implot, etc.)
- `anyany/<hash>/` — AnyAny headers
- `boost/` — downloaded via CPM (large, ~500MB extracted)

When investigating ImGui Bundle / HelloImGui internals, search:
```
/home/novikov_vk/cmake/cpm_cache/imgui_bundle/<hash>/external/hello_imgui/hello_imgui/src/hello_imgui/
```

## Plans and Specs

`plans/` contains design documents:
- `user-story.md` — high-level user stories
- `PROGRESS.md` — progress tracking
- `dod-compiler-notes.md` — design notes

## Workflow

1. Read the relevant source files to understand context.
2. Check `AGENTS.md` (this file) for conventions.
3. Build with `cmake --build out/Debug` after changes.
4. Run `./format_sources.sh` before committing (or let clang-format handle it).
5. Never commit unless explicitly asked.
6. When investigating third-party library behavior, read the cached source in `/home/novikov_vk/cmake/cpm_cache/`.
