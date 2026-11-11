#include "vasyan/opcodes.h"

namespace vasyan {

// The readable opcode-name table is a gift to a reverse engineer, so it is
// compiled only for debug/disassembler builds. In a release build (NDEBUG) it
// collapses to a single opaque string, leaving no symbol-to-meaning map in the
// binary. Define VASYAN_DISASM to force the full table (e.g. for tooling).
#if !defined(NDEBUG) || defined(VASYAN_DISASM)

const char* opName(Op op) {
    switch (op) {
        case Op::CONST:        return "CONST";
        case Op::NIL:          return "NIL";
        case Op::TRUE:         return "TRUE";
        case Op::FALSE:        return "FALSE";
        case Op::POP:          return "POP";
        case Op::GET_LOCAL:    return "GET_LOCAL";
        case Op::SET_LOCAL:    return "SET_LOCAL";
        case Op::GET_GLOBAL:   return "GET_GLOBAL";
        case Op::SET_GLOBAL:   return "SET_GLOBAL";
        case Op::DEF_GLOBAL:   return "DEF_GLOBAL";
        case Op::GET_FUNC:     return "GET_FUNC";
        case Op::ADD:          return "ADD";
        case Op::SUB:          return "SUB";
        case Op::MUL:          return "MUL";
        case Op::DIV:          return "DIV";
        case Op::MOD:          return "MOD";
        case Op::NEG:          return "NEG";
        case Op::EQ:           return "EQ";
        case Op::NEQ:          return "NEQ";
        case Op::LT:           return "LT";
        case Op::LE:           return "LE";
        case Op::GT:           return "GT";
        case Op::GE:           return "GE";
        case Op::NOT:          return "NOT";
        case Op::JMP:          return "JMP";
        case Op::JMP_IF_FALSE: return "JMP_IF_FALSE";
        case Op::LOOP:         return "LOOP";
        case Op::CALL:         return "CALL";
        case Op::RET:          return "RET";
        case Op::PRINT:        return "PRINT";
        case Op::BUILD_LIST:   return "BUILD_LIST";
        case Op::BUILD_DICT:   return "BUILD_DICT";
        case Op::INDEX_GET:    return "INDEX_GET";
        case Op::INDEX_SET:    return "INDEX_SET";
        case Op::HALT:         return "HALT";
    }
    return "???";
}

#else   // release: no meaning-revealing names

const char* opName(Op) { return "op"; }

#endif

} // namespace vasyan
