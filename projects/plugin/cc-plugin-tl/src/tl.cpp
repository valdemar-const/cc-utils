// cc-plugin-tl — frontend node for the "tl" language.
//
// Replaces the old v2 cc::frontend plugin. Single node:
//   - tl.frontend: input "src" (text) -> output "ast" (tl.ast)
//
// Type "Ast" is std::shared_ptr<cc::ast::tl_program>. The shared_ptr wrap
// is required because tl_program owns a unique_ptr<program> (non-copyable),
// but any_value is a value-semantic copyable carrier (aa::copy).
//
// Cross-DSO works because both the type (cc::ast::tl_program) and the smart
// pointer's typeinfo are anchored in libcc-astit (shared, default visibility).

#include "cc/any_value.hpp"
#include "cc/host.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_entry.hpp"

#include <cc/astit.hpp>   // cc::ast::tl_program, cc::ast::program
#include <cc/parseit.hpp> // cc::parseit::parse

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace cc::basic::tl
{

// Canonical value type carried on an "ast" wire. Aliased here so both this
// plugin (producer) and cc-plugin-tl-ir (consumer) spell the type identically
// and thus instantiate the same descriptor_of<...>.
using ast_value = std::shared_ptr<cc::ast::tl_program>;

namespace
{

    auto
    fresh_instance_id(std::string_view type_id) -> std::string
    {
        static std::atomic<unsigned> counter {0};
        unsigned                     n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        return std::string {type_id} + "#" + std::to_string(n);
    }

    class props final : public node_properties
    {
      public:

        auto
        get(std::string_view key) const -> std::string_view override
        {
            auto it = m_.find(std::string {key});
            return it == m_.end() ? std::string_view {} : it->second;
        }

        auto
        set(std::string_view key, std::string_view value) -> void override
        {
            m_[std::string {key}] = std::string {value};
        }

      private:

        std::unordered_map<std::string, std::string> m_;
    };

    // ---------------------------------------------------------------------------
    // Slots
    // ---------------------------------------------------------------------------
    class text_in_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "src";
        }

        auto
        type() const -> type_descriptor_t override
        {
            return descriptor_of<std::string>;
        }

        auto
        dir() const -> slot_dir override
        {
            return slot_dir::in;
        }

        auto
        card() const -> slot_card override
        {
            return slot_card::single;
        }
    };

    class ast_out_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "ast";
        }

        auto
        type() const -> type_descriptor_t override
        {
            return descriptor_of<ast_value>;
        }

        auto
        dir() const -> slot_dir override
        {
            return slot_dir::out;
        }

        auto
        card() const -> slot_card override
        {
            return slot_card::single;
        }
    };

    // ---------------------------------------------------------------------------
    // tl.frontend: text -> ast
    // ---------------------------------------------------------------------------
    class frontend_node final : public node
    {
      public:

        frontend_node()
            : id_(fresh_instance_id("tl.frontend"))
        {
        }

        explicit frontend_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "tl.frontend";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const text_in_slot    s_in {};
            static const ast_out_slot    s_out {};
            static constexpr const slot *arr[] = {&s_in, &s_out};
            return arr;
        }

        auto
        properties() -> node_properties & override
        {
            return props_;
        }

        auto
        slot_values() -> node_properties & override
        {
            return vals_;
        }

        auto
        activate(std::span<const input_pair> inputs, std::span<output_pair> outputs, const activate_context & /*ctx*/) -> activate_result override
        {
            // Pull the "src" input.
            const std::string *src_text = nullptr;
            for (auto [slot_id, value] : inputs)
            {
                if (slot_id == "src" && value != nullptr)
                {
                    src_text = aa::any_cast<std::string>(value);
                    break;
                }
            }
            if (src_text == nullptr)
            {
                return std::unexpected(failure {"'src' input not connected or wrong type"});
            }
            log("tl.frontend[" + id_ + "]: parsing " + std::to_string(src_text->size()) + " chars");

            auto parsed = cc::parseit::parse(*src_text);
            if (!parsed)
            {
                log("tl.frontend[" + id_ + "]: parse error: " + std::move(parsed).error());
                return std::unexpected(failure {"parse error: " + std::move(parsed).error()});
            }

            auto carrier  = std::make_shared<cc::ast::tl_program>();
            carrier->root = std::make_unique<cc::ast::program>(std::move(*parsed));
            log("tl.frontend[" + id_ + "]: AST ok");

            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "ast")
                {
                    *out = ast_value {std::move(carrier)};
                    return {};
                }
            }
            return std::unexpected(failure {"no 'ast' output slot on " + id_});
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class frontend_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "tl.frontend";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "AST Parser (TL)";
        }

        auto
        category() const -> std::string_view override
        {
            return "TL";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"compiler/lang/tl"};
            return ds;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return std::make_unique<frontend_node>();
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return std::make_unique<frontend_node>(std::string {instance_id});
        }
    };

} // namespace

} // namespace cc::basic::tl

// ===========================================================================
// Plugin entry points
// ===========================================================================

extern "C" cc::plugin_info
cc_plugin_load()
{
    return {cc::plugin_api_version, "tl", "frontend"};
}

extern "C" void
cc_plugin_register(cc::host_registry &r)
{
    r.register_domain({.id = "compiler/lang/tl", .display_name = "TL Language", .description = "tl frontend passes: parsing and AST", .depends_on = {"basic/types", "filesystem", "basic/view"}, .provided_types = {}});

    r.push_domain("compiler/lang/tl");
    r.types().register_value_type<cc::basic::tl::ast_value>({.name = "Ast", .short_name = "ast", .description = "tl syntax tree (shared_ptr<tl_program>)", .inline_control = {}, .parse = {}});
    r.pop_domain();
    r.register_node_factory(std::make_unique<cc::basic::tl::frontend_factory>());
}
