// End-to-end pipeline tests (no GUI).
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
// Also covers the vocabulary-domain registry (closure, membership) and the
// inline pin-value injection performed by the runner.
//
// Requires the plugins to be built and discoverable via CCP_PLUGIN_PATH
// (set by the CMake test properties).

#include "cc/any_value.hpp"
#include "cc/graph.hpp"
#include "cc/host_registry.hpp"
#include "cc/inline_editors.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_loader.hpp"
#include "cc/runner.hpp"
#include "cc/types/filesystem.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// Tiny file helper for the inline-editor tests.
void write_bytes(const std::filesystem::path& p, std::string_view content) {
  std::ofstream os{p, std::ios::binary | std::ios::trunc};
  os << content;
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
  cc::runtime::register_inline_editors(*host);
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
  connect(g, constant_id, "text", frontend_id, "src");
  connect(g, frontend_id, "ast", irgen_id,    "ast");
  connect(g, irgen_id,    "ir",  nasm_id,     "ir");
  connect(g, nasm_id,     "asm", asm_id,      "asm");
  connect(g, asm_id,      "file", exec_id,    "file");

  // --- Evaluate: pull Exec.ret_code. --------------------------------------
  cc::runtime::runner r{g, [](std::string_view msg) {
    std::fprintf(stderr, "[pipeline] %.*s\n",
                 static_cast<int>(msg.size()), msg.data());
  }, {}, &host->types()};

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

// Smoke test: text.constant emits a String on its "text" slot.
TEST(cc_pipeline, text_constant_emits_string) {
  cc::runtime::plugin_loader loader;  // outlives host (see teardown note above)
  auto host = cc::runtime::make_host_registry();
  ASSERT_GE(loader.load_all(*host), 1u);
  cc::runtime::register_inline_editors(*host);

  cc::runtime::graph g;
  std::string id = add_node(g, *host, "basic.text.constant");
  g.find_node(id)->properties().set("value", "hello world");

  cc::runtime::runner r{g};
  auto result = r.pull(id, "text");
  ASSERT_TRUE(result.has_value()) << result.error().what;
  const auto* s = aa::any_cast<std::string>(*result);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(*s, "hello world");
}

// Vocabulary domains: the seeded domain set, dependency closure and factory
// membership all survive a real multi-plugin load.
TEST(cc_pipeline, domain_registry_closure_and_membership) {
  cc::runtime::plugin_loader loader;
  auto host = cc::runtime::make_host_registry();
  ASSERT_GE(loader.load_all(*host), 4u);
  cc::runtime::register_inline_editors(*host);

  // Seeded domains exist.
  for (std::string_view id : {"basic/types", "filesystem", "basic/text",
                              "basic/view", "system/process",
                              "compiler/lang/tl", "compiler/backend/x86_64"}) {
    EXPECT_NE(host->find_domain(id), nullptr) << "domain " << id << " missing";
  }

  // Closure of the tl-compiler domain pulls in its declared deps.
  const std::string_view roots[] = {"compiler/lang/tl"};
  auto closure = host->domain_closure(roots);
  auto contains = [&closure](std::string_view id) {
    return std::find(closure.begin(), closure.end(), id) != closure.end();
  };
  EXPECT_TRUE(contains("compiler/lang/tl"));
  EXPECT_TRUE(contains("basic/types"));
  EXPECT_TRUE(contains("filesystem"));
  EXPECT_TRUE(contains("basic/view"));
  // Sibling compiler domains are NOT pulled in implicitly.
  EXPECT_FALSE(contains("compiler/backend/x86_64"));
  EXPECT_FALSE(contains("system/process"));

  // Factory membership: exec lives in system/process, frontend in tl.
  auto in_domain = [&](std::string_view type_id, std::string_view domain) {
    auto* f = host->find_node_factory(type_id);
    if (!f) { ADD_FAILURE() << "no factory " << type_id; return false; }
    for (auto d : f->domains())
      if (d == domain) return true;
    return false;
  };
  EXPECT_TRUE(in_domain("basic.exec", "system/process"));
  EXPECT_TRUE(in_domain("tl.frontend", "compiler/lang/tl"));
  EXPECT_TRUE(in_domain("x86_64.assemble", "compiler/backend/x86_64"));
  EXPECT_TRUE(in_domain("filesystem.get_file", "filesystem"));

  // Type attribution: basic/types provides the primitives.
  const auto* bt = host->find_domain("basic/types");
  ASSERT_NE(bt, nullptr);
  auto provides = [&](std::string_view name) {
    return std::find(bt->provided_types.begin(), bt->provided_types.end(),
                     name) != bt->provided_types.end();
  };
  EXPECT_TRUE(provides("String"));
  EXPECT_TRUE(provides("Integer"));
  EXPECT_TRUE(provides("Boolean"));
  const auto* fsdom = host->find_domain("filesystem");
  ASSERT_NE(fsdom, nullptr);
  EXPECT_TRUE(std::find(fsdom->provided_types.begin(),
                        fsdom->provided_types.end(),
                        "File") != fsdom->provided_types.end());

  // Short names resolve for pin annotations.
  EXPECT_EQ(host->types().short_name_of(
                host->types().descriptor_of_name("String")), "str");
  EXPECT_EQ(host->types().short_name_of(
                host->types().descriptor_of_name("File")), "file");
}

// The filesystem chain with an inline pin value: get_file.path is NOT wired —
// the runner parses the slot_values() text via the type's inline editor and
// injects it, then read_text turns the handle into String content.
TEST(cc_pipeline, inline_value_injection_and_file_chain) {
  cc::runtime::plugin_loader loader;
  auto host = cc::runtime::make_host_registry();
  ASSERT_GE(loader.load_all(*host), 1u);
  cc::runtime::register_inline_editors(*host);

  auto tmp = std::filesystem::temp_directory_path() / "cc_inline_test.txt";
  {
    std::ofstream os{tmp};
    os << "inline works";
  }

  cc::runtime::graph g;
  std::string get_id  = add_node(g, *host, "filesystem.get_file");
  std::string read_id = add_node(g, *host, "filesystem.read_text");
  connect(g, get_id, "file", read_id, "file");

  // No wire on get_file.path — feed it inline.
  g.find_node(get_id)->slot_values().set("path", tmp.string());

  // Without a type registry the runner must fail (required slot unconnected).
  {
    cc::runtime::runner r{g};
    auto res = r.pull(read_id, "text");
    ASSERT_FALSE(res.has_value());
  }
  // With the registry the inline value is parsed and injected.
  cc::runtime::runner r{g, {}, tmp.parent_path().string(), &host->types()};
  auto res = r.pull(read_id, "text");
  ASSERT_TRUE(res.has_value()) << res.error().what;
  const auto* s = aa::any_cast<std::string>(*res);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(*s, "inline works");

  // A bad inline value surfaces the validator's message.
  g.find_node(get_id)->slot_values().set("path", "");
  {
    cc::runtime::runner r2{g, {}, {}, &host->types()};
    auto res2 = r2.pull(read_id, "text");
    ASSERT_FALSE(res2.has_value());
    EXPECT_NE(res2.error().what.find("path"), std::string::npos)
        << res2.error().what;
  }
}

// basic/types ships its own nodes: typed constants (let-pattern — the inline
// `in` pin IS the value editor) populate the Basic Types palette section, so
// importing the domain yields usable sources, not just connection vocabulary.
TEST(cc_pipeline, basic_types_constant_nodes) {
  cc::runtime::plugin_loader loader;
  auto host = cc::runtime::make_host_registry();
  ASSERT_GE(loader.load_all(*host), 1u);
  cc::runtime::register_inline_editors(*host);

  // The four factories exist and are members of basic/types (palette feed).
  for (std::string_view id : {"basic.types.string", "basic.types.integer",
                              "basic.types.double", "basic.types.boolean"}) {
    const auto* f = host->find_node_factory(id);
    ASSERT_NE(f, nullptr) << id;
    bool member = false;
    for (auto d : f->domains()) member = member || d == "basic/types";
    EXPECT_TRUE(member) << id;
  }

  cc::runtime::graph g;
  std::string s = add_node(g, *host, "basic.types.string");
  std::string i = add_node(g, *host, "basic.types.integer");
  std::string d = add_node(g, *host, "basic.types.double");
  std::string b = add_node(g, *host, "basic.types.boolean");
  g.find_node(s)->slot_values().set("in", "hello");
  g.find_node(i)->slot_values().set("in", "42");
  g.find_node(d)->slot_values().set("in", "2.5");
  g.find_node(b)->slot_values().set("in", "true");

  cc::runtime::runner r{g, {}, {}, &host->types()};
  auto rs = r.pull(s, "value");
  ASSERT_TRUE(rs.has_value()) << rs.error().what;
  const auto* vs = aa::any_cast<std::string>(*rs);
  ASSERT_NE(vs, nullptr);
  EXPECT_EQ(*vs, "hello");
  auto ri = r.pull(i, "value");
  ASSERT_TRUE(ri.has_value()) << ri.error().what;
  const auto* vi = aa::any_cast<long>(*ri);
  ASSERT_NE(vi, nullptr);
  EXPECT_EQ(*vi, 42);
  auto rd = r.pull(d, "value");
  ASSERT_TRUE(rd.has_value()) << rd.error().what;
  const auto* vd = aa::any_cast<double>(*rd);
  ASSERT_NE(vd, nullptr);
  EXPECT_DOUBLE_EQ(*vd, 2.5);
  auto rb = r.pull(b, "value");
  ASSERT_TRUE(rb.has_value()) << rb.error().what;
  const auto* vb = aa::any_cast<bool>(*rb);
  ASSERT_NE(vb, nullptr);
  EXPECT_TRUE(*vb);

  // No inline text and no wire → helpful failure naming the remedies.
  g.find_node(s)->slot_values().set("in", "");
  cc::runtime::runner r2{g, {}, {}, &host->types()};
  auto bad = r2.pull(s, "value");
  ASSERT_FALSE(bad.has_value());
  EXPECT_NE(bad.error().what.find("value not set"), std::string::npos)
      << bad.error().what;
}

// Named value-editor catalog ("open with editor…"): type name → editor ids,
// host-layer like inline editors. String offers "Text"; types without
// editors report an empty list; registration is deduped.
TEST(cc_pipeline, value_editor_catalog) {
  cc::runtime::plugin_loader loader;
  auto host = cc::runtime::make_host_registry();
  ASSERT_GE(loader.load_all(*host), 1u);
  cc::runtime::register_inline_editors(*host);
  cc::runtime::register_value_editors(*host);

  const auto str_d = host->types().descriptor_of_name("String");
  ASSERT_TRUE(str_d != cc::type_descriptor_t{});
  auto editors = host->types().value_editors_of(str_d);
  ASSERT_EQ(editors.size(), 2u);
  EXPECT_EQ(editors[0], "editors.text.plain");
  EXPECT_EQ(editors[1], "editors.text.code");

  // Idempotent registration does not duplicate entries.
  cc::runtime::register_value_editors(*host);
  EXPECT_EQ(host->types().value_editors_of(str_d).size(), 2u);

  // Types without full-size editors — empty catalog.
  const auto int_d = host->types().descriptor_of_name("Integer");
  EXPECT_TRUE(host->types().value_editors_of(int_d).empty());
  EXPECT_TRUE(host->types()
                  .value_editors_of(host->types().descriptor_of_name(
                      "NoSuchType"))
                  .empty());

  // Path (footer properties map onto it) offers the plain text editor —
  // the SAME id as String's entry: editor ids are global identities.
  const auto path_d = host->types().descriptor_of_name("Path");
  auto path_editors = host->types().value_editors_of(path_d);
  ASSERT_EQ(path_editors.size(), 1u);
  EXPECT_EQ(path_editors[0], "editors.text.plain");
}

// The host-layer File editor: the inline text is resolved against
// pipeline_dir and stat-validated into a real file_handle BEFORE the graph
// runs — every :file input (e.g. exec's) gets it, uniformly.
TEST(cc_pipeline, file_inline_editor_materialises_handle) {
  cc::runtime::plugin_loader loader;
  auto host = cc::runtime::make_host_registry();
  ASSERT_GE(loader.load_all(*host), 1u);
  cc::runtime::register_inline_editors(*host);

  auto tmp = std::filesystem::temp_directory_path() / "cc_file_inline.bin";
  write_bytes(tmp, "bin");

  const auto file_t = host->types().descriptor_of_name("File");
  ASSERT_TRUE(file_t != cc::type_descriptor_t{});

  // Inline editor registered for the type — regardless of any node.
  EXPECT_EQ(host->types().inline_editor_of(file_t), cc::property_kind::path);

  // Relative text resolves against pipeline_dir, absolute passes through.
  auto rel = host->types().parse_value(
      file_t, "./cc_file_inline.bin",
      std::filesystem::temp_directory_path().string());
  ASSERT_TRUE(rel.has_value()) << rel.error();
  const auto* h = aa::any_cast<cc::fs::file_handle>(&*rel);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->path, tmp.lexically_normal());
  EXPECT_EQ(h->attrs.size, 3u);

  // Missing file → validator error mentioning the path.
  auto missing = host->types().parse_value(
      file_t, "./no_such_file.bin",
      std::filesystem::temp_directory_path().string());
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().find("does not exist"), std::string::npos);
}

// get_file fails loudly on a missing file (the Optional<File> None case)
// and get_or_create_file materialises an empty one instead.
TEST(cc_pipeline, get_file_strict_vs_get_or_create) {
  cc::runtime::plugin_loader loader;
  auto host = cc::runtime::make_host_registry();
  ASSERT_GE(loader.load_all(*host), 1u);
  cc::runtime::register_inline_editors(*host);

  auto missing = std::filesystem::temp_directory_path() / "cc_missing_no_such.file";
  std::error_code ec;
  std::filesystem::remove(missing, ec);

  cc::runtime::graph g;
  std::string strict = add_node(g, *host, "filesystem.get_file");
  g.find_node(strict)->slot_values().set("path", missing.string());
  {
    cc::runtime::runner r{g, {}, {}, &host->types()};
    auto res = r.pull(strict, "file");
    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().what.find("does not exist"), std::string::npos)
        << res.error().what;
  }

  cc::runtime::graph g2;
  std::string lenient = add_node(g2, *host, "filesystem.get_or_create_file");
  g2.find_node(lenient)->slot_values().set("path", missing.string());
  cc::runtime::runner r2{g2, {}, {}, &host->types()};
  auto res2 = r2.pull(lenient, "file");
  ASSERT_TRUE(res2.has_value()) << res2.error().what;
  const auto* h = aa::any_cast<cc::fs::file_handle>(*res2);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->path, missing.lexically_normal());
  EXPECT_TRUE(std::filesystem::exists(missing));
}
