#include <cc/plugin.hpp>

#include <cstdio>   // std::remove
#include <cstdlib>  // std::system
#include <fstream>
#include <string>

namespace {
class x86_64_backend final : public cc::backend {
 public:
  bool emit(cc::ir::module const& mod, std::string_view out_path) override {
    const std::string exe{out_path};
    const std::string asm_path = exe + ".asm";
    const std::string obj_path = exe + ".o";

    {
      std::ofstream os{asm_path};
      if (!os) return false;
      os << "global _start\n"
         << "section .text\n"
         << "_start:\n"
         << "    mov rax, 60\n"                       // sys_exit (Linux x86-64)
         << "    mov rdi, " << mod.exit_code << "\n"  // exit code
         << "    syscall\n";
    }
    if (std::system(("nasm -f elf64 " + asm_path + " -o " + obj_path).c_str()) != 0)
      return false;
    if (std::system(("ld " + obj_path + " -o " + exe).c_str()) != 0)
      return false;
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
