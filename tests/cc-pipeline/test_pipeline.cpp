// End-to-end pipeline test (no GUI).
//
// Builds the canonical "return 42;" compilation graph in code:
//
//   text.constant ("return 42;")
//     └─→ tl.frontend ─→ tl.irgen ─→ x86_64.nasm_gen
//                                       └─→ x86_64.assemble ─→ basic.exec
//                                                                └→ ret_code == 42
//
// All nodes are created through the plugin loader (same code path as the
// workbench host), so this exercises the full stack: dlopen, register, build
// graph, runner pull, activate, exit code propagation. If this test passes,
// the workbench can run the same pipeline by clicking Refresh.
//
// Requires the plugins to be built and discoverable via CCP_PLUGIN_PATH
// (set by the CMake test properties).

#include "cc/any_value.hpp"
#include "cc/graph.hpp"
#include "cc/host_registry.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_loader.hpp"
#include "cc/runner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace {

// Add a node from `type_id`, capture its instance_id, return the id.
auto add_node(cc::runtime::graph& g, const cc::host_registry& host,
              std::string_view type_id) -> std::string {
  auto* factory = host.find_node_factory(type_id);
  if (!factory) ADD_FAILURE() << "factory '" << type_id << "' not registered";
  auto node = factory->create();
  std::string id{node->instance_id()};
  g.add_node(std::move(node));
  return id;
}

// Hook up an output→input edge.
void connect(cc::runtime::graph& g,
             std::string_view src_node, std::string_view src_slot,
             std::string_view dst_node, std::string_view dst_slot) {
  cc::runtime::edge e{
      std::string{src_node}, std::string{src_slot},
      std::string{dst_node}, std::string{dst_slot}};
  g.add_edge(std::move(e));
}

}  // namespace

// The full "return 42;" pipeline — text constant to exit code, end-to-end.
TEST(cc_pipeline, return_42_end_to_end) {
  // --- Bootstrap: load every cc-plugin-*.so found in CCP_PLUGIN_PATH. ----
  // Loader must outlive the host: the host owns factory objects whose vtables
  // live in the plugin DLLs, so the DLLs (freed by ~plugin_loader) may only
  // unload after the host has deleted those factories. Declare loader first.
  cc::runtime::plugin_loader loader;
  auto host = cc::runtime::make_host_registry();
  std::size_t loaded = loader.load_all(*host);
  ASSERT_GE(loaded, 4u)
      << "expected at least basic/tl/tl-ir/x86_64 plugins to load; "
      << "is CCP_PLUGIN_PATH set? Got " << loaded << " plugins";

  ASSERT_NE(host->find_node_factory("basic.text.constant"), nullptr);
  ASSERT_NE(host->find_node_factory("tl.frontend"),         nullptr);
  ASSERT_NE(host->find_node_factory("tl.irgen"),            nullptr);
  ASSERT_NE(host->find_node_factory("x86_64.nasm_gen"),     nullptr);
  ASSERT_NE(host->find_node_factory("x86_64.assemble"),     nullptr);
  ASSERT_NE(host->find_node_factory("basic.exec"),          nullptr);

  cc::runtime::graph g;

  // --- Build the pipeline nodes. ------------------------------------------
  std::string constant_id = add_node(g, *host, "basic.text.constant");
  std::string frontend_id = add_node(g, *host, "tl.frontend");
  std::string irgen_id    = add_node(g, *host, "tl.irgen");
  std::string nasm_id     = add_node(g, *host, "x86_64.nasm_gen");
  std::string asm_id      = add_node(g, *host, "x86_64.assemble");
  std::string exec_id     = add_node(g, *host, "basic.exec");

  // Source: hard-coded "return 42;"
  {
    auto* n = g.find_node(constant_id);
    ASSERT_NE(n, nullptr);
    n->properties().set("value", "return 42;");
  }

  // Assemble: write the ELF binary to a fresh temp path.
  auto exe_path = std::filesystem::temp_directory_path()
                / "cc_pipeline_test_return42";
  std::error_code rm_ec;
  std::filesystem::remove(exe_path, rm_ec);
#ifdef _WIN32
  // MinGW gcc appends ".exe" to the output name; clear both forms.
  { std::filesystem::path with_ext = exe_path; with_ext += ".exe"; std::filesystem::remove(with_ext, rm_ec); }
#endif
  {
    auto* n = g.find_node(asm_id);
    ASSERT_NE(n, nullptr);
    n->properties().set("out_path", exe_path.string());
  }

  // --- Wire edges: out → in. ----------------------------------------------
  connect(g, constant_id, "out", frontend_id, "src");
  connect(g, frontend_id, "ast", irgen_id,    "ast");
  connect(g, irgen_id,    "ir",  nasm_id,     "ir");
  connect(g, nasm_id,     "asm", asm_id,      "asm");
  connect(g, asm_id,      "exe", exec_id,     "exe");

  // --- Evaluate: pull Exec.ret_code. --------------------------------------
  cc::runtime::runner r{g, [](std::string_view msg) {
    std::fprintf(stderr, "[pipeline] %.*s\n",
                 static_cast<int>(msg.size()), msg.data());
  }};

  auto result = r.pull(exec_id, "ret_code");
  ASSERT_TRUE(result.has_value()) << "pull failed: " << result.error().what;

  const cc::any_value* v = *result;
  ASSERT_NE(v, nullptr);
  ASSERT_TRUE(v->has_value());
  const auto* code = aa::any_cast<long>(v);
  ASSERT_NE(code, nullptr);
  EXPECT_EQ(*code, 42);

  // Sanity: the binary should exist at the requested path.
#ifdef _WIN32
  { std::filesystem::path with_ext = exe_path; with_ext += ".exe"; EXPECT_TRUE(std::filesystem::exists(with_ext)); }
#else
  EXPECT_TRUE(std::filesystem::exists(exe_path));
#endif
}

// Smoke test: text.constant → text.from_file path round-trips through View.
// Verifies that wire type `text` and `path` registrations don't collide.
TEST(cc_pipeline, text_constant_emits_string) {
  cc::runtime::plugin_loader loader;  // outlives host (see teardown note above)
  auto host = cc::runtime::make_host_registry();
  ASSERT_GE(loader.load_all(*host), 1u);

  cc::runtime::graph g;
  std::string id = add_node(g, *host, "basic.text.constant");
  g.find_node(id)->properties().set("value", "hello world");

  cc::runtime::runner r{g};
  auto result = r.pull(id, "out");
  ASSERT_TRUE(result.has_value()) << result.error().what;
  const auto* s = aa::any_cast<std::string>(*result);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(*s, "hello world");
}
