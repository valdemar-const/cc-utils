// executor_win32.txx — win32_tag specialization (raw Win32 API).
//
// This file is NOT a standalone translation unit. It is included textually by
// executor.cpp under `#if defined(_WIN32)` and expects executor.hpp to have
// been included immediately above it. Keeping the body here (rather than inline
// in executor.cpp) lets each platform live in its own file while the .cpp stays
// a one-screen dispatch stub.
//
// Why raw Win32 and not boost.process v2 here:
//   boost.process v2's default launcher builds a STARTUPINFOEX with a
//   PROC_THREAD_ATTRIBUTE_HANDLE_LIST, and that path deadlocks under MinGW.
//   MinGW's _popen is no better (the pipe never reaches EOF). A plain
//   CreateProcessW with STARTF_USESTDHANDLES + bInheritHandles=TRUE (no
//   STARTUPINFOEX, no handle list) is the classic, reliable form and works
//   under both MinGW and MSVC.

#ifndef NOMINMAX
# define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <thread>
#include <utility>

namespace cc::basic
{
namespace
{

// RAII CloseHandle. INVALID_HANDLE_VALUE is treated as "owns nothing".
struct handle_guard
{
    HANDLE h = nullptr;
    handle_guard() = default;
    explicit handle_guard(HANDLE x) : h(x) {}
    handle_guard(const handle_guard &) = delete;
    handle_guard &operator=(const handle_guard &) = delete;
    ~handle_guard() { reset(); }
    void reset(HANDLE x = nullptr)
    {
        if (h && h != INVALID_HANDLE_VALUE)
            ::CloseHandle(h);
        h = x;
    }
    HANDLE get() const { return h; }
};

std::wstring to_wide(std::string_view s)
{
    // Paths in the pipeline are ASCII; a char→wchar_t widening suffices. A
    // production version would use MultiByteToWideChar(CP_UTF8).
    return std::wstring(s.begin(), s.end());
}

std::string drain(HANDLE h)
{
    std::string out;
    char        buf[4096];
    DWORD       n = 0;
    while (::ReadFile(h, buf, static_cast<DWORD>(sizeof(buf)), &n, nullptr) && n > 0)
    {
        out.append(buf, n);
    }
    return out;
}

} // namespace

std::expected<exec_result, exec_error>
basic_executor<win32_tag>::exec(std::string_view        exe,
                                std::string_view        args,
                                std::optional<unsigned> timeout_ms)
{
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE }; // inheritable by default

    HANDLE out_r = nullptr, out_w = nullptr;
    HANDLE err_r = nullptr, err_w = nullptr;
    if (!::CreatePipe(&out_r, &out_w, &sa, 0) || !::CreatePipe(&err_r, &err_w, &sa, 0))
    {
        if (out_r) ::CloseHandle(out_r);
        if (out_w) ::CloseHandle(out_w);
        if (err_r) ::CloseHandle(err_r);
        if (err_w) ::CloseHandle(err_w);
        return std::unexpected(exec_error {"CreatePipe failed", false});
    }
    handle_guard g_out_r(out_r), g_out_w(out_w), g_err_r(err_r), g_err_w(err_w);

    // Child stdin → NUL (inheritable so the child actually gets it).
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE)
        return std::unexpected(exec_error {"CreateFile(NUL) failed", false});
    handle_guard g_nul(nul);

    // Parent keeps the read ends; make them non-inheritable so only the child's
    // write ends (and NUL) cross the boundary.
    ::SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = nul;
    si.hStdOutput = out_w;
    si.hStdError  = err_w;
    PROCESS_INFORMATION pi{};

    std::wstring wexe = to_wide(exe);
    std::wstring wcmd = L"\"" + wexe + L"\"";
    if (!args.empty())
    {
        wcmd += L" ";
        wcmd += to_wide(args);
    }

    if (!::CreateProcessW(wexe.c_str(), wcmd.data(),
                          nullptr, nullptr, /*bInheritHandles=*/TRUE,
                          0, nullptr, nullptr, &si, &pi))
    {
        return std::unexpected(exec_error {
            "CreateProcessW failed (err=" + std::to_string(::GetLastError()) + ")", false});
    }
    handle_guard g_proc(pi.hProcess), g_thread(pi.hThread);

    // Drop the parent's copies of the write ends + NUL so the read pipes see EOF
    // once the child exits/closes its inherited ends.
    g_out_w.reset();
    g_err_w.reset();
    g_nul.reset();

    // Drain stdout and stderr concurrently — a single-threaded sequential read
    // would deadlock on a child that fills the stderr pipe while we block on
    // stdout. Each ReadFile loop returns when the write end is closed (EOF).
    std::string out, err;
    std::thread out_t([&] { out = drain(g_out_r.get()); });
    std::thread err_t([&] { err = drain(g_err_r.get()); });
    out_t.join();
    err_t.join();

    DWORD wait = ::WaitForSingleObject(g_proc.get(), timeout_ms ? *timeout_ms : INFINITE);
    if (wait == WAIT_TIMEOUT)
    {
        ::TerminateProcess(g_proc.get(), 1);
        ::WaitForSingleObject(g_proc.get(), INFINITE);
        return std::unexpected(exec_error {"timeout", true});
    }
    DWORD exit_code = 0;
    ::GetExitCodeProcess(g_proc.get(), &exit_code);
    return exec_result { static_cast<long>(exit_code), std::move(out), std::move(err) };
}

} // namespace cc::basic
