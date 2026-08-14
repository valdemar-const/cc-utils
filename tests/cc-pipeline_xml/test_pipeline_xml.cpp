// Unit tests for pipeline_xml (format v2): serialise a runtime::graph +
// canvas positions + the domain contract to XML, parse it back, verify the
// round-trip preserves nodes / properties / inline values / edges /
// positions / <requires> / domain + imports. Also covers the legacy v1 mode
// and the domain-contract validation rules.
//
// Loads the same cc-plugin-*.so plugins as the workbench so the test exercises
// real factories with create_with_id() override.

#include "cc/graph.hpp"
#include "cc/host_registry.hpp"
#include "cc/inline_editors.hpp"
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
#include <string_view>
#include <unordered_map>

namespace cw = cc::workbench;

namespace {

// Shared fixture: boot the host, load every plugin once.
class pipeline_xml_fixture : public ::testing::Test {
 protected:
  void SetUp() override {
    host_ = cc::runtime::make_host_registry();
    const std::size_t loaded = loader_.load_all(*host_);
    ASSERT_GE(loaded, 4u) << "expected basic/tl/tl-ir/x86_64 plugins; got "
                          << loaded;
    cc::runtime::register_inline_editors(*host_);
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

  auto save(cc::runtime::graph& g, const std::string& path) -> bool {
    cw::pipeline_domains dom;
    dom.root = "compiler/lang/tl";
    dom.imports = {"compiler/backend/x86_64", "system/process"};
    return cw::save_pipeline(*host_, g, {}, dom, path).has_value();
  }

  // loader_ must be declared before (and so outlive) host_: host_ owns factory
  // objects whose vtables live in the plugin DLLs, freed by ~plugin_loader.
  // Members destruct in reverse declaration order.
  cc::runtime::plugin_loader loader_;
  std::unique_ptr<cc::host_registry> host_;
};

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
  ASSERT_TRUE(save(g, tmp.string()));

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  EXPECT_EQ(g2.nodes().size(), 0u);
  EXPECT_EQ(g2.edges().size(), 0u);
  EXPECT_EQ(load->domains.root, "compiler/lang/tl");
}

// ----- node + property + inline value round-trip ---------------------------

TEST_F(pipeline_xml_fixture, node_property_and_inline_value_round_trip) {
  cc::runtime::graph g;
  std::string id = add_node(g, "basic.text.constant");
  g.find_node(id)->properties().set("value", "return 7;");
  // Inline pin value on a get_file node (input slot "path").
  std::string gid = add_node(g, "filesystem.get_file");
  g.find_node(gid)->slot_values().set("path", "/tmp/somewhere.tl");

  std::unordered_map<std::string, cw::pos> positions{{id, {120.5f, 340.0f}}};
  cw::pipeline_domains dom;
  dom.root = "filesystem";
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_node.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, positions, dom, tmp.string())
                  .has_value());

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 2u);

  for (auto const& n : g2.nodes()) {
    if (n->type_id() == std::string_view{"basic.text.constant"}) {
      EXPECT_EQ(std::string{n->instance_id()}, id);
      EXPECT_EQ(std::string{n->properties().get("value")}, "return 7;");
      auto pit = load->positions.find(id);
      ASSERT_NE(pit, load->positions.end());
      EXPECT_FLOAT_EQ(pit->second.x, 120.5f);
      EXPECT_FLOAT_EQ(pit->second.y, 340.0f);
    }
    if (n->type_id() == std::string_view{"filesystem.get_file"}) {
      // <values> round-trip: the inline pin text survives verbatim.
      EXPECT_EQ(std::string{n->slot_values().get("path")}, "/tmp/somewhere.tl");
    }
  }
  EXPECT_EQ(load->domains.root, "filesystem");
  EXPECT_TRUE(load->domains.imports.empty());
}

// ----- edges survive the round-trip ---------------------------------------

TEST_F(pipeline_xml_fixture, edge_round_trip) {
  cc::runtime::graph g;
  std::string src = add_node(g, "filesystem.read_text");
  std::string dst = add_node(g, "tl.frontend");
  g.add_edge({src, "text", dst, "src"});

  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_edge.pipeline";
  ASSERT_TRUE(save(g, tmp.string()));

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.edges().size(), 1u);
  const auto& e = g2.edges()[0];
  EXPECT_EQ(std::string{e.src_node}, src);
  EXPECT_EQ(std::string{e.src_slot}, "text");
  EXPECT_EQ(std::string{e.dst_node}, dst);
  EXPECT_EQ(std::string{e.dst_slot}, "src");
}

// ----- <requires> lists the providers of every node type in the graph -----

TEST_F(pipeline_xml_fixture, requires_section_lists_providers) {
  cc::runtime::graph g;
  add_node(g, "basic.text.constant");   // provider = basic
  add_node(g, "tl.frontend");           // provider = tl

  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_req.pipeline";
  ASSERT_TRUE(save(g, tmp.string()));

  std::ifstream is{tmp};
  ASSERT_TRUE(is.good());
  std::string xml((std::istreambuf_iterator<char>(is)),
                   std::istreambuf_iterator<char>());
  EXPECT_NE(xml.find("<requires>"), std::string::npos);
  EXPECT_NE(xml.find("name=\"basic\""), std::string::npos);
  EXPECT_NE(xml.find("name=\"tl\""), std::string::npos);
}

// ----- save refuses a pipeline without a root domain -----------------------

TEST_F(pipeline_xml_fixture, save_without_domain_is_error) {
  cc::runtime::graph g;
  add_node(g, "basic.text.constant");
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_nodom.pipeline";
  cw::pipeline_domains dom;  // root empty
  auto res = cw::save_pipeline(*host_, g, {}, dom, tmp.string());
  ASSERT_FALSE(res.has_value());
  EXPECT_NE(res.error().find("root domain"), std::string::npos);
}

// ----- domain contract: missing root is a hard error, missing import a warning

TEST_F(pipeline_xml_fixture, missing_root_domain_is_hard_error) {
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_badroot.pipeline";
  static constexpr std::string_view kXml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<pipeline version=\"2\" domain=\"no/such/domain\">\n"
      "  <requires/>\n"
      "  <nodes/>\n"
      "  <edges/>\n"
      "</pipeline>\n";
  write_file(tmp.string(), std::string{kXml});

  cc::runtime::graph g;
  auto load = cw::load_pipeline(*host_, g, tmp.string());
  ASSERT_FALSE(load.has_value());
  EXPECT_NE(load.error().find("root domain"), std::string::npos);
}

TEST_F(pipeline_xml_fixture, missing_import_is_warning) {
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_badimp.pipeline";
  static constexpr std::string_view kXml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<pipeline version=\"2\" domain=\"filesystem\">\n"
      "  <imports>\n"
      "    <domain id=\"no/such/import\"/>\n"
      "  </imports>\n"
      "  <requires/>\n"
      "  <nodes/>\n"
      "  <edges/>\n"
      "</pipeline>\n";
  write_file(tmp.string(), std::string{kXml});

  cc::runtime::graph g;
  auto load = cw::load_pipeline(*host_, g, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  EXPECT_EQ(load->domains.root, "filesystem");
  ASSERT_EQ(load->domains.imports.size(), 1u);
  EXPECT_EQ(load->domains.imports[0], "no/such/import");
  ASSERT_EQ(load->warnings.missing_domains.size(), 1u);
}

// ----- legacy v1: loads with an empty contract + legacy flag ---------------

TEST_F(pipeline_xml_fixture, legacy_v1_loads_in_legacy_mode) {
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_legacy.pipeline";
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
  EXPECT_TRUE(load->legacy);
  EXPECT_TRUE(load->domains.root.empty());
  ASSERT_EQ(load->warnings.missing_plugins.size(), 1u);
  EXPECT_EQ(load->warnings.missing_plugins[0], "ghost-plugin");
  ASSERT_EQ(load->warnings.unknown_node_types.size(), 1u);
}

// ----- bad XML is a hard error --------------------------------------------

TEST_F(pipeline_xml_fixture, malformed_xml_is_error) {
  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_bad.pipeline";
  write_file(tmp.string(), "<pipeline version=\"2\"><nodes><not-closed...");
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

TEST_F(pipeline_xml_fixture, save_clear_load_run_produces_42) {
  cc::runtime::graph g;

  auto input_tl = std::filesystem::temp_directory_path() / "cc_xml_e2e_42.tl";
  write_file(input_tl.string(), "return 42;");

  auto path_id     = add_node(g, "filesystem.path");
  auto get_id      = add_node(g, "filesystem.get_file");
  auto read_id     = add_node(g, "filesystem.read_text");
  auto frontend_id = add_node(g, "tl.frontend");
  auto irgen_id    = add_node(g, "tl.irgen");
  auto nasm_id     = add_node(g, "x86_64.nasm_gen");
  auto asm_id      = add_node(g, "x86_64.assemble");
  auto exec_id     = add_node(g, "basic.exec");

  // Blender-style constant: the unconnected `in` pin carries an inline value.
  g.find_node(path_id)->slot_values().set("in", input_tl.string());

  auto exe_path = std::filesystem::temp_directory_path()
                / "cc_pipeline_xml_e2e_return42";
  std::error_code rm;
  std::filesystem::remove(exe_path, rm);
  g.find_node(asm_id)->properties().set("out_path", exe_path.string());

  g.add_edge({path_id,     "path", get_id,      "path"});
  g.add_edge({get_id,      "file", read_id,     "file"});
  g.add_edge({read_id,     "text", frontend_id, "src"});
  g.add_edge({frontend_id, "ast",  irgen_id,    "ast"});
  g.add_edge({irgen_id,    "ir",   nasm_id,     "ir"});
  g.add_edge({nasm_id,     "asm",  asm_id,      "asm"});
  g.add_edge({asm_id,      "file", exec_id,     "file"});

  std::unordered_map<std::string, cw::pos> positions{
      {path_id, {0, 100}},   {get_id, {200, 100}},
      {read_id, {400, 100}}, {frontend_id, {600, 100}},
      {irgen_id, {800, 100}}, {nasm_id, {1000, 100}},
      {asm_id, {1200, 100}},  {exec_id, {1400, 100}},
  };
  cw::pipeline_domains dom;
  dom.root = "compiler/lang/tl";
  dom.imports = {"compiler/backend/x86_64", "system/process"};
  auto tmp = std::filesystem::temp_directory_path() / "cc_pipeline_xml_e2e.pipeline";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, positions, dom, tmp.string())
                  .has_value());

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 8u) << "expected every node to round-trip";
  ASSERT_EQ(g2.edges().size(), 7u) << "expected every edge to round-trip";
  ASSERT_EQ(load->positions.size(), 8u);
  EXPECT_EQ(load->domains.root, "compiler/lang/tl");
  EXPECT_TRUE(load->warnings.missing_plugins.empty());
  EXPECT_TRUE(load->warnings.unknown_node_types.empty());
  EXPECT_TRUE(load->warnings.skipped_edges.empty());

  std::string restored_exec_id;
  for (auto const& n : g2.nodes()) {
    if (n->type_id() == std::string_view{"basic.exec"}) {
      restored_exec_id = n->instance_id();
    }
  }
  ASSERT_FALSE(restored_exec_id.empty());

  cc::runtime::runner r{g2, {}, std::filesystem::temp_directory_path().string(),
                        &host_->types()};
  auto result = r.pull(restored_exec_id, "ret_code");
  ASSERT_TRUE(result.has_value()) << "pull failed: " << result.error().what;
  const cc::any_value* v = *result;
  ASSERT_NE(v, nullptr);
  ASSERT_TRUE(v->has_value());
  const auto* code = aa::any_cast<long>(v);
  ASSERT_NE(code, nullptr);
  EXPECT_EQ(*code, 42);

  std::filesystem::remove(tmp, rm);
  std::filesystem::remove(exe_path, rm);
}

// ----- round-trip is verbatim: relative stays relative ---------------------

TEST_F(pipeline_xml_fixture, save_preserves_relative_path_verbatim) {
  cc::runtime::graph g;
  std::string id = add_node(g, "filesystem.path");
  g.find_node(id)->slot_values().set("in", "./input.txt");

  auto tmp = std::filesystem::temp_directory_path() / "cc_xml_verbatim_rel.pipeline";
  cw::pipeline_domains dom;
  dom.root = "filesystem";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, dom, tmp.string()).has_value());

  std::ifstream is{tmp};
  ASSERT_TRUE(is.good());
  std::string xml((std::istreambuf_iterator<char>(is)),
                   std::istreambuf_iterator<char>());
  EXPECT_NE(xml.find(">./input.txt</value>"), std::string::npos)
      << "expected stored inline value verbatim; XML was:\n" << xml;

  cc::runtime::graph g2;
  auto load = cw::load_pipeline(*host_, g2, tmp.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 1u);
  EXPECT_EQ(std::string{g2.nodes()[0]->slot_values().get("in")}, "./input.txt");
}

// ----- relocatable payoff: the same file works from two directories --------

TEST_F(pipeline_xml_fixture, runner_resolves_relative_path_via_pipeline_dir) {
  auto root = std::filesystem::temp_directory_path() / "cc_xml_runner_resolve";
  std::filesystem::remove_all(root);
  auto src_dir = root / "src";
  auto dst_dir = root / "dst";
  std::filesystem::create_directories(src_dir);
  std::filesystem::create_directories(dst_dir);

  auto src_input = src_dir / "source.txt";
  auto dst_input = dst_dir / "source.txt";
  write_file(src_input.string(), "from_src");
  write_file(dst_input.string(), "from_dst");

  // filesystem.path → get_file → read_text; the path inline value is relative.
  cc::runtime::graph g;
  std::string path_id = add_node(g, "filesystem.path");
  std::string get_id  = add_node(g, "filesystem.get_file");
  std::string read_id = add_node(g, "filesystem.read_text");
  g.find_node(path_id)->slot_values().set("in", "./source.txt");
  g.add_edge({path_id, "path", get_id, "path"});
  g.add_edge({get_id, "file", read_id, "file"});

  auto src_pipeline = src_dir / "graph.pipeline";
  cw::pipeline_domains dom;
  dom.root = "filesystem";
  ASSERT_TRUE(cw::save_pipeline(*host_, g, {}, dom, src_pipeline.string())
                  .has_value());

  std::filesystem::copy_file(src_pipeline, dst_dir / "graph.pipeline",
                              std::filesystem::copy_options::overwrite_existing);

  cc::runtime::graph g2;
  auto dst_pipeline = dst_dir / "graph.pipeline";
  auto load = cw::load_pipeline(*host_, g2, dst_pipeline.string());
  ASSERT_TRUE(load.has_value()) << load.error();
  ASSERT_EQ(g2.nodes().size(), 3u);
  for (auto const& n : g2.nodes()) {
    if (n->type_id() == std::string_view{"filesystem.path"}) {
      EXPECT_EQ(std::string{n->slot_values().get("in")}, "./source.txt")
          << "round-trip must preserve the relative text the user typed";
    }
  }

  std::string loaded_read_id;
  for (auto const& n : g2.nodes()) {
    if (n->type_id() == std::string_view{"filesystem.read_text"})
      loaded_read_id = n->instance_id();
  }
  ASSERT_FALSE(loaded_read_id.empty());

  cc::runtime::runner r{g2, {}, dst_dir.string(), &host_->types()};
  auto result = r.pull(loaded_read_id, "text");
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
