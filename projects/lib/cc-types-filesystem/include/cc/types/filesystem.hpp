#pragma once

// Type package for the `filesystem` vocabulary domain.
//
// Cross-DSO contract: every plugin that produces or consumes File / FileAttrs
// pins links libcc-types-filesystem, so the typeinfo is anchored in exactly
// one shared object and aa::any_cast succeeds across plugin boundaries. A
// plugin that merely wants to declare a pin compatible with these types does
// the same: include this header + link the package, then use
// cc::descriptor_of<cc::fs::file_handle>.

#include "cc-types-filesystem_export.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace cc::fs
{

// Snapshot of a file's metadata at materialisation time. A Path carries only
// naming invariants; the attributes below belong to an existing file, so they
// travel with a file_handle, never with a bare path.
struct file_attrs
{
    std::uintmax_t                  size {};
    std::filesystem::perms          permissions {};
    std::filesystem::file_time_type modified {};
};

// Materialised file: a canonical path plus a metadata snapshot taken when the
// file was opened/created. The "Optional<File>" of the DSL — the None case
// lives in activation failures (a node that cannot materialise the file
// fails), never in the value itself.
struct file_handle
{
    std::filesystem::path path;
    file_attrs            attrs;
};

// stat() a path into a file_handle snapshot. Returns nullopt if the path does
// not refer to an existing regular file (the caller decides whether that is a
// failure — get_file vs get_or_create_file).
CC_TYPES_FILESYSTEM_API auto stat_file(const std::filesystem::path &p) -> std::optional<file_handle>;

} // namespace cc::fs
