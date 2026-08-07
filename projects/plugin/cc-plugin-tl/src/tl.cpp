#include <cc/plugin.hpp>

#include <cctype>
#include <string_view>

namespace {
class tl_frontend final : public cc::frontend {
 public:
  bool compile(std::string_view src, cc::ir::module& out) override {
    // MVP "parser": `return <digits>;` -> module.exit_code. (cc-parseit comes later.)
    auto pos = src.find("return");
    if (pos == std::string_view::npos) return false;
    pos += 6;
    int v = 0;
    bool any = false;
    while (pos < src.size()) {
      char c = src[pos];
      if (c >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        any = true;
      } else if (c == ';') {
        break;
      } else if (std::isspace(static_cast<unsigned char>(c))) {
        // whitespace tolerated
      } else {
        return false;
      }
      ++pos;
    }
    if (!any) return false;
    out.exit_code = v;
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
