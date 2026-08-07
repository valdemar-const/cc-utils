#include <cc/plugin.hpp>

namespace {
class tl_frontend final : public cc::frontend {
 public:
  bool compile(std::string_view source, cc::ir::module& out) override {
    (void)source;  // skeleton: no real parser yet (cc-parseit wired later).
    out = cc::ir::module{};
    return true;
  }
};
tl_frontend g_instance;
}  // namespace

extern "C" cc::plugin_info cc_plugin_load() {
  return {cc::plugin_api_version, "tl", "frontend"};
}

extern "C" cc::frontend* cc_plugin_frontend() {
  return &g_instance;
}
