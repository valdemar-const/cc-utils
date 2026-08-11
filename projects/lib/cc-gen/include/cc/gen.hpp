#pragma once

#include "cc-gen_export.hpp"

#include <cc/ir.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cc::nasm
{

// A typed, POD representation of one x86_64 NASM instruction. The backend
// lowers ir::instr -> vector<nasm::instr> -> text listing (format).
enum class mnemonic : std::uint8_t
{
    mov,
    syscall
};
enum class operand_kind : std::uint8_t
{
    none,
    reg,
    imm
};
enum class reg : std::uint8_t
{
    rax,
    rdi
};

struct operand
{
    operand_kind  kind = operand_kind::none;
    cc::nasm::reg reg  = cc::nasm::reg::rax;
    std::int64_t  imm  = 0;
};

struct instr
{
    mnemonic mn = mnemonic::mov;
    operand  ops[2];
};

} // namespace cc::nasm

namespace cc::gen
{

[[nodiscard]] CC_GEN_API std::string_view version() noexcept;

// Lower each IR instruction to a sequence of NASM instructions (POD).
// e.g. {ret, 42} -> [mov rax,60; mov rdi,42; syscall].
[[nodiscard]] CC_GEN_API std::vector<cc::nasm::instr>
                         lower(const cc::ir::module &mod);

// Stringify the POD instruction vector into a NASM source listing (prologue
// + _start entry + one line per instruction), ready for `nasm -f elf64`.
[[nodiscard]] CC_GEN_API std::string
                         format(const std::vector<cc::nasm::instr> &instrs);

} // namespace cc::gen
