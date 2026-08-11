// Unit tests for pipeline_xml: serialise a runtime::graph + canvas positions
// to XML, parse it back, verify the round-trip preserves nodes / properties /
// edges / positions / <requires>.
//
// Loads the same cc-plugin-*.so plugins as the workbench so the test exercises
// real factories with create_with_id() override.

#include "cc/graph.hpp"
#include "cc/host_registry.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_loader.hpp"
#include "cc/runner.hpp"
#include "cc/any_value.hpp"
#include "pipeline_xml.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace cw = cc::workbench;

namespace {

// Shared fixture: boot the host, load every plugin once.
class pipeline_xml_fixture : public ::testing::Test {
 protected:
  void SetUp() override {
    host_ = cc::runtime::make_host_registry();
    cc::runtime::plugin_loader loader;
    const std::size_t loaded = loader.load_all(*host_);
    ASSERT_GE(loaded, 4u) << "expected basic/tl/tl-ir/x86_64 plugins; got "
                          << loaded;
  }

  // Add a node of the given type and capture its instance_id.
  auto add_node(cc::runtime::graph& g, std::string_view type_id) -> std::string {
    auto* f = host_->find_node_factory(type_id);
    if (!f) ADD_FAILURE() << "missing factory for " << type_id;
    auto n = f->create();
    std::string id{n->instance_id()};
    g.add_node(std::move(n));
    return id;
  }

  std::unique_ptr<cc::host_registry> host_;
};

// Match the helper in pipeline_xml.cpp: parse a piece of XML in-memory to
// validate behaviour on synthetic inputs (no disk I/O required).
auto write_file(const std::string& path, const std::string& content) -> void {
  std::ofstream os{path, std::ios::binary | std::ios::trunc};
  os << content;
}

}  // namespace

// ----- create_with_id preserves the passed instance_id --------------------

TEST_F(pipeline_xml_fixture, create_with_id_keeps_passed_id) {
  auto* f = host_->find_node_factory("basic.text.constant");
  ASSERT_NE(f, nullptr);
  auto n = f->create_with_id("custom-instance-id-42");
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(std::string{n->instance_id()}, "custom-instance-id-42");
}

TEST_F(pipeline_xml_fixture, create_with_id_stamps_default_properties) {
  auto* f = host_->find_node_factory("basic.text.constant");
  ASSERT_NE(f, nullptr);
  auto n = f->create_with_id("id");
  // Default property value should already be applied so the user sees a sane
  // initial state in the editor.
  EXPECT_EQ(std::string{n->properties().get("value")}, "");
}

// ----- empty graph round-trip ---------------------------------------------

TEST_F(pipeline_xml_fixture, empty_graph_round_trip) {
  cc::runtime::graph g;
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_empty.pipeline";
  auto save = cw::save_pipeline(*host_, g, {}, tmp.string());
  ASSERT_TRUE(save.has_value()) << save.error();

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  EXPECT_TRUE(g2.nodes().empty());
  EXPECT_TRUE(g2.edges().empty());
  EXPECT_TRUE(load->positions.empty());
  EXPECT_TRUE(load->warnings.missing_plugins.empty());
  EXPECT_TRUE(load->warnings.unknown_node_types.empty());
  EXPECT_TRUE(load->warnings.skipped_edges.empty());
}

// ----- single node round-trips with properties + position -----------------

TEST_F(pipeline_xml_fixture, single_node_round_trip) {
  cc::runtime::graph g;
  std::string id = add_node(g, "basic.text.constant");
  g.find_node(id)->properties().set("value", "return 7;");
  // Round-trip through a different factory to verify the id is preserved.
  ASSERT_NE(id, std::string{}) << "precondition: instance id is non-empty";

  std::unordered_map<std::string, cw::pos> positions{{id, {120.5f, 340.0f}}};
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_one.pipeline";

  auto save = cw::save_pipeline(*host_, g, positions, tmp.string());
  ASSERT_TRUE(save.has_value()) << save.error();

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 1u);
  EXPECT_EQ(std::string{g2.nodes()[0]->type_id()}, "basic.text.constant");
  EXPECT_EQ(std::string{g2.nodes()[0]->instance_id()}, id);
  EXPECT_EQ(std::string{g2.nodes()[0]->properties().get("value")}, "return 7;");

  auto pit = load->positions.find(id);
  ASSERT_NE(pit, load->positions.end());
  EXPECT_FLOAT_EQ(pit->second.x, 120.5f);
  EXPECT_FLOAT_EQ(pit->second.y, 340.0f);
}

// ----- edges survive the round-trip ---------------------------------------

TEST_F(pipeline_xml_fixture, edge_round_trip) {
  cc::runtime::graph g;
  std::string src = add_node(g, "basic.text.constant");
  std::string dst = add_node(g, "tl.frontend");
  g.add_edge({src, "out", dst, "src"});

  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_edge.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, tmp.string()).has_value());

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.edges().size(), 1u);
  const auto& e = g2.edges()[0];
  EXPECT_EQ(std::string{e.src_node}, src);
  EXPECT_EQ(std::string{e.src_slot}, "out");
  EXPECT_EQ(std::string{e.dst_node}, dst);
  EXPECT_EQ(std::string{e.dst_slot}, "src");
}

// ----- <requires> lists the providers of every node type in the graph -----

TEST_F(pipeline_xml_fixture, requires_section_lists_providers) {
  cc::runtime::graph g;
  add_node(g, "basic.text.constant");   // provider = basic
  add_node(g, "tl.frontend");           // provider = tl

  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_req.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, tmp.string()).has_value());

  // Read back the XML and inspect the <requires> block directly — we don't
  // expose it through the load API, but it's the contract the workbench UI
  // relies on to tell the user which plugins it needs.
  std::ifstream is{tmp};
  ASSERT_TRUE(is.good());
  std::string xml((std::istreambuf_iterator<char>(is)),
                   std::istreambuf_iterator<char>());
  EXPECT_NE(xml.find("<requires>"), std::string::npos);
  EXPECT_NE(xml.find("name=\"basic\""), std::string::npos);
  EXPECT_NE(xml.find("name=\"tl\""), std::string::npos);
}

// ----- load surfaces missing plugins as warnings (not errors) -------------

TEST_F(pipeline_xml_fixture, missing_plugin_is_warning_not_error) {
  // Hand-craft a pipeline that requires a plugin the host never loaded. The
  // easiest way is to claim a type_id that no factory provides: it will be
  // skipped, and its edges also skipped — but the load should still succeed.
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_missing.pipeline";
  static constexpr std::string_view kXml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<pipeline version=\"1\">\n"
      "  <requires>\n"
      "    <plugin name=\"ghost-plugin\"/>\n"
      "  </requires>\n"
      "  <nodes>\n"
      "    <node type=\"ghost.ghost_node\" id=\"ghost#1\">\n"
      "      <properties/>\n"
      "    </node>\n"
      "  </nodes>\n"
      "  <edges/>\n"
      "</pipeline>\n";
  write_file(tmp.string(), std::string{kXml});

  cc::runtime::graph g;
  auto load = cw::load_pipeline(*host_, g, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  EXPECT_EQ(g.nodes().size(), 0u);
  ASSERT_EQ(load->warnings.missing_plugins.size(), 1u);
  EXPECT_EQ(load->warnings.missing_plugins[0], "ghost-plugin");
  ASSERT_EQ(load->warnings.unknown_node_types.size(), 1u);
}

// ----- bad XML is a hard error --------------------------------------------

TEST_F(pipeline_xml_fixture, malformed_xml_is_error) {
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_bad.pipeline";
  write_file(tmp.string(), "<pipeline version=\"1\"><nodes><not-closed...");
  cc::runtime::graph g;
  auto load = cw::load_pipeline(*host_, g, tmp.string());
  ASSERT_FALSE(load.has_value());
  EXPECT_NE(load.error().find("XML parse error"), std::string::npos);
}

TEST_F(pipeline_xml_fixture, missing_file_is_error) {
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_no_such.pipeline";
  std::error_code ec;
  std::filesystem::remove(tmp, ec);

  cc::runtime::graph g;
  auto load = cw::load_pipeline(*host_, g, tmp.string());
  ASSERT_FALSE(load.has_value());
  EXPECT_NE(load.error().find("file not found"), std::string::npos);
}

TEST_F(pipeline_xml_fixture, unsupported_version_is_error) {
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_ver.pipeline";
  static constexpr std::string_view kXml =
      "<?xml version=\"1.0\"?>\n"
      "<pipeline version=\"9999\"/>\n";
  write_file(tmp.string(), std::string{kXml});
  cc::runtime::graph g;
  auto load = cw::load_pipeline(*host_, g, tmp.string());
  ASSERT_FALSE(load.has_value());
  EXPECT_NE(load.error().find("unsupported pipeline version"),
            std::string::npos);
}

// ----- end-to-end: build, save, clear, load, run = 42 ---------------------
// This is the smoke test the user asked for: prove that a pipeline file
// produced by Save can be Loaded back into a fresh graph and produce the
// same exit code. It exercises every layer of the new code at once:
//   - create_with_id preserves instance_ids so the edges resolve
//   - <requires> + <properties> round-trip
//   - the runner can drive the restored graph

TEST_F(pipeline_xml_fixture, save_clear_load_run_produces_42) {
  cc::runtime::graph g;

  auto constant_id = add_node(g, "basic.text.constant");
  auto frontend_id = add_node(g, "tl.frontend");
  auto irgen_id    = add_node(g, "tl.irgen");
  auto nasm_id     = add_node(g, "x86_64.nasm_gen");
  auto asm_id      = add_node(g, "x86_64.assemble");
  auto exec_id     = add_node(g, "basic.exec");

  g.find_node(constant_id)->properties().set("value", "return 42;");
  auto exe_path = std::filesystem::temp_directory_path()
                / "cc_pipeline_xml_e2e_return42";
  std::error_code rm;
  std::filesystem::remove(exe_path, rm);
  g.find_node(asm_id)->properties().set("out_path", exe_path.string());

  g.add_edge({constant_id, "out", frontend_id, "src"});
  g.add_edge({frontend_id, "ast", irgen_id,    "ast"});
  g.add_edge({irgen_id,    "ir",  nasm_id,     "ir"});
  g.add_edge({nasm_id,     "asm", asm_id,      "asm"});
  g.add_edge({asm_id,      "exe", exec_id,     "exe"});

  // Save with some fake positions to also exercise the pos round-trip.
  std::unordered_map<std::string, cw::pos> positions{
      {constant_id, {100, 100}},
      {frontend_id, {300, 100}},
      {irgen_id,    {500, 100}},
      {nasm_id,     {700, 100}},
      {asm_id,      {900, 100}},
      {exec_id,     {1100, 100}},
  };
  auto tmp = std::filesystem::temp_directory_path() / "cc_pipeline_xml_e2e.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, positions, tmp.string()).has_value());

  // Drop the in-memory graph and load it back from disk — simulates File →
  // Open after File → Save in the workbench, including a fresh process.
  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 6u) << "expected every node to round-trip";
  ASSERT_EQ(g2.edges().size(), 5u) << "expected every edge to round-trip";
  ASSERT_EQ(load->positions.size(), 6u)
      << "expected every position to round-trip";
  EXPECT_TRUE(load->warnings.missing_plugins.empty());
  EXPECT_TRUE(load->warnings.unknown_node_types.empty());
  EXPECT_TRUE(load->warnings.skipped_edges.empty());

  // The assemble node's `out_path` must survive the round-trip — otherwise
  // the runner below would fail when the node tries to write the ELF binary.
  std::string restored_asm_id;
  std::string restored_exec_id;
  for (auto const& n : g2.nodes()) {
    if (n->type_id() == std::string_view{"x86_64.assemble"}) {
      restored_asm_id = n->instance_id();
      EXPECT_EQ(std::string{n->properties().get("out_path")}, exe_path.string());
    }
    if (n->type_id() == std::string_view{"basic.exec"}) {
      restored_exec_id = n->instance_id();
    }
  }
  ASSERT_FALSE(restored_asm_id.empty());
  ASSERT_FALSE(restored_exec_id.empty());

  cc::runtime::runner r{g2};
  auto result = r.pull(restored_exec_id, "ret_code");
  ASSERT_TRUE(result.has_value()) << "pull failed: " << result.error().what;
  const cc::any_value* v = *result;
  ASSERT_NE(v, nullptr);
  ASSERT_TRUE(v->has_value());
  const auto* code = aa::any_cast<long>(v);
  ASSERT_NE(code, nullptr);
  EXPECT_EQ(*code, 42);

  // Clean up the artifacts.
  std::filesystem::remove(tmp, rm);
  std::filesystem::remove(exe_path, rm);
}

// ----- round-trip is verbatim: relative stays relative, absolute stays absolute -----

TEST_F(pipeline_xml_fixture, save_preserves_relative_path_verbatim) {
  cc::runtime::graph g;
  std::string id = add_node(g, "basic.text.from_file");
  // A user-typed relative path: must stay exactly this string through save
  // and load — resolution against the pipeline's directory is activate()'s
  // job, not the storage layer's.
  g.find_node(id)->properties().set("path", "./input.txt");

  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_verbatim_rel.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, tmp.string()).has_value());

  // On-disk form: the relative path is written verbatim.
  std::ifstream is{tmp};
  ASSERT_TRUE(is.good());
  std::string xml((std::istreambuf_iterator<char>(is)),
                   std::istreambuf_iterator<char>());
  EXPECT_NE(xml.find(">./input.txt</property>"), std::string::npos)
      << "expected stored path verbatim; XML was:\n" << xml;

  // Round-trip through load: the in-memory property string is identical to
  // what the user typed. No expansion, no canonicalisation.
  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 1u);
  EXPECT_EQ(std::string{g2.nodes()[0]->properties().get("path")}, "./input.txt");
}

TEST_F(pipeline_xml_fixture, save_preserves_absolute_path_verbatim) {
  cc::runtime::graph g;
  std::string id = add_node(g, "basic.text.from_file");
  std::string abs_path =
      (std::filesystem::temp_directory_path() / "cc_xml_abs_input.txt").string();
  g.find_node(id)->properties().set("path", abs_path);

  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_verbatim_abs.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, tmp.string()).has_value());

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 1u);
  EXPECT_EQ(std::string{g2.nodes()[0]->properties().get("path")}, abs_path)
      << "absolute path must round-trip unchanged";
}

// ----- relocatable payoff: the same file works from two different directories -----
//
// The .pipeline file stores `./source.txt`. We copy file + input into a
// second directory, load from there, and ask the runner to activate. The
// node resolves `./source.txt` against the *destination* pipeline_dir
// (forwarded via runner constructor), reading the destination's copy. This
// is the "no surprise" UX: the property text stays what the user typed, and
// the right thing happens at runtime.

TEST_F(pipeline_xml_fixture, runner_resolves_relative_path_via_pipeline_dir) {
  auto root = std::filesystem::temp_directory_path() / "cc_xml_runner_resolve";
  std::filesystem::remove_all(root);
  auto src_dir = root / "src";
  auto dst_dir = root / "dst";
  std::filesystem::create_directories(src_dir);
  std::filesystem::create_directories(dst_dir);

  // Place a different content in each directory's source.txt — that way we
  // can tell which one the runner actually read.
  auto src_input = src_dir / "source.txt";
  auto dst_input = dst_dir / "source.txt";
  write_file(src_input.string(), "from_src");
  write_file(dst_input.string(), "from_dst");

  cc::runtime::graph g;
  std::string id = add_node(g, "basic.text.from_file");
  g.find_node(id)->properties().set("path", "./source.txt");  // relative — user's choice

  auto src_pipeline = src_dir / "graph.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, src_pipeline.string()).has_value());

  // Copy into dst. The .pipeline file is byte-identical (so the property text
  // is still "./source.txt" — round-trip preserves it), and source.txt comes
  // along for the ride.
  std::filesystem::copy_file(src_pipeline, dst_dir / "graph.pipeline",
                              std::filesystem::copy_options::overwrite_existing);

  // Load from dst_dir — the in-memory property is still "./source.txt".
  cc::runtime::graph g2;
  auto dst_pipeline = dst_dir / "graph.pipeline";
  auto load = cw::load_pipeline(*host_, g2, dst_pipeline.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 1u);
  EXPECT_EQ(std::string{g2.nodes()[0]->properties().get("path")}, "./source.txt")
      << "round-trip must preserve the relative text the user typed";

  // Now pull. The runner constructed with pipeline_dir = dst_dir resolves
  // "./source.txt" against dst_dir, so the node reads dst_dir/source.txt and
  // emits "from_dst" — NOT src_dir/source.txt.
  std::string loaded_id{g2.nodes()[0]->instance_id()};
  cc::runtime::runner r{g2, {}, dst_dir.string()};
  auto result = r.pull(loaded_id, "out");
  ASSERT_TRUE(result.has_value()) << result.error().what;
  const cc::any_value* v = *result;
  ASSERT_NE(v, nullptr);
  const auto* s = aa::any_cast<std::string>(v);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(*s, "from_dst")
      << "runner with pipeline_dir=dst must resolve relative paths against "
      << "dst_dir, not the cwd or the original src_dir";

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}
