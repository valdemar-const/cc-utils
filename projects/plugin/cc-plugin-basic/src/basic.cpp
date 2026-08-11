// cc-plugin-basic — basic node plugin (no UI).
//
// Registers the "text" value type and two nodes:
//   - text.from_file: output slot "out" (text), property "path". Reads a file.
//   - view:           input slot "in" (any, multi), property "name". Debug tap.
//
// No view renderer here — renderers are UI code and live host-side (cc-workbench
// registers a text renderer for the "text" type at startup).

#include "cc/any_value.hpp"
#include "cc/host.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_entry.hpp"

#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace cc::basic {

namespace {

// Generate a fresh unique instance id for a node of the given type.
auto fresh_instance_id(std::string_view type_id) -> std::string {
  static std::atomic<unsigned> counter{0};
  unsigned n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return std::string{type_id} + "#" + std::to_string(n);
}

// Minimal property bag for plugin-local nodes.
class props final : public node_properties {
 public:
  auto get(std::string_view key) const -> std::string_view override {
    auto it = m_.find(std::string{key});
    return it == m_.end() ? std::string_view{} : it->second;
  }
  auto set(std::string_view key, std::string_view value) -> void override {
    m_[std::string{key}] = std::string{value};
  }
 private:
  std::unordered_map<std::string, std::string> m_;
};

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
class text_out_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "out"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::string>; }
  auto dir()  const -> slot_dir          override { return slot_dir::out; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class any_in_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "in"; }
  auto type() const -> type_descriptor_t override { return type_descriptor_t{}; }  // wildcard
  auto dir()  const -> slot_dir          override { return slot_dir::in; }
  auto card() const -> slot_card         override { return slot_card::multi; }
};

// ---------------------------------------------------------------------------
// text.from_file node — reads a file from the "path" property.
// ---------------------------------------------------------------------------
class from_file_node final : public node {
 public:
  auto type_id()     const -> std::string_view override { return "basic.text.from_file"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const text_out_slot s_out{};
    static constexpr slot const* arr[] = {&s_out};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  auto activate(std::span<const input_pair>,
                std::span<output_pair>      outputs) -> activate_result override {
    std::string path{props_.get("path")};
    if (path.empty()) {
      return std::unexpected(failure{"'path' not set"});
    }
    std::ifstream in(path);
    if (!in) {
      return std::unexpected(failure{"cannot open '" + path + "'"});
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    for (auto& [slot_id, out] : outputs) {
      if (slot_id == "out") {
        *out = ss.str();
        return {};
      }
    }
    return std::unexpected(failure{"no 'out' slot on " + id_});
  }

 private:
  std::string id_{fresh_instance_id("basic.text.from_file")};
  props       props_;
};

class from_file_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "basic.text.from_file"; }
  auto display_name() const -> std::string_view override { return "Source File"; }
  auto category()     const -> std::string_view override { return "Basic"; }

  auto property_schema() const -> std::span<const property_desc> override {
    static constexpr property_desc schema[] = {
      {"path", "File Path", property_kind::path, ""},
    };
    return schema;
  }

  auto create() const -> std::unique_ptr<node> override {
    auto n = std::make_unique<from_file_node>();
    for (auto const& d : property_schema()) {
      n->properties().set(d.key, d.default_value);
    }
    return n;
  }
};

// ---------------------------------------------------------------------------
// view node — debug tap, accepts any type.
// ---------------------------------------------------------------------------
class view_node final : public node {
 public:
  auto type_id()     const -> std::string_view override { return "basic.view"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const any_in_slot s_in{};
    static constexpr slot const* arr[] = {&s_in};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  // Sinks; the View panel pulls upstream value directly via the runner.
  auto activate(std::span<const input_pair>,
                std::span<output_pair>) -> activate_result override {
    return {};
  }

 private:
  std::string id_{fresh_instance_id("basic.view")};
  props       props_;
};

class view_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "basic.view"; }
  auto display_name() const -> std::string_view override { return "View"; }
  auto category()     const -> std::string_view override { return "Basic"; }

  auto property_schema() const -> std::span<const property_desc> override {
    static constexpr property_desc schema[] = {
      {"name", "Name", property_kind::text, "view"},
    };
    return schema;
  }

  auto create() const -> std::unique_ptr<node> override {
    auto n = std::make_unique<view_node>();
    for (auto const& d : property_schema()) {
      n->properties().set(d.key, d.default_value);
    }
    return n;
  }
};

}  // namespace

}  // namespace cc::basic

// ===========================================================================
// Plugin entry points
// ===========================================================================

extern "C" cc::plugin_info cc_plugin_load() {
  return {cc::plugin_api_version, "basic", "basic"};
}

extern "C" void cc_plugin_register(cc::host_registry& r) {
  r.types().register_value_type<std::string>("text");
  r.register_node_factory(std::make_unique<cc::basic::from_file_factory>());
  r.register_node_factory(std::make_unique<cc::basic::view_factory>());
}
