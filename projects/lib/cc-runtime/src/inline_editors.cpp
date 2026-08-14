#include "cc/inline_editors.hpp"

#include "cc/property_kind.hpp"
#include "cc/types/filesystem.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace cc::runtime
{

namespace
{

    auto
    parse_string(std::string_view text, std::string_view /*pipeline_dir*/) -> std::expected<any_value, std::string>
    {
        any_value v {};
        v.emplace<std::string>(text);
        return v;
    }

    auto
    parse_integer(std::string_view text, std::string_view /*pipeline_dir*/) -> std::expected<any_value, std::string>
    {
        std::string s {text};
        char       *end = nullptr;
        const long  n   = std::strtoll(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0')
        {
            return std::unexpected("'" + s + "' is not an integer");
        }
        any_value v {};
        v.emplace<long>(n);
        return v;
    }

    auto
    parse_double(std::string_view text, std::string_view /*pipeline_dir*/) -> std::expected<any_value, std::string>
    {
        std::string  s {text};
        char        *end = nullptr;
        const double d   = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0')
        {
            return std::unexpected("'" + s + "' is not a number");
        }
        any_value v {};
        v.emplace<double>(d);
        return v;
    }

    auto
    parse_boolean(std::string_view text, std::string_view /*pipeline_dir*/) -> std::expected<any_value, std::string>
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
        v.emplace<bool>(b);
        return v;
    }

    // Path stays verbatim: nodes with path inputs resolve relative entries
    // against the pipeline's directory at activation time (see
    // activate_context::pipeline_dir), so the stored text remains what the
    // user typed.
    auto
    parse_path(std::string_view text, std::string_view /*pipeline_dir*/) -> std::expected<any_value, std::string>
    {
        if (text.empty())
        {
            return std::unexpected("path must not be empty");
        }
        any_value v {};
        v.emplace<std::filesystem::path>(text);
        return v;
    }

    // File materialises immediately: resolve the text against the
    // pipeline's directory and stat it into a handle. A missing file is a
    // validator error (the Optional<File> None case surfaces next to the
    // control, before the graph even runs).
    auto
    parse_file(std::string_view text, std::string_view pipeline_dir) -> std::expected<any_value, std::string>
    {
        if (text.empty())
        {
            return std::unexpected("file path must not be empty");
        }
        std::filesystem::path p {text};
        if (!p.is_absolute() && !pipeline_dir.empty())
        {
            p = std::filesystem::path {pipeline_dir} / p;
        }
        p           = p.lexically_normal();
        auto handle = cc::fs::stat_file(p);
        if (!handle)
        {
            return std::unexpected("file does not exist: '" + p.string() + "'");
        }
        any_value v {};
        v.emplace<cc::fs::file_handle>(std::move(*handle));
        return v;
    }

} // namespace

auto
register_inline_editors(host_registry &host) -> void
{
    auto &t = host.types();
    t.register_inline_editor("String", property_kind::text, parse_string);
    t.register_inline_editor("Integer", property_kind::integer, parse_integer);
    t.register_inline_editor("Double", property_kind::text, parse_double);
    t.register_inline_editor("Boolean", property_kind::boolean, parse_boolean);
    t.register_inline_editor("Path", property_kind::path, parse_path);
    t.register_inline_editor("File", property_kind::path, parse_file);
}

} // namespace cc::runtime
