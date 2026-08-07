#include <cc/plugin.hpp>

#include <fstream>

namespace {
class x86_64_backend final : public cc::backend {
 public:
  bool emit(cc::ir::module const& mod, std::string_view out_path) override {
    // skeleton: writes a placeholder file. Real codegen (cc-gen) wired later.
    std::ofstream os{std::string{out_path}};
    if (!os) return false;
    (void)mod;
    return true;
  }
};
x86_64_backend g_instance;
}  // namespace

extern "C" cc::plugin_info cc_plugin_load() {
  return {cc::plugin_api_version, "x86_64", "backend"};
}

extern "C" cc::backend* cc_plugin_backend() {
  return &g_instance;
}
