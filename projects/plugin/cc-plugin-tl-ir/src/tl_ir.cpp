// cc-plugin-tl-ir — IR generator node for the "tl" language.
//
// Replaces the old v2 cc::ir_generator plugin. Single node:
//   - tl.irgen: input "ast" (tl.ast) -> output "ir" (ir.module)
//
// Downcasts the wire value back to ast_value (shared_ptr<tl_program>), then
// calls cc::astq::lower on the contained program to emit a cc::ir::module.

#include "cc/any_value.hpp"
#include "cc/host.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_entry.hpp"

#include <cc/astit.hpp>  // cc::ast::tl_program
#include <cc/astq.hpp>   // cc::astq::lower
#include <cc/ir.hpp>     // cc::ir::module

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace cc::basic::tl {

// Must match cc-plugin-tl's spelling exactly so descriptor_of<ast_value>
// resolves to the same typeinfo on both sides of the DSO boundary.
using ast_value = std::shared_ptr<cc::ast::tl_program>;

namespace {

auto fresh_instance_id(std::string_view type_id) -> std::string {
  static std::atomic<unsigned> counter{0};
  unsigned n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return std::string{type_id} + "#" + std::to_string(n);
}

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
class ast_in_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "ast"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<ast_value>; }
  auto dir()  const -> slot_dir          override { return slot_dir::in; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class ir_out_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "ir"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<cc::ir::module>; }
  auto dir()  const -> slot_dir          override { return slot_dir::out; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

// ---------------------------------------------------------------------------
// tl.irgen: ast -> ir
// ---------------------------------------------------------------------------
class irgen_node final : public node {
 public:
  irgen_node() : id_(fresh_instance_id("tl.irgen")) {}
  explicit irgen_node(std::string id) : id_(std::move(id)) {}

  auto type_id()     const -> std::string_view override { return "tl.irgen"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const ast_in_slot s_in{};
    static const ir_out_slot s_out{};
    static constexpr slot const* arr[] = {&s_in, &s_out};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  auto activate(std::span<const input_pair>  inputs,
                std::span<output_pair>       outputs,
                const activate_context&      /*ctx*/) -> activate_result override {
    const ast_value* ast_ptr = nullptr;
    for (auto [slot_id, value] : inputs) {
      if (slot_id == "ast" && value != nullptr) {
        ast_ptr = aa::any_cast<ast_value>(value);
        break;
      }
    }
    if (ast_ptr == nullptr || *ast_ptr == nullptr || (*ast_ptr)->root == nullptr) {
      log("tl.irgen[" + id_ + "]: ast input missing or empty");
      return std::unexpected(failure{"'ast' input missing or invalid"});
    }
    log("tl.irgen[" + id_ + "]: lowering AST");

    cc::ir::module mod = cc::astq::lower(*(*ast_ptr)->root);
    log("tl.irgen[" + id_ + "]: emitted " + std::to_string(mod.code.size()) + " instrs");

    for (auto& [slot_id, out] : outputs) {
      if (slot_id == "ir") {
        *out = std::move(mod);
        return {};
      }
    }
    return std::unexpected(failure{"no 'ir' output slot on " + id_});
  }

 private:
  std::string id_;
  props       props_;
};

class irgen_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "tl.irgen"; }
  auto display_name() const -> std::string_view override { return "IR Generator (TL)"; }
  auto category()     const -> std::string_view override { return "TL"; }

  auto create() const -> std::unique_ptr<node> override {
    return std::make_unique<irgen_node>();
  }
  auto create_with_id(std::string_view instance_id) const
      -> std::unique_ptr<node> override {
    return std::make_unique<irgen_node>(std::string{instance_id});
  }
};

}  // namespace

}  // namespace cc::basic::tl

// ===========================================================================
// Plugin entry points
// ===========================================================================

extern "C" cc::plugin_info cc_plugin_load() {
  return {cc::plugin_api_version, "tl-ir", "irgen"};
}

extern "C" void cc_plugin_register(cc::host_registry& r) {
  // cc-plugin-tl also registers "tl.ast" — idempotent re-register is a no-op
  // (same name + same descriptor). This keeps the type available even if load
  // order ever changes.
  r.types().register_value_type<cc::basic::tl::ast_value>("tl.ast");
  r.types().register_value_type<cc::ir::module>("ir.module");
  r.register_node_factory(std::make_unique<cc::basic::tl::irgen_factory>());
}
