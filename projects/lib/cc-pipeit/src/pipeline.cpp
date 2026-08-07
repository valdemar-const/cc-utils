#include "cc/pipeline.hpp"

#include <cc/plugin.hpp>

#include <boost/dll/shared_library.hpp>
#include <boost/system/error_code.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __linux__
#include <limits.h>
#include <unistd.h>
#endif

namespace cc::pipeit {

namespace {

using lib_t = boost::dll::shared_library;

std::string plugin_filename(std::string_view name) {
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

std::string exe_dir() {
#ifdef __linux__
  char buf[PATH_MAX];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf));
  if (n > 0) {
    std::string p(buf, static_cast<size_t>(n));
    auto slash = p.find_last_of('/');
    if (slash != std::string::npos) return p.substr(0, slash);
  }
#endif
  return ".";
}

std::vector<std::string> search_dirs() {
  std::vector<std::string> dirs;
  if (const char* env = std::getenv("CCP_PLUGIN_PATH"); env && *env) {
    std::string s{env};
    std::size_t beg = 0;
    while (true) {
      auto sep = s.find(':', beg);
      if (sep == std::string::npos) {
        dirs.emplace_back(s.substr(beg));
        break;
      }
      dirs.emplace_back(s.substr(beg, sep - beg));
      beg = sep + 1;
    }
  }
  std::string ed = exe_dir();
  dirs.push_back(ed);
  dirs.push_back(ed + "/../lib");
  dirs.push_back(".");
  return dirs;
}

// Try each search dir; on success `lib` holds the handle. Returns err string.
std::string open_plugin(lib_t& lib, std::string_view name) {
  std::string fname = plugin_filename(name);
  for (auto const& d : search_dirs()) {
    boost::system::error_code ec;
    lib.load(d + "/" + fname, ec);
    if (lib.is_loaded()) return {};
  }
  return "cc-plugin-" + std::string{name} +
         " not found (set CCP_PLUGIN_PATH or run from the build dir)";
}

// dlopen + resolve + api-check one stage. Empty string = ok.
template <typename PluginFn>
std::string resolve(lib_t& so, std::string_view name, std::string_view kind,
                    std::string_view factory_sym, PluginFn& out) {
  if (auto e = open_plugin(so, name); !e.empty()) return e;
  auto info = so.get<cc::plugin_info()>("cc_plugin_load")();
  if (info.api_version != cc::plugin_api_version) {
    return std::string{kind} + " '" + std::string{name} + "' api " +
           std::to_string(info.api_version) + " != host " +
           std::to_string(cc::plugin_api_version) + " (rebuild plugin)";
  }
  out = so.get<PluginFn()>(std::string{factory_sym}.c_str())();
  return out ? std::string{} : std::string{kind} + " '" + std::string{name} +
                                    "': missing " + std::string{factory_sym};
}

}  // namespace

struct pipeline::impl {
  lib_t front_so;
  lib_t irgen_so;
  lib_t back_so;
  cc::frontend* front = nullptr;
  cc::ir_generator* irgen = nullptr;
  cc::backend* back = nullptr;
};

pipeline::pipeline() : pimpl_(std::make_unique<impl>()) {}
pipeline::pipeline(pipeline&&) noexcept = default;
pipeline& pipeline::operator=(pipeline&&) noexcept = default;
pipeline::~pipeline() = default;

bool pipeline::run(std::string_view source, std::string_view out_path) const {
  if (!pimpl_->front || !pimpl_->irgen || !pimpl_->back) return false;

  auto ast = pimpl_->front->parse(source);  // source -> erased AST
  if (!ast) return false;

  cc::ir::module mod;  // the narrow waist: language-neutral from here on
  if (!pimpl_->irgen->generate(*ast, mod)) return false;

  return pimpl_->back->emit(mod, out_path);  // IR -> target exe
}

std::expected<pipeline, std::string> pipeline_builder::build() const {
  pipeline::impl state;
  if (auto e = resolve(state.front_so, front_, "frontend", "cc_plugin_frontend",
                       state.front);
      !e.empty()) {
    return std::unexpected(std::move(e));
  }
  if (auto e = resolve(state.irgen_so, irgen_, "irgen", "cc_plugin_irgen",
                       state.irgen);
      !e.empty()) {
    return std::unexpected(std::move(e));
  }
  if (auto e = resolve(state.back_so, back_, "backend", "cc_plugin_backend",
                       state.back);
      !e.empty()) {
    return std::unexpected(std::move(e));
  }

  pipeline p;
  *p.pimpl_ = std::move(state);
  return p;
}

}  // namespace cc::pipeit
