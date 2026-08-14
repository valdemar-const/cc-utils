#include "cc/host_registry.hpp" // make_host_registry
#include "cc/host.hpp"
#include "cc/node_factory.hpp"
#include "cc/registry.hpp"
#include "cc/view.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cc::runtime
{

// ===========================================================================
// type_registry_impl
// ===========================================================================
class type_registry_impl final : public type_registry
{
  public:

    // Called after each successful registration with the canonical type
    // name. Wired by host_registry_impl to attribute types to the domain
    // currently active in the push_domain()/pop_domain() scope.
    void
    set_registration_hook(std::function<void(std::string_view)> h)
    {
        hook_ = std::move(h);
    }

    auto
    name_of(type_descriptor_t d) const -> std::string_view override
    {
        auto it = desc_to_name_.find(d);
        return it == desc_to_name_.end() ? std::string_view {} : it->second;
    }

    auto
    short_name_of(type_descriptor_t d) const -> std::string_view override
    {
        auto it = desc_to_short_.find(d);
        return it == desc_to_short_.end() ? std::string_view {} : it->second;
    }

    auto
    descriptor_of_name(std::string_view name) const -> type_descriptor_t override
    {
        auto it = name_to_desc_.find(std::string {name});
        return it == name_to_desc_.end() ? type_descriptor_t {} : it->second;
    }

    auto
    is_connectable(type_descriptor_t out, type_descriptor_t in) const -> bool override
    {
        if (out == in)
        {
            return true;
        }
        if (any_descriptor_ && in == *any_descriptor_)
        {
            return true;
        }
        return false;
    }

    auto
    inline_editor_of(type_descriptor_t d) const -> std::optional<property_kind> override
    {
        auto name = name_of(d);
        if (name.empty())
        {
            return std::nullopt;
        }
        auto it = ext_by_name_.find(std::string {name});
        return it == ext_by_name_.end() ? std::nullopt : it->second.inline_control;
    }

    auto
    parse_value(type_descriptor_t d, std::string_view text) const
            -> std::expected<any_value, std::string> override
    {
        auto name = name_of(d);
        if (name.empty())
        {
            return std::unexpected("unknown value type");
        }
        auto it = ext_by_name_.find(std::string {name});
        if (it == ext_by_name_.end() || !it->second.parse)
        {
            return std::unexpected("type '" + std::string {name} + "' has no inline editor");
        }
        return it->second.parse(text);
    }

    auto
    register_value_type_impl(value_type_desc d, type_descriptor_t t) -> bool override
    {
        // The wildcard input type. Accepted under either spelling for
        // tolerance towards plugins; hosts register it as "Any".
        if (d.name == "any" || d.name == "Any")
        {
            if (any_descriptor_ && *any_descriptor_ != t)
            {
                return false;
            }
            any_descriptor_ = t;
        }
        auto [it_n, inserted_n] = name_to_desc_.try_emplace(std::string {d.name}, t);
        if (!inserted_n)
        {
            if (it_n->second != t)
            {
                return false; // name already bound to a different type
            }
        }
        // Extension fields (short name, inline editor, parser): first
        // registration wins; a conflicting re-registration is an error.
        auto [it_e, inserted_e] = ext_by_name_.try_emplace(std::string {d.name});
        if (inserted_e)
        {
            it_e->second.short_name     = std::string {d.short_name};
            it_e->second.inline_control = d.inline_control;
            it_e->second.parse          = std::move(d.parse);
        }
        else if (it_e->second.short_name != d.short_name || it_e->second.inline_control != d.inline_control)
        {
            return false; // same type, conflicting descriptor
        }
        desc_to_name_[t]  = std::string {d.name};
        desc_to_short_[t] = std::string {d.short_name};
        if (hook_)
        {
            hook_(d.name);
        }
        return true;
    }

  private:

    struct type_ext
    {
        std::string                  short_name;
        std::optional<property_kind> inline_control;
        value_parse_fn               parse;
    };

    std::unordered_map<std::string, type_descriptor_t> name_to_desc_;
    std::unordered_map<type_descriptor_t, std::string> desc_to_name_;
    std::unordered_map<type_descriptor_t, std::string> desc_to_short_;
    std::unordered_map<std::string, type_ext>          ext_by_name_;
    std::optional<type_descriptor_t>                   any_descriptor_;
    std::function<void(std::string_view)>              hook_;
};

// ===========================================================================
// view_renderer_provider_impl
//
// Indexes renderers by the type-name they declare. `get_for_type(descriptor)`
// asks the bound type_registry to translate descriptor→name, then looks up by
// name. This decouples provider from the registry but still keeps descriptor
// dispatch working for callers that only have a value's runtime descriptor.
// ===========================================================================
class view_renderer_provider_impl final : public view_renderer_provider
{
  public:

    explicit view_renderer_provider_impl(const type_registry &types)
        : types_ {types}
    {
    }

    auto
    get_for_type(type_descriptor_t type) const -> view_renderer * override
    {
        auto name = types_.name_of(type);
        if (name.empty())
        {
            return nullptr;
        }
        auto it = by_name_.find(std::string {name});
        return it == by_name_.end() ? nullptr : it->second.get();
    }

    auto
    all() const -> std::span<view_renderer * const> override
    {
        return order_;
    }

    auto
    register_renderer(std::unique_ptr<view_renderer> r) -> void override
    {
        if (!r)
        {
            return;
        }
        auto           name       = std::string {r->type_name()};
        view_renderer *raw        = r.get();
        by_name_[std::move(name)] = std::move(r);
        order_.push_back(raw);
    }

  private:

    const type_registry                                            &types_;
    std::unordered_map<std::string, std::unique_ptr<view_renderer>> by_name_;
    std::vector<view_renderer *>                                    order_;
};

// ===========================================================================
// host_registry_impl
// ===========================================================================
class host_registry_impl final : public host_registry
{
  public:

    host_registry_impl()
        : types_ {}
        , renderers_ {types_}
    {
        types_.set_registration_hook([this](std::string_view name)
                                     {
                                         attribute_type_to_domain(name);
                                     });
    }

    auto
    types() -> type_registry & override
    {
        return types_;
    }

    auto
    types() const -> const type_registry & override
    {
        return types_;
    }

    // ---- vocabulary domains ----
    auto
    register_domain(domain_desc d) -> void override
    {
        if (d.id.empty())
        {
            return;
        }
        auto [it, inserted] = domain_index_.try_emplace(d.id, domains_.size());
        if (inserted)
        {
            domains_.push_back(std::move(d));
            return;
        }
        // Merge into the existing descriptor: fill empty metadata (first
        // non-empty wins), union the dependencies.
        auto       &existing = domains_[it->second];
        domain_desc donor    = std::move(d);
        if (existing.display_name.empty())
        {
            existing.display_name = std::move(donor.display_name);
        }
        if (existing.description.empty())
        {
            existing.description = std::move(donor.description);
        }
        for (auto &dep : donor.depends_on)
        {
            if (std::find(existing.depends_on.begin(), existing.depends_on.end(), dep) == existing.depends_on.end())
            {
                existing.depends_on.push_back(std::move(dep));
            }
        }
    }

    auto
    find_domain(std::string_view id) const -> const domain_desc * override
    {
        auto it = domain_index_.find(std::string {id});
        return it == domain_index_.end() ? nullptr : &domains_[it->second];
    }

    auto
    domains() const -> std::span<const domain_desc> override
    {
        return domains_;
    }

    auto
    domain_closure(std::span<const std::string_view> roots) const -> std::vector<std::string> override
    {
        std::vector<std::string>        out;
        std::unordered_set<std::string> seen;
        std::deque<std::string_view>    queue {roots.begin(), roots.end()};
        while (!queue.empty())
        {
            std::string id {queue.front()};
            queue.pop_front();
            if (!seen.insert(id).second)
            {
                continue;
            }
            auto it = domain_index_.find(id);
            if (it == domain_index_.end())
            {
                continue; // unknown ids are surfaced by callers, not here
            }
            out.push_back(id);
            for (const auto &dep : domains_[it->second].depends_on)
            {
                queue.push_back(dep);
            }
        }
        return out;
    }

    auto
    push_domain(std::string_view id) -> void override
    {
        domain_stack_.emplace_back(id);
        register_domain(domain_desc {std::string {id}, {}, {}, {}, {}});
    }

    auto
    pop_domain() -> void override
    {
        if (!domain_stack_.empty())
        {
            domain_stack_.pop_back();
        }
    }

    // ---- node factories ----
    auto
    register_node_factory(std::unique_ptr<node_factory> factory) -> void override
    {
        if (!factory)
        {
            return;
        }
        std::string   id {factory->type_id()};
        node_factory *raw = factory.get();
        factories_storage_.push_back(std::move(factory));
        factories_by_id_[id] = raw;
        factories_order_.push_back(raw);
        // Attribute the new factory to the innermost active provider, if any.
        // Host-side factories registered outside any push/pop scope get an empty
        // provider string (= unknown), which is fine for our purposes.
        if (!provider_stack_.empty())
        {
            provider_by_type_[id] = provider_stack_.back();
        }
    }

    auto
    find_node_factory(std::string_view type_id) const -> node_factory * override
    {
        auto it = factories_by_id_.find(std::string {type_id});
        return it == factories_by_id_.end() ? nullptr : it->second;
    }

    auto
    node_factories() const -> std::span<node_factory * const> override
    {
        return factories_order_;
    }

    auto
    renderers() -> view_renderer_provider & override
    {
        return renderers_;
    }

    // ---- provider bookkeeping ----
    auto
    push_provider(std::string_view name) -> void override
    {
        provider_stack_.emplace_back(name);
        // Record on first sight so loaded_plugins() reports every plugin we ever
        // entered a scope for, even if it registered zero factories.
        if (std::find(loaded_plugins_.begin(), loaded_plugins_.end(), provider_stack_.back()) == loaded_plugins_.end())
        {
            loaded_plugins_.push_back(provider_stack_.back());
        }
    }

    auto
    pop_provider() -> void override
    {
        if (!provider_stack_.empty())
        {
            provider_stack_.pop_back();
        }
    }

    auto
    provider_of(std::string_view type_id) const -> std::string_view override
    {
        auto it = provider_by_type_.find(std::string {type_id});
        return it == provider_by_type_.end() ? std::string_view {} : it->second;
    }

    auto
    loaded_plugins() const -> std::span<const std::string> override
    {
        return loaded_plugins_;
    }

  private:

    void
    attribute_type_to_domain(std::string_view type_name)
    {
        if (domain_stack_.empty())
        {
            return;
        }
        auto it = domain_index_.find(domain_stack_.back());
        if (it == domain_index_.end())
        {
            return;
        }
        auto &provided = domains_[it->second].provided_types;
        if (std::find(provided.begin(), provided.end(), type_name) == provided.end())
        {
            provided.emplace_back(type_name);
        }
    }

    type_registry_impl                              types_;
    view_renderer_provider_impl                     renderers_;
    std::vector<domain_desc>                        domains_;
    std::unordered_map<std::string, std::size_t>    domain_index_;
    std::vector<std::string>                        domain_stack_;
    std::vector<std::unique_ptr<node_factory>>      factories_storage_;
    std::unordered_map<std::string, node_factory *> factories_by_id_;
    std::vector<node_factory *>                     factories_order_;
    std::vector<std::string>                        provider_stack_;
    std::vector<std::string>                        loaded_plugins_;
    std::unordered_map<std::string, std::string>    provider_by_type_;
};

// ===========================================================================
// Public factory
// ===========================================================================

auto
make_host_registry() -> std::unique_ptr<host_registry>
{
    return std::make_unique<host_registry_impl>();
}

} // namespace cc::runtime
