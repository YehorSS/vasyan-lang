#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "vasyan/module.h"

namespace vasyan {

// --- Bytecode validator ------------------------------------------------------
//
// Bytecode arrives from outside (a file, the network), so the VM must not take
// its shape on trust. The validator proves, before a function ever executes,
// that:
//
//   * every byte decodes as a known opcode with all of its operand bytes;
//   * constant, function and local-slot operands are in range, and global
//     names refer to string constants;
//   * every jump lands on the start of an instruction inside the function;
//   * on every reachable path the operand stack never underflows, has the same
//     depth wherever two paths meet, and execution cannot run off the end.
//
// The interpreter loop relies on these guarantees instead of re-checking each
// stack access. What remains dynamic (types, indices, limits) is checked at
// run time by the VM.

struct FunctionCheck {
    bool        ok = false;
    std::string error;          // why it was rejected (when !ok)
    size_t      offset = 0;     // offending instruction offset (when !ok)
    uint32_t    maxStack = 0;   // deepest stack use, counted from the frame's slot 0
};

// Validate one function. Its code must be present (decrypted).
FunctionCheck validateFunction(const Module& module, size_t index);

struct ModuleCheck {
    bool        ok = false;
    std::string error;          // includes the function name and offset
};

// Validate the entry point and every function of a plaintext module.
ModuleCheck validateModule(const Module& module);

} // namespace vasyan
