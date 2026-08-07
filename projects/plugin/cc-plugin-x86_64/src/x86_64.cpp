#include <cc/plugin.hpp>

#include <cc/gen.hpp>  // cc::gen::lower / format

#include <cstdio>   // std::remove
#include <cstdlib>  // std::system
#include <fstream>
#include <string>
#include <string_view>

namespace {

// Stage 3: IR -> executable. Language-agnostic. Lowers IR to a NASM POD
// instruction vector, stringifies it to a listing, then nasm + ld.
class x86_64_backend final : public cc::backend {
 public:
  bool emit(const cc::ir::module& mod, std::string_view out_path) override {
    const std::string exe{out_path};
    const std::string asm_path = exe + ".asm";
    const std::string obj_path = exe + ".o";

    {
      const std::vector<cc::nasm::instr> instrs = cc::gen::lower(mod);
      const std::string text = cc::gen::format(instrs);
      std::ofstream os{asm_path};
      if (!os) return false;
      os << text;
    }
    if (std::system(("nasm -f elf64 " + asm_path + " -o " + obj_path).c_str()) != 0) {
      return false;
    }
    if (std::system(("ld " + obj_path + " -o " + exe).c_str()) != 0) {
      return false;
    }
    std::remove(asm_path.c_str());
    std::remove(obj_path.c_str());
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
