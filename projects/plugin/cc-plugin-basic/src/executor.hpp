#pragma once

// cc::basic::basic_executor<Tag> — platform-tagged subprocess runner.
//
// One common signature, two specializations:
//   - linux_tag : boost.process v2 (the original, verified-on-Linux path).
//   - win32_tag : raw CreateProcess + CreatePipe + STARTF_USESTDHANDLES.
//                 boost.process v2's default launcher takes the STARTUPINFOEX
//                 handle-list path which deadlocks under MinGW, and MinGW's
//                 _popen never sees pipe EOF — so win32 rolls its own minimal
//                 launcher (no STARTUPINFOEX, no handle list) which works.
//
// This header holds only the common declarations (tags, result/error types, the
// specialization method declarations and the native `executor` alias). Each
// platform's body lives in its own fragment — executor_win32.txx /
// executor_posix.txx — and is pulled in by executor.cpp under an #if switch, so
// the two implementations never share a file and a TU compiles only its native
// one.
//
// Usage:
//   auto r = cc::basic::executor::exec(exe, args, timeout_ms);
//   if (!r) { handle r.error(); }
//   auto [code, out, err] = *r;

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace cc::basic
{

struct win32_tag
{
};

struct linux_tag
{
};

#if defined(_WIN32)
using native_tag = win32_tag;
#else
using native_tag = linux_tag;
#endif

struct exec_result
{
    long        code;
    std::string out;
    std::string err;
};

struct exec_error
{
    std::string what;
    bool        timed_out = false;
};

template<typename Tag>
struct basic_executor;

template<>
struct basic_executor<linux_tag>
{
    static std::expected<exec_result, exec_error>
    exec(std::string_view        exe,
         std::string_view        args       = {},
         std::optional<unsigned> timeout_ms = std::nullopt);
};

template<>
struct basic_executor<win32_tag>
{
    static std::expected<exec_result, exec_error>
    exec(std::string_view        exe,
         std::string_view        args       = {},
         std::optional<unsigned> timeout_ms = std::nullopt);
};

// The native instantiation: cc::basic::executor::exec(...).
using executor = basic_executor<native_tag>;

} // namespace cc::basic
