#pragma once
#include <cstdint>

namespace vasyan {

// Bytecode instruction set for the VasyanLang stack machine.
// Each opcode is a single byte, optionally followed by fixed-width operands
// (little-endian). Operand widths are documented per-op and centralized in
// operandBytes() so the disassembler and (de)serializer stay in sync.
enum class Op : uint8_t {
    CONST,        // u16: push constants[idx]
    NIL,          //      push nil
    TRUE,         //      push true
    FALSE,        //      push false
    POP,          //      discard top

    GET_LOCAL,    // u8:  push locals[slot]
    SET_LOCAL,    // u8:  locals[slot] = peek (value stays on stack)
    GET_GLOBAL,   // u16: push globals[name=constants[idx]]
    SET_GLOBAL,   // u16: globals[name] = peek
    DEF_GLOBAL,   // u16: globals[name] = pop
    GET_FUNC,     // u16: push function value for module.functions[idx]

    ADD, SUB, MUL, DIV, MOD, NEG,
    EQ, NEQ, LT, LE, GT, GE,
    NOT,

    JMP,          // i16: ip += offset
    JMP_IF_FALSE, // i16: if !peek.truthy: ip += offset (does not pop)
    LOOP,         // i16: ip -= offset

    CALL,         // u8:  call with argc arguments
    RET,          //      return top of stack to caller
    PRINT,        //      pop and print

    BUILD_LIST,   // u16: pop N values, push a list of them (in order)
    BUILD_DICT,   // u16: pop N key/value pairs (value on top), push a dict
    INDEX_GET,    //      pop index, pop container; push container[index]
    INDEX_SET,    //      pop value, index, container; set; push value back

    HALT          //      stop the VM
};

// Number of operand bytes that follow the given opcode.
inline int operandBytes(Op op) {
    switch (op) {
        case Op::CONST:
        case Op::GET_GLOBAL:
        case Op::SET_GLOBAL:
        case Op::DEF_GLOBAL:
        case Op::GET_FUNC:
        case Op::JMP:
        case Op::JMP_IF_FALSE:
        case Op::LOOP:
        case Op::BUILD_LIST:
        case Op::BUILD_DICT:
            return 2;
        case Op::GET_LOCAL:
        case Op::SET_LOCAL:
        case Op::CALL:
            return 1;
        default:
            return 0;
    }
}

const char* opName(Op op);

} // namespace vasyan
