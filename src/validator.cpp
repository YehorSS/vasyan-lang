#include "vasyan/validator.h"
#include "vasyan/opcodes.h"
#include <utility>
#include <vector>

namespace vasyan {
namespace {

// What an instruction does to the operand stack: it needs `need` values to be
// present and changes the depth by `delta`.
struct StackEffect {
    int64_t need;
    int64_t delta;
};

StackEffect stackEffect(Op op, uint32_t operand) {
    switch (op) {
        case Op::CONST: case Op::NIL: case Op::TRUE: case Op::FALSE:
        case Op::GET_LOCAL: case Op::GET_GLOBAL: case Op::GET_FUNC:
            return {0, 1};
        case Op::POP: case Op::DEF_GLOBAL: case Op::PRINT:
            return {1, -1};
        case Op::SET_LOCAL: case Op::SET_GLOBAL:
        case Op::NEG: case Op::NOT:
        case Op::JMP_IF_FALSE:
        case Op::RET:
            return {1, 0};
        case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV: case Op::MOD:
        case Op::EQ: case Op::NEQ: case Op::LT: case Op::LE: case Op::GT: case Op::GE:
        case Op::INDEX_GET:
            return {2, -1};
        case Op::INDEX_SET:
            return {3, -2};
        case Op::CALL:         // callee + argc arguments -> result
            return {int64_t(operand) + 1, -int64_t(operand)};
        case Op::BUILD_LIST:
            return {int64_t(operand), 1 - int64_t(operand)};
        case Op::BUILD_DICT:   // operand = number of key/value pairs
            return {int64_t(operand) * 2, 1 - int64_t(operand) * 2};
        case Op::JMP: case Op::LOOP: case Op::HALT:
            return {0, 0};
    }
    return {0, 0};
}

bool isPrimitiveConstant(const Value& v) {
    switch (v.type) {
        case ValueType::Nil: case ValueType::Bool:
        case ValueType::Int: case ValueType::Double:
            return true;
        case ValueType::Str:
            return v.s != nullptr;
        default:
            return false;
    }
}

FunctionCheck reject(size_t offset, std::string msg) {
    FunctionCheck r;
    r.ok = false;
    r.offset = offset;
    r.error = std::move(msg);
    return r;
}

} // namespace

FunctionCheck validateFunction(const Module& module, size_t index) {
    if (index >= module.functions.size()) return reject(0, "немає такої функції");
    const FunctionDef& fn = module.functions[index];
    const std::vector<uint8_t>& code = fn.code;
    const size_t size = code.size();
    if (size == 0) return reject(0, "порожній код функції");

    for (const Value& c : fn.constants)
        if (!isPrimitiveConstant(c)) return reject(0, "недопустима константа");

    // Operand of the instruction at `ip` (already known to be fully present).
    auto operandAt = [&](size_t ip, int width) -> uint32_t {
        if (width == 1) return code[ip + 1];
        if (width == 2) return uint32_t(code[ip + 1]) | (uint32_t(code[ip + 2]) << 8);
        return 0;
    };
    // Destination of a jump instruction at `ip`; mirrors the VM, which applies
    // the signed 16-bit offset after reading the operand.
    auto jumpTarget = [&](size_t ip, Op op) -> int64_t {
        int16_t off = static_cast<int16_t>(operandAt(ip, 2));
        int64_t next = int64_t(ip) + 3;
        return op == Op::LOOP ? next - off : next + off;
    };

    // Pass 1: decode linearly. Every byte must belong to a well-formed
    // instruction, including code no path reaches.
    std::vector<uint8_t> isStart(size, 0);
    for (size_t ip = 0; ip < size;) {
        if (code[ip] > static_cast<uint8_t>(Op::HALT))
            return reject(ip, "невідомий опкод");
        Op op = static_cast<Op>(code[ip]);
        int width = operandBytes(op);
        if (size - ip - 1 < size_t(width))
            return reject(ip, "обірвана інструкція");
        isStart[ip] = 1;

        uint32_t operand = operandAt(ip, width);
        switch (op) {
            case Op::CONST:
                if (operand >= fn.constants.size())
                    return reject(ip, "індекс константи за межами");
                break;
            case Op::GET_GLOBAL: case Op::SET_GLOBAL: case Op::DEF_GLOBAL:
                if (operand >= fn.constants.size())
                    return reject(ip, "індекс константи за межами");
                if (fn.constants[operand].type != ValueType::Str)
                    return reject(ip, "ім'я глобальної змінної не є рядком");
                break;
            case Op::GET_FUNC:
                if (operand >= module.functions.size())
                    return reject(ip, "індекс функції за межами");
                break;
            default:
                break;
        }
        ip += 1 + size_t(width);
    }

    // Jump targets can only be checked once all instruction starts are known.
    for (size_t ip = 0; ip < size; ip += 1 + size_t(operandBytes(static_cast<Op>(code[ip])))) {
        Op op = static_cast<Op>(code[ip]);
        if (op != Op::JMP && op != Op::JMP_IF_FALSE && op != Op::LOOP) continue;
        int64_t target = jumpTarget(ip, op);
        if (target < 0 || target >= int64_t(size) || !isStart[size_t(target)])
            return reject(ip, "стрибок не на початок інструкції");
    }

    // Pass 2: follow every reachable path, tracking the stack depth relative
    // to the frame's first slot (arguments are already on the stack).
    std::vector<int64_t> depthAt(size, -1);
    std::vector<std::pair<size_t, int64_t>> pending;
    pending.emplace_back(0, int64_t(fn.arity));
    int64_t maxDepth = fn.arity;

    while (!pending.empty()) {
        size_t  ip    = pending.back().first;
        int64_t depth = pending.back().second;
        pending.pop_back();

        for (;;) {
            if (ip >= size) return reject(size - 1, "виконання виходить за межі коду");
            if (depthAt[ip] != -1) {
                if (depthAt[ip] != depth)
                    return reject(ip, "неузгоджена глибина стека");
                break;   // already explored from here
            }
            depthAt[ip] = depth;

            Op op = static_cast<Op>(code[ip]);
            int width = operandBytes(op);
            uint32_t operand = operandAt(ip, width);

            StackEffect fx = stackEffect(op, operand);
            if (depth < fx.need) return reject(ip, "спустошення стека");
            if ((op == Op::GET_LOCAL || op == Op::SET_LOCAL) && int64_t(operand) >= depth)
                return reject(ip, "локальна змінна за межами кадру");

            depth += fx.delta;
            if (depth > maxDepth) maxDepth = depth;

            if (op == Op::RET || op == Op::HALT) break;
            if (op == Op::JMP || op == Op::LOOP) {
                ip = size_t(jumpTarget(ip, op));
                continue;
            }
            if (op == Op::JMP_IF_FALSE)
                pending.emplace_back(size_t(jumpTarget(ip, op)), depth);
            ip += 1 + size_t(width);
        }
    }

    FunctionCheck r;
    r.ok = true;
    r.maxStack = uint32_t(maxDepth);
    return r;
}

ModuleCheck validateModule(const Module& module) {
    ModuleCheck r;
    if (module.functions.empty()) { r.error = "модуль без функцій"; return r; }
    if (module.entry >= module.functions.size()) {
        r.error = "точка входу за межами модуля";
        return r;
    }
    if (module.functions[module.entry].arity != 0) {
        r.error = "точка входу не може мати параметрів";
        return r;
    }
    for (size_t i = 0; i < module.functions.size(); ++i) {
        FunctionCheck fc = validateFunction(module, i);
        if (!fc.ok) {
            r.error = fc.error + " (функція '" + module.functions[i].name +
                      "', зсув " + std::to_string(fc.offset) + ")";
            return r;
        }
    }
    r.ok = true;
    return r;
}

} // namespace vasyan
