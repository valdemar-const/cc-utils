// cc-plugin-basic — basic node plugin (no UI).
//
// Registers:
//   wire types: text (std::string), path (std::filesystem::path), int (long)
//   nodes:
//     - text.from_file: reads file content from a "path" property → outputs text
//     - view:           debug tap, accepts any value
//     - exec:           runs an executable, returns ret_code/cout/cerr
//
// exec delegates to cc::basic::executor (see executor.hpp), a platform-tagged
// runner: raw Win32 CreateProcess on Windows, boost.process v2 on POSIX. The
// node itself is fully platform-agnostic — it only wires the executor's result
// into its ret_code/cout/cerr slots. std::filesystem::path is used for every
// filesystem location — no platform-specific path encoding.

#include "cc/any_value.hpp"
#include "cc/host.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_entry.hpp"
#include "executor.hpp"

#include <atomic>
#include <cstdlib> // std::strtoul
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

    // Minimal property bag for plugin-local nodes.
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
    // Slots — reused across nodes where the wire type matches.
    // ---------------------------------------------------------------------------
    class text_out_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "out";
        }

        auto
        type() const -> type_descriptor_t override
        {
            return descriptor_of<std::string>;
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

    class any_in_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "in";
        }

        auto
        type() const -> type_descriptor_t override
        {
            return type_descriptor_t {};
        } // wildcard

        auto
        dir() const -> slot_dir override
        {
            return slot_dir::in;
        }

        auto
        card() const -> slot_card override
        {
            return slot_card::multi;
        }
    };

    // ---------------------------------------------------------------------------
    // text.from_file node — reads a file from the "path" property.
    // ---------------------------------------------------------------------------
    class from_file_node final : public node
    {
      public:

        from_file_node()
            : id_(fresh_instance_id("basic.text.from_file"))
        {
        }

        explicit from_file_node(std::string id)
            : id_(std::move(id))
        {
        }

        auto
        type_id() const -> std::string_view override
        {
            return "basic.text.from_file";
        }

        auto
        instance_id() const -> std::string_view override
        {
            return id_;
        }

        auto
        slots() const -> std::span<const slot * const> override
        {
            static const text_out_slot   s_out {};
            static constexpr const slot *arr[] = {&s_out};
            return arr;
        }

        auto
        properties() -> node_properties & override
        {
            return props_;
        }

        auto
        activate(std::span<const input_pair>, std::span<output_pair> outputs, const activate_context &ctx) -> activate_result override
        {
            std::string raw {props_.get("path")};
            if (raw.empty())
            {
                return std::unexpected(failure {"'path' not set"});
            }
            // Resolve relative paths against the pipeline's parent directory so the
            // .pipeline file is relocatable: the user types `./test.tl`, save/load
            // preserve the text verbatim, and the file resolves to a different
            // absolute path depending on where the .pipeline file was opened from.
            // Absolute paths are taken verbatim. Empty pipeline_dir (in-memory graph,
            // unit tests) → leave the string as-is, std::ifstream resolves against
            // the process working directory.
            std::filesystem::path path {raw};
            if (!path.is_absolute() && !ctx.pipeline_dir.empty())
            {
                path = std::filesystem::path {ctx.pipeline_dir} / path;
            }
            // Collapse redundant "."/".." left by the lexical join above (e.g.
            // a property of "./source.txt" would otherwise read as …\dir\.\source.txt).
            path = path.lexically_normal();
            log("from_file[" + id_ + "]: opening " + path.string());
            std::ifstream in(path);
            if (!in)
            {
                log("from_file[" + id_ + "]: cannot open");
                return std::unexpected(failure {"cannot open '" + path.string() + "'"});
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            auto content = ss.str();
            log("from_file[" + id_ + "]: read " + std::to_string(content.size()) + " bytes");

            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "out")
                {
                    *out = std::move(content);
                    return {};
                }
            }
            return std::unexpected(failure {"no 'out' slot on " + id_});
        }

      private:

        std::string id_;
        props       props_;
    };

    class from_file_factory final : public node_factory
    {
      public:

        auto
        type_id() const -> std::string_view override
        {
            return "basic.text.from_file";
        }

        auto
        display_name() const -> std::string_view override
        {
            return "Source File";
        }

        auto
        category() const -> std::string_view override
        {
            return "Basic";
        }

        auto
        property_schema() const -> std::span<const property_desc> override
        {
            static constexpr property_desc schema[] = {
                    {"path", "File Path", property_kind::path, ""},
            };
            return schema;
        }

        auto
        create() const -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<from_file_node>());
        }

        auto
        create_with_id(std::string_view instance_id) const
                -> std::unique_ptr<node> override
        {
            return apply_defaults(std::make_unique<from_file_node>(std::string {instance_id}));
        }
    };

    // ---------------------------------------------------------------------------
    // text.constant node — emits a literal string entered in the property editor
    // (multiline). Useful for args / inline source / hardcoded values without a
    // round-trip through a file on disk.
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
            static const text_out_slot   s_out {};
            static constexpr const slot *arr[] = {&s_out};
            return arr;
        }

        auto
        properties() -> node_properties & override
        {
            return props_;
        }

        auto
        activate(std::span<const input_pair>, std::span<output_pair> outputs, const activate_context & /*ctx*/) -> activate_result override
        {
            std::string value {props_.get("value")};
            log("text.constant[" + id_ + "]: emitting " + std::to_string(value.size()) + " bytes");
            for (auto &[slot_id, out] : outputs)
            {
                if (slot_id == "out")
                {
                    *out = std::move(value);
                    return {};
                }
            }
            return std::unexpected(failure {"no 'out' slot on " + id_});
        }

      private:

        std::string id_;
        props       props_;
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
            return "Basic";
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
    // view node — debug tap, accepts any type.
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
            static const any_in_slot     s_in {};
            static constexpr const slot *arr[] = {&s_in};
            return arr;
        }

        auto
        properties() -> node_properties & override
        {
            return props_;
        }

        // Sinks; the View panel pulls upstream value directly via the runner.
        auto
        activate(std::span<const input_pair>, std::span<output_pair>, const activate_context & /*ctx*/) -> activate_result override
        {
            return {};
        }

      private:

        std::string id_;
        props       props_;
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
            return "Basic";
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
    // exec node — runs an executable, exposes ret_code / cout / cerr as wires.
    //
    //   inputs:  exe (path), args (text, optional)
    //   outputs: ret_code (long), cout (text), cerr (text)
    //   properties:
    //     merge_stderr (bool, default false) — fold stderr into stdout (2>&1)
    //     timeout_ms   (int,  default 0)      — kill the child after N ms (0=none)
    //
    // Subprocess launch + capture is delegated to cc::basic::executor (see
    // executor.hpp), which picks a working launcher per platform (raw Win32 on
    // Windows, boost.process v2 on POSIX). merge_stderr is applied here in
    // software: the executor always returns separate stdout/stderr.
    // ---------------------------------------------------------------------------
    class path_in_exe_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "exe";
        }

        auto
        type() const -> type_descriptor_t override
        {
            return descriptor_of<std::filesystem::path>;
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

    class text_in_args_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "args";
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

        // Optional — exec without args is legitimate (the process just runs plainly).
        auto
        is_required() const -> bool override
        {
            return false;
        }
    };

    class long_out_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "ret_code";
        }

        auto
        type() const -> type_descriptor_t override
        {
            return descriptor_of<long>;
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

    class text_out_cout_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "cout";
        }

        auto
        type() const -> type_descriptor_t override
        {
            return descriptor_of<std::string>;
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

    class text_out_cerr_slot final : public slot
    {
      public:

        auto
        id() const -> std::string_view override
        {
            return "cerr";
        }

        auto
        type() const -> type_descriptor_t override
        {
            return descriptor_of<std::string>;
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
            static const path_in_exe_slot   s_exe {};
            static const text_in_args_slot  s_args {};
            static const long_out_slot      s_ret {};
            static const text_out_cout_slot s_cout {};
            static const text_out_cerr_slot s_cerr {};
            static constexpr const slot    *arr[] = {&s_exe, &s_args, &s_ret, &s_cout, &s_cerr};
            return arr;
        }

        auto
        properties() -> node_properties & override
        {
            return props_;
        }

        auto
        activate(std::span<const input_pair> inputs, std::span<output_pair> outputs, const activate_context & /*ctx*/) -> activate_result override
        {
            const std::filesystem::path *exe_p  = nullptr;
            const std::string           *args_s = nullptr;
            for (auto [slot_id, value] : inputs)
            {
                if (slot_id == "exe" && value)
                {
                    exe_p = aa::any_cast<std::filesystem::path>(value);
                }
                if (slot_id == "args" && value)
                {
                    args_s = aa::any_cast<std::string>(value);
                }
            }
            if (!exe_p || exe_p->empty())
            {
                return std::unexpected(failure {"'exe' input not connected or empty"});
            }

            const bool merge = props_.get("merge_stderr") == "true"
                            || props_.get("merge_stderr") == "1";

            // Optional timeout, in milliseconds. Malformed / zero values → none.
            std::optional<unsigned> timeout;
            if (auto sv = props_.get("timeout_ms"); !sv.empty())
            {
                char           *end = nullptr;
                unsigned long   ms  = std::strtoul(sv.data(), &end, 10);
                if (end != sv.data() && ms != 0)
                    timeout = static_cast<unsigned>(ms);
            }

            std::string exe  = exe_p->string();
            std::string args = args_s ? *args_s : std::string {};

            log("exec[" + id_ + "]: spawn " + exe + (args.empty() ? "" : " " + args));

            // Hand off to the platform executor: raw Win32 on Windows, boost.
            // process v2 on POSIX. Both return {code, out, err} or an error.
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
            return "Basic";
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
    r.types().register_value_type<std::string>("text");
    r.types().register_value_type<std::filesystem::path>("path");
    r.types().register_value_type<long>("int");
    r.register_node_factory(std::make_unique<cc::basic::from_file_factory>());
    r.register_node_factory(std::make_unique<cc::basic::constant_factory>());
    r.register_node_factory(std::make_unique<cc::basic::view_factory>());
    r.register_node_factory(std::make_unique<cc::basic::exec_factory>());
}
