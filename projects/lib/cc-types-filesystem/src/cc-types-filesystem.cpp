#include "cc/types/filesystem.hpp"

namespace cc::fs
{

auto
stat_file(const std::filesystem::path &p) -> std::optional<file_handle>
{
    std::error_code ec;
    const auto      st = std::filesystem::status(p, ec);
    if (ec || !std::filesystem::exists(st) || !std::filesystem::is_regular_file(st))
    {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(p, ec);
    if (ec)
    {
        return std::nullopt;
    }
    const auto modified = std::filesystem::last_write_time(p, ec);
    if (ec)
    {
        return std::nullopt;
    }
    return file_handle {p.lexically_normal(), file_attrs {size, st.permissions(), modified}};
}

} // namespace cc::fs
