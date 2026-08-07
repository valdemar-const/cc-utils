#pragma once

#include "cc-ir_export.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace cc::ir {

[[nodiscard]] CC_IR_API std::string_view version() noexcept;

// A single three-address-ish operation. The narrow waist of the pipeline:
// language-specific on the way in (frontend/irgen), target-specific on the way
// out (backend). Everything between speaks only this.
enum class opcode : std::uint8_t {
  ret,  // exit with immediate
};

struct instr {
  opcode op = opcode::ret;
  std::int64_t imm = 0;
};

struct module {
  std::vector<instr> code;
};

}  // namespace cc::ir
