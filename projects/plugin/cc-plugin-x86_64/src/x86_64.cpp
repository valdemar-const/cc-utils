// cc-plugin-x86_64 — backend nodes for x86_64 ELF target.
//
// Replaces the old v2 cc::backend plugin. Two nodes split what used to be one
// opaque `emit()` call, so the NASM listing is observable on a wire (and can
// be viewed, edited, or piped to a different assembler):
//
//   - x86_64.nasm_gen:   input "ir" (ir.module)  -> output "asm" (text)
//   - x86_64.assemble:   input "asm" (text)        -> output "exe" (text path)
//                        + property "out_path" (filesystem path to write)
//
// The split makes it possible to debug the lowering step (View the listing)
// before paying for a fork+exec of nasm/ld.

#include "cc/any_value.hpp"
#include "cc/host.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_entry.hpp"

#include <cc/gen.hpp>  // cc::gen::lower / format
#include <cc/ir.hpp>   // cc::ir::module

#include <atomic>
#include <cstdio>       // std::remove
#include <cstdlib>      // std::system
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace cc::basic::x86_64 {

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
class ir_in_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "ir"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<cc::ir::module>; }
  auto dir()  const -> slot_dir          override { return slot_dir::in; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class text_in_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "asm"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::string>; }
  auto dir()  const -> slot_dir          override { return slot_dir::in; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class text_out_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "asm"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::string>; }
  auto dir()  const -> slot_dir          override { return slot_dir::out; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class exe_out_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "exe"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::filesystem::path>; }
  auto dir()  const -> slot_dir          override { return slot_dir::out; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

// ---------------------------------------------------------------------------
// x86_64.nasm_gen: ir.module -> NASM text listing
// ---------------------------------------------------------------------------
class nasm_gen_node final : public node {
 public:
  auto type_id()     const -> std::string_view override { return "x86_64.nasm_gen"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const ir_in_slot   s_in{};
    static const text_out_slot s_out{};
    static constexpr slot const* arr[] = {&s_in, &s_out};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  auto activate(std::span<const input_pair>  inputs,
                std::span<output_pair>       outputs) -> activate_result override {
    const cc::ir::module* mod = nullptr;
    for (auto [slot_id, value] : inputs) {
      if (slot_id == "ir" && value != nullptr) {
        mod = aa::any_cast<cc::ir::module>(value);
        break;
      }
    }
    if (mod == nullptr) {
      return std::unexpected(failure{"'ir' input not connected or wrong type"});
    }

    const std::vector<cc::nasm::instr> instrs = cc::gen::lower(*mod);
    const std::string listing = cc::gen::format(instrs);

    for (auto& [slot_id, out] : outputs) {
      if (slot_id == "asm") {
        *out = listing;
        return {};
      }
    }
    return std::unexpected(failure{"no 'asm' output slot on " + id_});
  }

 private:
  std::string id_{fresh_instance_id("x86_64.nasm_gen")};
  props       props_;
};

class nasm_gen_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "x86_64.nasm_gen"; }
  auto display_name() const -> std::string_view override { return "NASM Generator"; }
  auto category()     const -> std::string_view override { return "Backend"; }

  auto create() const -> std::unique_ptr<node> override {
    return std::make_unique<nasm_gen_node>();
  }
};

// ---------------------------------------------------------------------------
// x86_64.assemble: NASM text listing -> executable ELF (via nasm + ld)
// ---------------------------------------------------------------------------
class assemble_node final : public node {
 public:
  auto type_id()     const -> std::string_view override { return "x86_64.assemble"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const text_in_slot s_in{};
    static const exe_out_slot s_out{};
    static constexpr slot const* arr[] = {&s_in, &s_out};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  auto activate(std::span<const input_pair>  inputs,
                std::span<output_pair>       outputs) -> activate_result override {
    const std::string* asm_text = nullptr;
    for (auto [slot_id, value] : inputs) {
      if (slot_id == "asm" && value != nullptr) {
        asm_text = aa::any_cast<std::string>(value);
        break;
      }
    }
    if (asm_text == nullptr) {
      return std::unexpected(failure{"'asm' input not connected or wrong type"});
    }

    std::string raw_out{props_.get("out_path")};
    if (raw_out.empty()) {
      return std::unexpected(failure{"'out_path' property not set"});
    }
    std::filesystem::path exe(raw_out);
    std::filesystem::path asm_path = exe;
    asm_path += ".asm";
    std::filesystem::path obj_path = exe;
    obj_path += ".o";

    {
      std::ofstream os{asm_path};
      if (!os) {
        return std::unexpected(failure{"cannot write '" + asm_path.string() + "'"});
      }
      os << *asm_text;
    }
    if (std::system(("nasm -f elf64 " + asm_path.string() + " -o " + obj_path.string()).c_str()) != 0) {
      std::remove(asm_path.string().c_str());
      return std::unexpected(failure{"nasm failed (see console)"});
    }
    if (std::system(("ld " + obj_path.string() + " -o " + exe.string()).c_str()) != 0) {
      std::remove(asm_path.string().c_str());
      std::remove(obj_path.string().c_str());
      return std::unexpected(failure{"ld failed (see console)"});
    }
    std::remove(asm_path.string().c_str());
    std::remove(obj_path.string().c_str());

    for (auto& [slot_id, out] : outputs) {
      if (slot_id == "exe") {
        *out = exe;
        return {};
      }
    }
    return std::unexpected(failure{"no 'exe' output slot on " + id_});
  }

 private:
  std::string id_{fresh_instance_id("x86_64.assemble")};
  props       props_;
};

class assemble_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "x86_64.assemble"; }
  auto display_name() const -> std::string_view override { return "Assemble (NASM + ld)"; }
  auto category()     const -> std::string_view override { return "Backend"; }

  auto property_schema() const -> std::span<const property_desc> override {
    static constexpr property_desc schema[] = {
      {"out_path", "Output Path", property_kind::path, ""},
    };
    return schema;
  }

  auto create() const -> std::unique_ptr<node> override {
    auto n = std::make_unique<assemble_node>();
    for (auto const& d : property_schema()) {
      n->properties().set(d.key, d.default_value);
    }
    return n;
  }
};

}  // namespace

}  // namespace cc::basic::x86_64

// ===========================================================================
// Plugin entry points
// ===========================================================================

extern "C" cc::plugin_info cc_plugin_load() {
  return {cc::plugin_api_version, "x86_64", "backend"};
}

extern "C" void cc_plugin_register(cc::host_registry& r) {
  // ir.module is also registered by cc-plugin-tl-ir; idempotent re-register.
  r.types().register_value_type<cc::ir::module>("ir.module");
  r.register_node_factory(std::make_unique<cc::basic::x86_64::nasm_gen_factory>());
  r.register_node_factory(std::make_unique<cc::basic::x86_64::assemble_factory>());
}
