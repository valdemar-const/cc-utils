#include "cc/plugin_loader.hpp"
#include "cc/plugin_entry.hpp"
#include "cc/host.hpp"

#include <boost/dll/shared_library.hpp>
#include <boost/system/error_code.hpp>

#include <cstdio>  // std::fprintf (stderr)
#include <cstdlib> // std::getenv
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef __linux__
#include <limits.h>
#include <unistd.h> // readlink /proc/self/exe
#endif
#ifdef _WIN32
#include <windows.h> // GetModuleFileNameA
#endif

namespace cc::runtime
{

namespace
{

    namespace dll = boost::dll;

    std::string
    plugin_filename(std::string_view name)
    {
        std::string f = "cc-plugin-";
        f.append(name);
#if defined(_WIN32)
        f += ".dll";
#elif defined(__APPLE__)
        f += ".dylib";
#else
        f += ".so";
#endif
        return f;
    }

    std::string
    exe_dir()
    {
#if defined(__linux__)
        char    buf[PATH_MAX];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf));
        if (n > 0)
        {
            std::string p(buf, static_cast<size_t>(n));
            auto        slash = p.find_last_of('/');
            if (slash != std::string::npos)
            {
                return p.substr(0, slash);
            }
        }
#elif defined(_WIN32)
        char  buf[MAX_PATH];
        DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
        if (n > 0 && n < sizeof(buf))
        {
            std::string p(buf, static_cast<size_t>(n));
            auto        slash = p.find_last_of("\\/");
            if (slash != std::string::npos)
            {
                return p.substr(0, slash);
            }
        }
#endif
        return ".";
    }

    auto
    search_dirs() -> std::vector<std::string>
    {
#if defined(_WIN32)
        constexpr char sep = ';'; // PATH-style on Windows — ':' would slice "C:\"
#else
        constexpr char sep = ':';
#endif
        std::vector<std::string> dirs;
        if (const char *env = std::getenv("CCP_PLUGIN_PATH"); env && *env)
        {
            std::string s {env};
            std::size_t beg = 0;
            while (true)
            {
                auto p = s.find(sep, beg);
                if (p == std::string::npos)
                {
                    dirs.emplace_back(s.substr(beg));
                    break;
                }
                dirs.emplace_back(s.substr(beg, p - beg));
                beg = p + 1;
            }
        }
        // Plugins are runtime "assets" discovered relative to the host
        // executable. Two layouts are covered: installed (plugins/ is a
        // subdirectory of exe_dir, beside the binary) and the cc-utils build
        // tree (plugins/ is a sibling of bin/, i.e. one level up from exe_dir).
        std::string ed = exe_dir();
        dirs.push_back(ed + "/plugins");
        dirs.push_back(ed + "/../plugins");
        dirs.push_back(".");
        return dirs;
    }

} // namespace

struct plugin_loader::impl
{
    std::vector<std::unique_ptr<dll::shared_library>> libs;
};

plugin_loader::plugin_loader()
    : pimpl_(std::make_unique<impl>())
{
}

plugin_loader::~plugin_loader() = default;

auto
plugin_loader::default_search_dirs() -> std::vector<std::string>
{
    return search_dirs();
}

auto
plugin_loader::load_path(const std::string &path, host_registry &host) -> std::string
{
    auto                      lib = std::make_unique<dll::shared_library>();
    boost::system::error_code ec;
    lib->load(path, ec);
    if (!lib->is_loaded())
    {
        return "failed to load '" + path + "': " + ec.message();
    }

    // cc_plugin_load — version check.
    auto info_fn = lib->get<cc::plugin_info()>("cc_plugin_load");
    auto info    = info_fn();
    if (info.api_version != cc::plugin_api_version)
    {
        return std::string {path} + ": api " + std::to_string(info.api_version) + " != host " + std::to_string(cc::plugin_api_version) + " (rebuild plugin)";
    }

    // cc_plugin_register — populate the host.
    auto register_fn = lib->get<void(cc::host_registry &)>("cc_plugin_register");
    // Attribute every factory registered during this call to the plugin's
    // declared name. Scope guard guarantees pop_provider() even if register_fn
    // throws — keeps host bookkeeping consistent on partial failure.
    host.push_provider(info.name ? info.name : "");

    struct provider_guard
    {
        host_registry &h;

        ~provider_guard()
        {
            h.pop_provider();
        }
    } guard {host};

    register_fn(host);

    pimpl_->libs.push_back(std::move(lib));
    return {};
}

auto
plugin_loader::load(std::string_view name, host_registry &host) -> std::string
{
    std::string fname = plugin_filename(name);
    for (const auto &d : search_dirs())
    {
        std::string full = d + "/" + fname;
        if (!std::filesystem::exists(full))
        {
            continue;
        }
        std::string err = load_path(full, host);
        if (err.empty())
        {
            return {};
        }
        return err; // exists but failed to load → return the error
    }
    return "cc-plugin-" + std::string {name} + " not found (set CCP_PLUGIN_PATH or run from the build dir)";
}

auto
plugin_loader::load_all(host_registry &host) -> std::size_t
{
    std::size_t loaded = 0;
    for (const auto &d : search_dirs())
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(d, ec))
        {
            continue;
        }
        for (const auto &entry : std::filesystem::directory_iterator(d, ec))
        {
            auto path = entry.path();
            if (path.extension() != ".so" && path.extension() != ".dylib" && path.extension() != ".dll")
            {
                continue;
            }
            auto                       stem   = path.stem().string();
            constexpr std::string_view prefix = "cc-plugin-";
            if (stem.size() < prefix.size() || stem.substr(0, prefix.size()) != prefix)
            {
                continue;
            }

            std::string err = load_path(path.string(), host);
            if (!err.empty())
            {
                std::fprintf(stderr, "cc-runtime: skip %s: %s\n", path.string().c_str(), err.c_str());
                continue;
            }
            ++loaded;
        }
    }
    return loaded;
}

} // namespace cc::runtime
