// cc-plugin-basic — basic + filesystem + process node plugin (no UI).
//
// Seeds vocabulary domains:
//   basic/types      — String, Integer, Double, Boolean, Any (wildcard)
//   filesystem       — Path, File, FileAttrs + file/path nodes
//   basic/text       — text.constant
//   basic/view       — view (debug tap)
//   system/process   — exec
//
// Value types carry short pin annotations (name:short) and inline editors
// for primitives; the runner parses inline texts and injects them as regular
// input values. File is an opaque handle type anchored in
// libcc-types-filesystem (cross-DSO contract).

#include "cc/any_value.hpp"
#include "cc/host.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_entry.hpp"
#include "cc/types/filesystem.hpp"
#include "executor.hpp"

#include <atomic>
#include <cstdlib> // std::strtoll
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace cc::basic
{

namespace
{

    // Generate a fresh unique instance id for a node of the given type.
    auto
    fresh_instance_id(std::string_view type_id) -> std::string
    {
        static std::atomic<unsigned> counter {0};
        unsigned                     n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        return std::string {type_id} + "#" + std::to_string(n);
    }

    // Minimal property bag for plugin-local nodes (also backs slot_values()).
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
    // Slots — one parameterised class; static instances live inside each
    // node's slots() override so addresses stay stable.
    // ---------------------------------------------------------------------------
    class basic_slot final : public slot
    {
      public:

        basic_slot(std::string_view id, type_descriptor_t type, slot_dir dir, slot_card card, bool required = true)
            : id_ {id}
            , type_ {type}
            , dir_ {dir}
            , card_ {card}
            , required_ {required}
        {
        }

        auto
        id() const -> std::string_view override
        {
            return id_;
        }

        auto
        type() const -> type_descriptor_t override
        {
            return type_;
        }

        auto
        dir() const -> slot_dir override
        {
            return dir_;
        }

        auto
        card() const -> slot_card override
        {
            return card_;
        }

        auto
        is_required() const -> bool override
        {
            return required_;
        }

      private:

        std::string_view  id_;
        type_descriptor_t type_;
        slot_dir          dir_;
        slot_card         card_;
        bool              required_;
    };

    constexpr auto str_t   = descriptor_of<std::string>;
    constexpr auto path_t  = descriptor_of<std::filesystem::path>;
    constexpr auto file_t  = descriptor_of<cc::fs::file_handle>;
    constexpr auto attrs_t = descriptor_of<cc::fs::file_attrs>;
    constexpr auto int_t   = descriptor_of<long>;
    constexpr auto any_t   = descriptor_of<std::monostate>; // wildcard

    // Resolve a relative path against the pipeline's directory (relocatable
    // .pipeline files); absolute paths pass through verbatim.
    auto
    resolve_path(std::string_view raw, const activate_context &ctx) -> std::filesystem::path
    {
        std::filesystem::path p {raw};
        if (!p.is_absolute() && !ctx.pipeline_dir.empty())
        {
            p = std::filesystem::path {ctx.pipeline_dir} / p;
        }
        return p.lexically_normal();
    }

    auto
    find_input(std::span<const input_pair> inputs, std::string_view slot_id) -> const any_value *
    {
        for (auto [id, value] : inputs)
        {
            if (id == slot_id)
            {
                return value;
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // filesystem.path — typed let-node for Path values.
    //
    //   in (optional) ─┐
    //                  ├─▶ out:path   (the connected wire wins; the
    //   property "value" ─┘            property is the fallback)
    //
    // Fan-out point for reusing one path across several consumers, and the
    // canonical constant source for pipelines that prefer explicit nodes.
    // The input slot is named "in" (not "path") — slot ids share one
    // namespace per node, and a colliding in/out pair would make edge
    // resolution ambiguous.
    // ---------------------------------------------------------------------------
    class path_node final : public node
    {
      public:

        path_node()
            : id_(fresh_instance_id("filesystem.path"))
        {
        }

        explicit path_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.path";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const basic_slot      s_in {"in", path_t, slot_dir::in, slot_card::single, false};
            static const basic_slot      s_out {"path", path_t, slot_dir::out, slot_card::single};
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
            std::filesystem::path value;
            if (const auto *wired = find_input(inputs, "in"))
            {
                if (const auto *p = aa::any_cast<std::filesystem::path>(wired))
                {
                    value = *p;
                }
            }
            else
            {
                value = std::filesystem::path {std::string {props_.get("value")}};
            }
            if (value.empty())
            {
                return std::unexpected(failure {"path not set (connect 'in' or set the 'value' property)"});
            }
            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "path")
                {
                    *out = value;
                    return {};
                }
            }
            return std::unexpected(failure {"no 'path' slot on " + id_});
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class path_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.path";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "Path";
        }

        auto
        category() const -> std::string_view override
        {
            return "Filesystem";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"filesystem"};
            return ds;
        }

        auto
        property_schema() const -> std::span<const property_desc> override
        {
            static constexpr property_desc schema[] = {
                    {"value", "Value (when 'in' is not connected)", property_kind::path, ""},
            };
            return schema;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<path_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<path_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // filesystem.get_file — strict materialisation Path → File. Fails when the
    // file does not exist (the None case of the Optional<File> mental model).
    // ---------------------------------------------------------------------------
    class get_file_node final : public node
    {
      public:

        get_file_node()
            : id_(fresh_instance_id("filesystem.get_file"))
        {
        }

        explicit get_file_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.get_file";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const basic_slot      s_in {"path", path_t, slot_dir::in, slot_card::single};
            static const basic_slot      s_out {"file", file_t, slot_dir::out, slot_card::single};
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
        activate(std::span<const input_pair> inputs, std::span<output_pair> outputs, const activate_context &ctx) -> activate_result override
        {
            const auto *wired = find_input(inputs, "path");
            const auto *p     = wired ? aa::any_cast<std::filesystem::path>(wired) : nullptr;
            if (!p || p->empty())
            {
                return std::unexpected(failure {"'path' input not connected or empty"});
            }
            auto resolved = resolve_path(p->string(), ctx);
            auto handle   = cc::fs::stat_file(resolved);
            if (!handle)
            {
                log("get_file[" + id_ + "]: no such file '" + resolved.string() + "'");
                return std::unexpected(failure {"file does not exist: '" + resolved.string() + "'"});
            }
            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "file")
                {
                    *out = std::move(*handle);
                    return {};
                }
            }
            return std::unexpected(failure {"no 'file' slot on " + id_});
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class get_file_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.get_file";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "Get File";
        }

        auto
        category() const -> std::string_view override
        {
            return "Filesystem";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"filesystem"};
            return ds;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<get_file_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<get_file_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // filesystem.get_or_create_file — lenient materialisation Path → File for
    // write targets: creates an empty regular file when missing, then stats.
    // ---------------------------------------------------------------------------
    class get_or_create_node final : public node
    {
      public:

        get_or_create_node()
            : id_(fresh_instance_id("filesystem.get_or_create_file"))
        {
        }

        explicit get_or_create_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.get_or_create_file";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const basic_slot      s_in {"path", path_t, slot_dir::in, slot_card::single};
            static const basic_slot      s_out {"file", file_t, slot_dir::out, slot_card::single};
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
        activate(std::span<const input_pair> inputs, std::span<output_pair> outputs, const activate_context &ctx) -> activate_result override
        {
            const auto *wired = find_input(inputs, "path");
            const auto *p     = wired ? aa::any_cast<std::filesystem::path>(wired) : nullptr;
            if (!p || p->empty())
            {
                return std::unexpected(failure {"'path' input not connected or empty"});
            }
            auto            resolved = resolve_path(p->string(), ctx);
            std::error_code ec;
            if (!std::filesystem::exists(resolved, ec))
            {
                std::ofstream os {resolved, std::ios::app};
                if (!os)
                {
                    return std::unexpected(failure {"cannot create '" + resolved.string() + "'"});
                }
                log("get_or_create_file[" + id_ + "]: created " + resolved.string());
            }
            auto handle = cc::fs::stat_file(resolved);
            if (!handle)
            {
                return std::unexpected(failure {"not a regular file: '" + resolved.string() + "'"});
            }
            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "file")
                {
                    *out = std::move(*handle);
                    return {};
                }
            }
            return std::unexpected(failure {"no 'file' slot on " + id_});
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class get_or_create_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.get_or_create_file";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "Get or Create File";
        }

        auto
        category() const -> std::string_view override
        {
            return "Filesystem";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"filesystem"};
            return ds;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<get_or_create_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<get_or_create_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // filesystem.file — inspector ("тройник"): passes the handle through and
    // exposes a projection of its metadata snapshot. No I/O at activation.
    // ---------------------------------------------------------------------------
    class file_node final : public node
    {
      public:

        file_node()
            : id_(fresh_instance_id("filesystem.file"))
        {
        }

        explicit file_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.file";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const basic_slot      s_in {"file", file_t, slot_dir::in, slot_card::single};
            static const basic_slot      s_out {"file", file_t, slot_dir::out, slot_card::single};
            static const basic_slot      s_attrs {"attrs", attrs_t, slot_dir::out, slot_card::single};
            static constexpr const slot *arr[] = {&s_in, &s_out, &s_attrs};
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
            const auto *wired = find_input(inputs, "file");
            const auto *h     = wired ? aa::any_cast<cc::fs::file_handle>(wired) : nullptr;
            if (!h)
            {
                return std::unexpected(failure {"'file' input not connected or wrong type"});
            }
            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "file")
                {
                    *out = *h;
                }
                else if (slot_id == "attrs")
                {
                    *out = h->attrs;
                }
            }
            return {};
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class file_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.file";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "File";
        }

        auto
        category() const -> std::string_view override
        {
            return "Filesystem";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"filesystem"};
            return ds;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<file_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<file_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // filesystem.read_text — File handle → String content.
    // ---------------------------------------------------------------------------
    class read_text_node final : public node
    {
      public:

        read_text_node()
            : id_(fresh_instance_id("filesystem.read_text"))
        {
        }

        explicit read_text_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.read_text";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const basic_slot      s_in {"file", file_t, slot_dir::in, slot_card::single};
            static const basic_slot      s_out {"text", str_t, slot_dir::out, slot_card::single};
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
            const auto *wired = find_input(inputs, "file");
            const auto *h     = wired ? aa::any_cast<cc::fs::file_handle>(wired) : nullptr;
            if (!h)
            {
                return std::unexpected(failure {"'file' input not connected or wrong type"});
            }
            std::ifstream in {h->path};
            if (!in)
            {
                return std::unexpected(failure {"cannot open '" + h->path.string() + "'"});
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            auto content = ss.str();
            log("read_text[" + id_ + "]: read " + std::to_string(content.size()) + " bytes from " + h->path.string());
            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "text")
                {
                    *out = std::move(content);
                    return {};
                }
            }
            return std::unexpected(failure {"no 'text' slot on " + id_});
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class read_text_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "filesystem.read_text";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "Read Text";
        }

        auto
        category() const -> std::string_view override
        {
            return "Filesystem";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"filesystem"};
            return ds;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<read_text_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<read_text_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // basic.text.constant — emits a literal string entered in the property
    // editor (multiline).
    // ---------------------------------------------------------------------------
    class constant_node final : public node
    {
      public:

        constant_node()
            : id_(fresh_instance_id("basic.text.constant"))
        {
        }

        explicit constant_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "basic.text.constant";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const basic_slot      s_out {"text", str_t, slot_dir::out, slot_card::single};
            static constexpr const slot *arr[] = {&s_out};
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
        activate(std::span<const input_pair>, std::span<output_pair> outputs, const activate_context & /*ctx*/) -> activate_result override
        {
            std::string value {props_.get("value")};
            log("text.constant[" + id_ + "]: emitting " + std::to_string(value.size()) + " bytes");
            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "text")
                {
                    *out = std::move(value);
                    return {};
                }
            }
            return std::unexpected(failure {"no 'text' slot on " + id_});
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class constant_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "basic.text.constant";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "Text Constant";
        }

        auto
        category() const -> std::string_view override
        {
            return "Text";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"basic/text"};
            return ds;
        }

        auto
        property_schema() const -> std::span<const property_desc> override
        {
            static constexpr property_desc schema[] = {
                    {"value", "Value", property_kind::multiline, ""},
            };
            return schema;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<constant_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<constant_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // basic.view — debug tap, accepts any type. The View panel pulls the
    // upstream value directly via the runner.
    // ---------------------------------------------------------------------------
    class view_node final : public node
    {
      public:

        view_node()
            : id_(fresh_instance_id("basic.view"))
        {
        }

        explicit view_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "basic.view";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const basic_slot      s_in {"in", any_t, slot_dir::in, slot_card::multi, false};
            static constexpr const slot *arr[] = {&s_in};
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
        activate(std::span<const input_pair>, std::span<output_pair>, const activate_context & /*ctx*/) -> activate_result override
        {
            return {};
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class view_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "basic.view";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "View";
        }

        auto
        category() const -> std::string_view override
        {
            return "View";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"basic/view"};
            return ds;
        }

        auto
        property_schema() const -> std::span<const property_desc> override
        {
            static constexpr property_desc schema[] = {
                    {"name", "Name", property_kind::text, "view"},
            };
            return schema;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<view_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<view_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // basic.exec — runs an executable File, exposes ret_code / cout / cerr.
    //
    //   inputs:  file (File — the executable handle), args (String, optional)
    //   outputs: ret_code (Integer), cout (String), cerr (String)
    //   properties:
    //     merge_stderr (bool, default false) — fold stderr into stdout (2>&1)
    //     timeout_ms   (int,  default 0)      — kill the child after N ms (0=none)
    // ---------------------------------------------------------------------------
    class exec_node final : public node
    {
      public:

        exec_node()
            : id_(fresh_instance_id("basic.exec"))
        {
        }

        explicit exec_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "basic.exec";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const basic_slot      s_file {"file", file_t, slot_dir::in, slot_card::single};
            static const basic_slot      s_args {"args", str_t, slot_dir::in, slot_card::single, false};
            static const basic_slot      s_ret {"ret_code", int_t, slot_dir::out, slot_card::single};
            static const basic_slot      s_cout {"cout", str_t, slot_dir::out, slot_card::single};
            static const basic_slot      s_cerr {"cerr", str_t, slot_dir::out, slot_card::single};
            static constexpr const slot *arr[] = {&s_file, &s_args, &s_ret, &s_cout, &s_cerr};
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
            const auto *wired_file = find_input(inputs, "file");
            const auto *exe_f      = wired_file ? aa::any_cast<cc::fs::file_handle>(wired_file) : nullptr;
            const auto *wired_args = find_input(inputs, "args");
            const auto *args_s     = wired_args ? aa::any_cast<std::string>(wired_args) : nullptr;
            if (!exe_f || exe_f->path.empty())
            {
                return std::unexpected(failure {"'file' input not connected or empty"});
            }

            const bool merge = props_.get("merge_stderr") == "true"
                            || props_.get("merge_stderr") == "1";

            std::optional<unsigned> timeout;
            if (auto sv = props_.get("timeout_ms"); !sv.empty())
            {
                char         *end = nullptr;
                unsigned long ms  = std::strtoul(sv.data(), &end, 10);
                if (end != sv.data() && ms != 0)
                {
                    timeout = static_cast<unsigned>(ms);
                }
            }

            std::string exe  = exe_f->path.string();
            std::string args = args_s ? *args_s : std::string {};

            log("exec[" + id_ + "]: spawn " + exe + (args.empty() ? "" : " " + args));

            auto r = executor::exec(exe, args, timeout);
            if (!r)
            {
                return std::unexpected(failure {r.error().what});
            }

            std::string out = std::move(r->out);
            std::string err = std::move(r->err);
            if (merge)
            {
                out += err;
                err.clear();
            }

            log("exec[" + id_ + "]: exit_code = " + std::to_string(r->code)
                + ", cout=" + std::to_string(out.size()) + "B"
                + ", cerr=" + std::to_string(err.size()) + "B");

            for (auto &[slot_id, out_v] : outputs)
            {
                if (slot_id == "ret_code")
                {
                    *out_v = r->code;
                }
                else if (slot_id == "cout")
                {
                    *out_v = out;
                }
                else if (slot_id == "cerr")
                {
                    *out_v = err;
                }
            }
            return {};
        }

      private:

        std::string id_;
        props       props_;
        props       vals_;
    };

    class exec_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "basic.exec";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "Exec";
        }

        auto
        category() const -> std::string_view override
        {
            return "Process";
        }

        auto
        domains() const -> std::span<const std::string_view> override
        {
            static constexpr std::string_view ds[] = {"system/process"};
            return ds;
        }

        auto
        property_schema() const -> std::span<const property_desc> override
        {
            static constexpr property_desc schema[] = {
                    {"merge_stderr", "Merge stderr into stdout (2>&1)", property_kind::boolean, "false"},
                    {"timeout_ms", "Kill the child after N ms (0 = none)", property_kind::text, "0"},
            };
            return schema;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<exec_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<exec_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // Inline editors + validators for primitive types.
    // ---------------------------------------------------------------------------
    auto
    parse_string(std::string_view text) -> std::expected<any_value, std::string>
    {
        any_value v {};
        v = std::string {text};
        return v;
    }

    auto
    parse_integer(std::string_view text) -> std::expected<any_value, std::string>
    {
        std::string s {text};
        char       *end = nullptr;
        const long  n   = std::strtoll(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0')
        {
            return std::unexpected("'" + s + "' is not an integer");
        }
        any_value v {};
        v = n;
        return v;
    }

    auto
    parse_double(std::string_view text) -> std::expected<any_value, std::string>
    {
        std::string  s {text};
        char        *end = nullptr;
        const double d   = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0')
        {
            return std::unexpected("'" + s + "' is not a number");
        }
        any_value v {};
        v = d;
        return v;
    }

    auto
    parse_boolean(std::string_view text) -> std::expected<any_value, std::string>
    {
        bool b = false;
        if (text == "true" || text == "1")
        {
            b = true;
        }
        else if (text == "false" || text == "0")
        {
            b = false;
        }
        else
        {
            return std::unexpected("'" + std::string {text} + "' is not a boolean (true/false)");
        }
        any_value v {};
        v = b;
        return v;
    }

    auto
    parse_path(std::string_view text) -> std::expected<any_value, std::string>
    {
        if (text.empty())
        {
            return std::unexpected("path must not be empty");
        }
        any_value v {};
        v = std::filesystem::path {text};
        return v;
    }

    // RAII domain attribution scope (mirrors the loader's provider guard).
    struct domain_scope
    {
        host_registry &h;

        ~domain_scope()
        {
            h.pop_domain();
        }
    };

} // namespace

} // namespace cc::basic

// ===========================================================================
// Plugin entry points
// ===========================================================================

extern "C" cc::plugin_info
cc_plugin_load()
{
    return {cc::plugin_api_version, "basic", "basic"};
}

extern "C" void
cc_plugin_register(cc::host_registry &r)
{
    using namespace cc::basic;

    // ---- vocabulary domains (this plugin is the seeder, not the owner) ----
    r.register_domain({.id = "basic/types", .display_name = "Basic Types", .description = "String, Integer, Double, Boolean and the Any wildcard", .depends_on = {}, .provided_types = {}});
    r.register_domain({.id = "filesystem", .display_name = "Filesystem", .description = "Path/File vocabulary: materialisation, inspection, text reading", .depends_on = {"basic/types"}, .provided_types = {}});
    r.register_domain({.id = "basic/text", .display_name = "Text", .description = "String sources", .depends_on = {"basic/types"}, .provided_types = {}});
    r.register_domain({.id = "basic/view", .display_name = "View", .description = "Debug taps for any value", .depends_on = {"basic/types"}, .provided_types = {}});
    r.register_domain({.id = "system/process", .display_name = "Process", .description = "Subprocess execution", .depends_on = {"filesystem", "basic/types"}, .provided_types = {}});

    // ---- value types, attributed to their domains ----
    {
        domain_scope ds {r};
        r.push_domain("basic/types");
        r.types().register_value_type<std::string>({.name = "String", .short_name = "str", .description = "UTF-8 text", .inline_control = cc::property_kind::text, .parse = parse_string});
        r.types().register_value_type<long>({.name = "Integer", .short_name = "int", .description = "64-bit signed integer", .inline_control = cc::property_kind::integer, .parse = parse_integer});
        r.types().register_value_type<double>({.name = "Double", .short_name = "double", .description = "64-bit floating point", .inline_control = cc::property_kind::text, .parse = parse_double});
        r.types().register_value_type<bool>({.name = "Boolean", .short_name = "bool", .description = "true / false", .inline_control = cc::property_kind::boolean, .parse = parse_boolean});
        r.types().register_value_type<std::monostate>({.name = "Any", .short_name = "any", .description = "Wildcard — accepts any value", .inline_control = {}, .parse = {}});
    }
    {
        domain_scope ds {r};
        r.push_domain("filesystem");
        r.types().register_value_type<std::filesystem::path>({.name = "Path", .short_name = "path", .description = "Filesystem path (naming invariants only)", .inline_control = cc::property_kind::path, .parse = parse_path});
        r.types().register_value_type<cc::fs::file_handle>({.name = "File", .short_name = "file", .description = "Materialised file handle with a metadata snapshot", .inline_control = {}, .parse = {}});
        r.types().register_value_type<cc::fs::file_attrs>({.name = "FileAttrs", .short_name = "attrs", .description = "File metadata snapshot: size, permissions, modified", .inline_control = {}, .parse = {}});
    }

    // ---- node factories ----
    r.register_node_factory(std::make_unique<path_factory>());
    r.register_node_factory(std::make_unique<get_file_factory>());
    r.register_node_factory(std::make_unique<get_or_create_factory>());
    r.register_node_factory(std::make_unique<file_factory>());
    r.register_node_factory(std::make_unique<read_text_factory>());
    r.register_node_factory(std::make_unique<constant_factory>());
    r.register_node_factory(std::make_unique<view_factory>());
    r.register_node_factory(std::make_unique<exec_factory>());
}
