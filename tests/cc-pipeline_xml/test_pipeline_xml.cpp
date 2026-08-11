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

// ----- relocatable: absolute path-typed properties are stored relative -----

TEST_F(pipeline_xml_fixture, save_writes_path_property_relative_to_base_dir) {
  cc::runtime::graph g;
  std::string id = add_node(g, "basic.text.from_file");

  // Construct an absolute path that lives under base_dir.
  auto base_dir = std::filesystem::temp_directory_path() / "cc_xml_reloc_src";
  std::filesystem::create_directories(base_dir);
  auto input_under = base_dir / "src.txt";
  write_file(input_under.string(), "hello");

  g.find_node(id)->properties().set("path", input_under.string());

  auto tmp = base_dir / "graph.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, tmp.string(),
                                 /*base_dir=*/base_dir.string()).has_value());

  // Read the XML back as text and confirm the path is stored relative
  // (the canonical "lives in the same tree" case becomes just the filename).
  // We can't just substring-match `<property key="path">src.txt</property>`
  // because pugixml's format_indent_attributes puts the attribute on its own
  // line; check the closing tag instead and verify no absolute-path prefix
  // leaks through.
  std::ifstream is{tmp};
  ASSERT_TRUE(is.good());
  std::string xml((std::istreambuf_iterator<char>(is)),
                   std::istreambuf_iterator<char>());
  EXPECT_NE(xml.find(">src.txt</property>"), std::string::npos)
      << "expected stored path to be relative; XML was:\n"
      << xml;
  // Sanity: the absolute prefix of the input path must NOT appear in the
  // file — if it does, save didn't relativise.
  EXPECT_EQ(xml.find(input_under.parent_path().string()), std::string::npos)
      << "absolute directory leaked into stored path; XML was:\n"
      << xml;
}

TEST_F(pipeline_xml_fixture, outside_base_dir_uses_dotdot_relative) {
  cc::runtime::graph g;
  std::string id = add_node(g, "basic.text.from_file");

  // base_dir is a sibling of input_path's parent.
  auto root = std::filesystem::temp_directory_path() / "cc_xml_reloc_outside";
  std::filesystem::remove_all(root);
  auto input_dir   = root / "inputs";
  auto pipeline_dir = root / "pipelines";
  std::filesystem::create_directories(input_dir);
  std::filesystem::create_directories(pipeline_dir);
  auto input_path = input_dir / "in.txt";
  write_file(input_path.string(), "x");
  auto tmp = pipeline_dir / "graph.pipeline";

  g.find_node(id)->properties().set("path", input_path.string());
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, tmp.string(),
                                 /*base_dir=*/pipeline_dir.string()).has_value());

  // Load with base_dir = the directory we saved into; we should get back the
  // same absolute path, regardless of how it was stored.
  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string(),
                                 /*base_dir=*/pipeline_dir.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 1u);
  std::string restored{g2.nodes()[0]->properties().get("path")};
  EXPECT_EQ(std::filesystem::path(restored), input_path)
      << "expected round-trip to recover the original absolute path; got "
      << restored;

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

// ----- the relocatable payoff: move file + assets, paths still resolve -----

TEST_F(pipeline_xml_fixture, moving_pipeline_keeps_paths_working) {
  // Build the source pipeline in src_dir.
  auto root = std::filesystem::temp_directory_path() / "cc_xml_reloc_move";
  std::filesystem::remove_all(root);
  auto src_dir = root / "src";
  auto dst_dir = root / "dst";
  std::filesystem::create_directories(src_dir);
  std::filesystem::create_directories(dst_dir);

  // A text input that the graph references.
  auto src_input = src_dir / "source.txt";
  write_file(src_input.string(), "data");

  cc::runtime::graph g;
  std::string id = add_node(g, "basic.text.from_file");
  g.find_node(id)->properties().set("path", src_input.string());

  auto src_pipeline = src_dir / "graph.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, src_pipeline.string(),
                                 /*base_dir=*/src_dir.string()).has_value());

  // Copy the pipeline file and its referenced asset into dst_dir preserving
  // the relative layout (the file itself sits next to source.txt in both).
  std::filesystem::copy_file(src_pipeline, dst_dir / "graph.pipeline",
                              std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(src_input,    dst_dir / "source.txt",
                              std::filesystem::copy_options::overwrite_existing);

  // Now load from the *destination* location. The stored path is "source.txt"
  // (relative); the loader resolves it against dst_dir, so the absolute path
  // we get back points at dst_dir / "source.txt", not src_dir / "source.txt".
  cc::runtime::graph g2;
  auto dst_pipeline = dst_dir / "graph.pipeline";
  auto load = cw::load_pipeline(*host_, g2, dst_pipeline.string(),
                                 /*base_dir=*/dst_dir.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 1u);
  std::string restored{g2.nodes()[0]->properties().get("path")};
  EXPECT_EQ(std::filesystem::path(restored), dst_dir / "source.txt")
      << "relocatable load should resolve to the destination copy, not the "
      << "original source path; got " << restored;

  // Sanity: the file at the restored path must actually exist.
  EXPECT_TRUE(std::filesystem::exists(restored));

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}
