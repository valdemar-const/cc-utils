#include "cc/host_registry.hpp" // make_host_registry
#include "cc/host.hpp"
#include "cc/node_factory.hpp"
#include "cc/registry.hpp"
#include "cc/view.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace cc::runtime
{

// ===========================================================================
// type_registry_impl
// ===========================================================================
class type_registry_impl final : public type_registry
{
  public:

    auto
    name_of(type_descriptor_t d) const -> std::string_view override
    {
        auto it = desc_to_name_.find(d);
        return it == desc_to_name_.end() ? std::string_view {} : it->second;
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
    register_value_type_impl(std::string_view name, type_descriptor_t d) -> bool override
    {
        if (name == "any")
        {
            if (any_descriptor_ && *any_descriptor_ != d)
            {
                return false;
            }
            any_descriptor_ = d;
        }
        auto [it_n, inserted_n] = name_to_desc_.try_emplace(std::string {name}, d);
        if (!inserted_n)
        {
            return it_n->second == d; // idempotent ok, mismatch fails
        }
        desc_to_name_[d] = std::string {name};
        return true;
    }

  private:

    std::unordered_map<std::string, type_descriptor_t> name_to_desc_;
    std::unordered_map<type_descriptor_t, std::string> desc_to_name_;
    std::optional<type_descriptor_t>                   any_descriptor_;
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

    type_registry_impl                              types_;
    view_renderer_provider_impl                     renderers_;
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
