#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "vasyan/value.h"

namespace vasyan {

// A compiled function: its own bytecode plus a private constant pool.
// Constants are primitives only (nil/bool/int/double/str), which keeps
// serialization flat and self-contained — function references live in the
// bytecode as GET_FUNC <index> instead of in the constant pool.
struct FunctionDef {
    std::string           name;
    uint8_t               arity = 0;         // declared parameter count
    std::vector<Value>    constants;         // primitives only
    std::vector<uint8_t>  code;              // instruction stream
    std::vector<uint32_t> lines;             // source line per code byte (debug)
};

// A whole program. functions[0] is the implicit "<script>" entry unless
// entry is set otherwise. Cross-function calls reference indices here.
struct Module {
    std::vector<FunctionDef> functions;
    uint16_t                 entry = 0;

    // Binary serialization. The on-disk / on-wire form the loader consumes.
    // This is the representation you ship to clients (optionally encrypted).
    std::vector<uint8_t> serialize() const;

    // Parse a module from bytes. Throws std::runtime_error on malformed input.
    static Module deserialize(const std::vector<uint8_t>& bytes);
};

// Magic + version tag written at the head of a serialized module.
constexpr uint8_t  kMagic[4] = {'V', 'S', 'N', 'B'};
constexpr uint16_t kFormatVersion = 2;   // v2 adds the per-function line table

// Largest serialized module accepted from a file or the network.
constexpr size_t kMaxModuleBytes = 64 * 1024 * 1024;

} // namespace vasyan
