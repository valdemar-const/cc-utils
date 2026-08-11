#include "cc/gen.hpp"

#include <string>

namespace cc::gen
{

namespace
{

    const char *
    reg_name(cc::nasm::reg r)
    {
        switch (r)
        {
        case cc::nasm::reg::rax:
            return "rax";
        case cc::nasm::reg::rdi:
            return "rdi";
        }
        return "?";
    }

} // namespace

std::string_view
version() noexcept
{
    return "0.1.0";
}

std::vector<cc::nasm::instr>
lower(const cc::ir::module &mod)
{
    std::vector<cc::nasm::instr> out;
    for (const auto &i : mod.code)
    {
        switch (i.op)
        {
        case cc::ir::opcode::ret:
            {
#if defined(_WIN32)
                // Win32/PE: we emit `main` returning imm in rax; the CRT startup
                // pulled in by `gcc` at link time calls ExitProcess(main()).
                // (Windows has no raw exit syscall — it goes through kernel32.)
                cc::nasm::instr a;
                a.mn     = cc::nasm::mnemonic::mov;
                a.ops[0] = {cc::nasm::operand_kind::reg, cc::nasm::reg::rax, 0};
                a.ops[1] = {cc::nasm::operand_kind::imm, cc::nasm::reg::rax, i.imm};
                out.push_back(a);

                cc::nasm::instr b;
                b.mn = cc::nasm::mnemonic::ret;
                out.push_back(b);
#else
                // Linux/ELF: sys_exit(imm) via the raw x86_64 syscall interface.
                cc::nasm::instr a;
                a.mn     = cc::nasm::mnemonic::mov;
                a.ops[0] = {cc::nasm::operand_kind::reg, cc::nasm::reg::rax, 0};
                a.ops[1] = {cc::nasm::operand_kind::imm, cc::nasm::reg::rax, 60}; // sys_exit
                out.push_back(a);

                cc::nasm::instr b;
                b.mn     = cc::nasm::mnemonic::mov;
                b.ops[0] = {cc::nasm::operand_kind::reg, cc::nasm::reg::rdi, 0};
                b.ops[1] = {cc::nasm::operand_kind::imm, cc::nasm::reg::rax, i.imm};
                out.push_back(b);

                cc::nasm::instr c;
                c.mn = cc::nasm::mnemonic::syscall;
                out.push_back(c);
#endif
                break;
            }
        }
    }
    return out;
}

std::string
format(const std::vector<cc::nasm::instr> &instrs)
{
    std::string s;
#if defined(_WIN32)
    // Win32/PE: entry symbol is `main` (CRT startup calls it, then ExitProcess).
    s += "global main\nsection .text\nmain:\n";
#else
    s += "global _start\nsection .text\n_start:\n";
#endif
    for (const auto &i : instrs)
    {
        s += "    ";
        switch (i.mn)
        {
        case cc::nasm::mnemonic::mov:
            {
                s += "mov ";
                if (i.ops[0].kind == cc::nasm::operand_kind::reg)
                {
                    s += reg_name(i.ops[0].reg);
                }
                s += ", ";
                if (i.ops[1].kind == cc::nasm::operand_kind::imm)
                {
                    s += std::to_string(i.ops[1].imm);
                }
                else if (i.ops[1].kind == cc::nasm::operand_kind::reg)
                {
                    s += reg_name(i.ops[1].reg);
                }
                break;
            }
        case cc::nasm::mnemonic::syscall:
            s += "syscall";
            break;
        case cc::nasm::mnemonic::ret:
            s += "ret";
            break;
        }
        s += "\n";
    }
    return s;
}

} // namespace cc::gen
