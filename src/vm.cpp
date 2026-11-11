#include "vasyan/vm.h"
#include "vasyan/opcodes.h"
#include "vasyan/validator.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

namespace vasyan {
namespace {

// --- UTF-8 helpers ---------------------------------------------------------
// Strings are treated as sequences of Unicode codepoints, not raw bytes, so
// that `довжина` and indexing behave intuitively for Cyrillic (and any other
// multi-byte) text. A byte is a codepoint start unless it is a continuation
// byte (0b10xxxxxx).
inline bool isContinuation(unsigned char b) { return (b & 0xC0) == 0x80; }

size_t utf8Count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if (!isContinuation(c)) ++n;
    return n;
}

// Return the byte range [start, end) of the codepoint at character index `idx`,
// or set ok=false if idx is out of range.
std::string utf8At(const std::string& s, size_t idx, bool& ok) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        size_t start = i++;
        while (i < s.size() && isContinuation((unsigned char)s[i])) ++i;
        if (chars == idx) { ok = true; return s.substr(start, i - start); }
        ++chars;
    }
    ok = false;
    return std::string();
}

// --- Integer arithmetic ----------------------------------------------------
// Int is a 64-bit two's-complement value that wraps on overflow. Going through
// uint64_t keeps that well-defined in C++ (signed overflow is not).
inline int64_t wrapAdd(int64_t a, int64_t b) { return int64_t(uint64_t(a) + uint64_t(b)); }
inline int64_t wrapSub(int64_t a, int64_t b) { return int64_t(uint64_t(a) - uint64_t(b)); }
inline int64_t wrapMul(int64_t a, int64_t b) { return int64_t(uint64_t(a) * uint64_t(b)); }
inline int64_t wrapNeg(int64_t a)            { return int64_t(uint64_t(0) - uint64_t(a)); }

} // namespace

VM::VM(std::ostream& out) : out_(out), in_(&std::cin) {}
VM::VM(std::ostream& out, std::istream& in) : out_(out), in_(&in) {}

std::istream& VM::input() { return *in_; }

uint8_t VM::readByte(Frame& f) { return f.fn->code[f.ip++]; }

uint16_t VM::readU16(Frame& f) {
    uint16_t lo = f.fn->code[f.ip++];
    uint16_t hi = f.fn->code[f.ip++];
    return uint16_t(lo | (hi << 8));
}

int16_t VM::readI16(Frame& f) {
    return static_cast<int16_t>(readU16(f));
}

bool VM::runtimeError(const std::string& msg) {
    error_ = msg;
    if (!frames_.empty()) {
        const Frame& f = frames_.back();
        // ip points just past the last byte read; map it back to a source line.
        uint32_t line = 0;
        if (f.ip > 0 && f.ip - 1 < f.fn->lines.size()) line = f.fn->lines[f.ip - 1];
        else if (!f.fn->lines.empty()) line = f.fn->lines.back();
        error_ += " (рядок " + std::to_string(line) +
                  ", у функції '" + f.fn->name + "')";
    }
    return false;
}

bool VM::binaryNumeric(Op op) {
    // String concatenation for '+'.
    if (op == Op::ADD && peek(0).type == ValueType::Str && peek(1).type == ValueType::Str) {
        if (peek(0).s->size() + peek(1).s->size() > config_.maxStringSize)
            return runtimeError("рядок завеликий");
        Value b = pop(), a = pop();
        push(Value::str(*a.s + *b.s));
        return true;
    }
    if (!peek(0).isNumber() || !peek(1).isNumber())
        return runtimeError("операнди мають бути числами");

    Value b = pop(), a = pop();
    bool bothInt = a.type == ValueType::Int && b.type == ValueType::Int;

    if (bothInt) {
        int64_t x = a.i, y = b.i;
        switch (op) {
            case Op::ADD: push(Value::integer(wrapAdd(x, y))); return true;
            case Op::SUB: push(Value::integer(wrapSub(x, y))); return true;
            case Op::MUL: push(Value::integer(wrapMul(x, y))); return true;
            // y == -1 is handled apart: INT64_MIN / -1 traps on x86.
            case Op::DIV:
                if (y == 0) return runtimeError("ділення на нуль");
                push(Value::integer(y == -1 ? wrapNeg(x) : x / y)); return true;
            case Op::MOD:
                if (y == 0) return runtimeError("остача від ділення на нуль");
                push(Value::integer(y == -1 ? 0 : x % y)); return true;
            default: break;
        }
    } else {
        double x = a.asDouble(), y = b.asDouble();
        switch (op) {
            case Op::ADD: push(Value::number(x + y)); return true;
            case Op::SUB: push(Value::number(x - y)); return true;
            case Op::MUL: push(Value::number(x * y)); return true;
            case Op::DIV:
                if (y == 0.0) return runtimeError("ділення на нуль");
                push(Value::number(x / y)); return true;
            case Op::MOD:
                return runtimeError("остача визначена лише для цілих");
            default: break;
        }
    }
    return runtimeError("невідома арифметична операція");
}

bool VM::compare(Op op) {
    if (!peek(0).isNumber() || !peek(1).isNumber())
        return runtimeError("порівнювати >,<,>=,<= можна лише числа");
    Value b = pop(), a = pop();
    double x = a.asDouble(), y = b.asDouble();
    switch (op) {
        case Op::LT: push(Value::boolean(x <  y)); return true;
        case Op::LE: push(Value::boolean(x <= y)); return true;
        case Op::GT: push(Value::boolean(x >  y)); return true;
        case Op::GE: push(Value::boolean(x >= y)); return true;
        default: return runtimeError("невідоме порівняння");
    }
}

bool VM::callFunction(int funcIndex, uint8_t argc) {
    if (funcIndex < 0 || size_t(funcIndex) >= module_->functions.size())
        return runtimeError("виклик неіснуючої функції");
    const FunctionDef* fn = &module_->functions[funcIndex];
    if (argc != fn->arity)
        return runtimeError("функція '" + fn->name + "' чекає " +
                            std::to_string(fn->arity) + " арг., дали " + std::to_string(argc));
    if (frames_.size() >= config_.maxCallDepth)
        return runtimeError("переповнення стека викликів");

    // First call: prove the bytecode is well-formed before running any of it.
    Checked& checked = checked_[size_t(funcIndex)];
    if (!checked.done) {
        FunctionCheck fc = validateFunction(*module_, size_t(funcIndex));
        if (!fc.ok) {
            return runtimeError("байткод не пройшов перевірку: " + fc.error +
                                " (функція '" + fn->name + "', зсув " +
                                std::to_string(fc.offset) + ")");
        }
        checked.done = true;
        checked.maxStack = fc.maxStack;
    }
    // The validator knows how deep this frame can grow, so the stack limit is
    // checked once per call rather than on every push.
    if (stack_.size() - argc + checked.maxStack > config_.maxStackSize) {
        return runtimeError("переповнення стека значень");
    }

    Frame f;
    f.fn = fn;
    f.ip = 0;
    f.slotBase = stack_.size() - argc;  // first argument becomes local slot 0
    frames_.push_back(f);
    return true;
}

// Register built-in functions as globals. `argc` for each is validated inside
// its lambda (via args.size()) so we keep the native table flat.
void VM::registerNatives() {
    natives_.clear();
    auto add = [&](const std::string& name, NativeFn fn) {
        globals_[name] = Value::nativeFn(int(natives_.size()));
        natives_.push_back(std::move(fn));
    };
    auto wrongArgs = [](std::string& err, const char* n) {
        err = std::string("невірна кількість аргументів для '") + n + "'";
        return false;
    };

    add("довжина", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 1) return wrongArgs(e, "довжина");
        switch (a[0].type) {
            case ValueType::Str:  r = Value::integer(int64_t(a[0].s ? utf8Count(*a[0].s) : 0)); return true;
            case ValueType::List: r = Value::integer(int64_t(a[0].list ? a[0].list->size() : 0)); return true;
            case ValueType::Dict: r = Value::integer(int64_t(a[0].dict ? a[0].dict->size() : 0)); return true;
            default: e = "довжина: очікує рядок, список або словник"; return false;
        }
    });

    add("текст", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 1) return wrongArgs(e, "текст");
        r = Value::str(a[0].toString());
        return true;
    });

    add("число", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 1) return wrongArgs(e, "число");
        if (a[0].isNumber()) { r = a[0]; return true; }
        if (a[0].type != ValueType::Str) { e = "число: очікує рядок або число"; return false; }
        const std::string& s = *a[0].s;
        if (s.find('.') != std::string::npos || s.find('e') != std::string::npos)
            r = Value::number(std::strtod(s.c_str(), nullptr));
        else
            r = Value::integer(int64_t(std::strtoll(s.c_str(), nullptr, 10)));
        return true;
    });

    if (config_.allowInput)
    add("ввід", [](VM& vm, std::vector<Value>& a, Value& r, std::string&) {
        if (a.size() == 1) vm.out_ << a[0].toString();   // optional prompt
        std::string line;
        std::getline(vm.input(), line);
        r = Value::str(line);
        return true;
    });

    add("корінь", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 1 || !a[0].isNumber()) return wrongArgs(e, "корінь");
        r = Value::number(std::sqrt(a[0].asDouble()));
        return true;
    });

    add("абс", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 1 || !a[0].isNumber()) return wrongArgs(e, "абс");
        if (a[0].type == ValueType::Int) r = Value::integer(a[0].i < 0 ? wrapNeg(a[0].i) : a[0].i);
        else r = Value::number(std::fabs(a[0].d));
        return true;
    });

    add("додати", [wrongArgs](VM& vm, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 2 || a[0].type != ValueType::List || !a[0].list)
            return wrongArgs(e, "додати");
        if (a[0].list->size() >= vm.config_.maxListSize) { e = "список завеликий"; return false; }
        a[0].list->push_back(a[1]);
        r = a[0];
        return true;
    });

    add("прибрати", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 1 || a[0].type != ValueType::List || !a[0].list)
            return wrongArgs(e, "прибрати");
        if (a[0].list->empty()) { e = "прибрати: список порожній"; return false; }
        r = a[0].list->back();
        a[0].list->pop_back();
        return true;
    });

    add("ключі", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 1 || a[0].type != ValueType::Dict || !a[0].dict)
            return wrongArgs(e, "ключі");
        r = Value::newList();
        for (const auto& kv : *a[0].dict) r.list->push_back(Value::str(kv.first));
        return true;
    });

    add("є_ключ", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 2 || a[0].type != ValueType::Dict || a[1].type != ValueType::Str)
            return wrongArgs(e, "є_ключ");
        r = Value::boolean(a[0].dict && a[0].dict->count(*a[1].s) > 0);
        return true;
    });

    add("випадкове", [wrongArgs](VM&, std::vector<Value>& a, Value& r, std::string& e) {
        if (a.size() != 2 || a[0].type != ValueType::Int || a[1].type != ValueType::Int)
            return wrongArgs(e, "випадкове");
        if (a[1].i < a[0].i) { e = "випадкове: верхня межа менша за нижню"; return false; }
        static std::mt19937_64 gen{std::random_device{}()};
        std::uniform_int_distribution<int64_t> dist(a[0].i, a[1].i);
        r = Value::integer(dist(gen));
        return true;
    });

    for (const auto& host : hostNatives_) add(host.first, host.second);
}

bool VM::callNative(int nativeIndex, uint8_t argc) {
    if (nativeIndex < 0 || size_t(nativeIndex) >= natives_.size())
        return runtimeError("виклик неіснуючої вбудованої функції");
    std::vector<Value> args(stack_.end() - argc, stack_.end());
    Value result;
    std::string err;
    bool ok = natives_[nativeIndex](*this, args, result, err);
    if (!ok) return runtimeError(err.empty() ? "помилка вбудованої функції" : err);
    if (result.type == ValueType::Str && result.s && result.s->size() > config_.maxStringSize)
        return runtimeError("рядок завеликий");
    stack_.resize(stack_.size() - argc - 1);  // drop args + the callee slot
    push(std::move(result));
    return true;
}

InterpretResult VM::run(Module& module) {
    module_ = &module;
    stack_.clear();
    frames_.clear();
    globals_.clear();
    error_.clear();
    checked_.assign(module.functions.size(), Checked{});
    registerNatives();

    InterpretResult result = execute();

    frames_.clear();
    return result;
}

InterpretResult VM::execute() {
    // Seed the entry frame: a callee slot for the entry function, then call it.
    push(Value::function(module_->entry));
    if (!callFunction(module_->entry, 0))
        return InterpretResult::RuntimeError;

    using Clock = std::chrono::steady_clock;
    const Clock::time_point started = Clock::now();
    uint64_t executed = 0;

    for (;;) {
        Frame* frame = &frames_.back();
        if (frame->ip >= frame->fn->code.size())
            return runtimeError("виконання вийшло за межі коду"), InterpretResult::RuntimeError;

        ++executed;
        if (config_.maxInstructions && executed > config_.maxInstructions)
            return runtimeError("перевищено ліміт інструкцій"), InterpretResult::RuntimeError;
        // The clock is comparatively slow, so look at it only now and then.
        if (config_.maxExecutionMs && (executed & 0xFFF) == 0) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now() - started).count();
            if (uint64_t(ms) > config_.maxExecutionMs)
                return runtimeError("перевищено ліміт часу виконання"), InterpretResult::RuntimeError;
        }

        Op op = static_cast<Op>(readByte(*frame));
        switch (op) {
            case Op::CONST:      push(frame->fn->constants[readU16(*frame)]); break;
            case Op::NIL:        push(Value::nil()); break;
            case Op::TRUE:       push(Value::boolean(true)); break;
            case Op::FALSE:      push(Value::boolean(false)); break;
            case Op::POP:        pop(); break;

            case Op::GET_LOCAL: {
                uint8_t slot = readByte(*frame);
                push(stack_[frame->slotBase + slot]);
                break;
            }
            case Op::SET_LOCAL: {
                uint8_t slot = readByte(*frame);
                stack_[frame->slotBase + slot] = peek(0);
                break;
            }
            case Op::DEF_GLOBAL: {
                const Value& name = frame->fn->constants[readU16(*frame)];
                globals_[*name.s] = pop();
                break;
            }
            case Op::GET_GLOBAL: {
                const Value& name = frame->fn->constants[readU16(*frame)];
                auto it = globals_.find(*name.s);
                if (it == globals_.end())
                    return runtimeError("невизначена змінна '" + *name.s + "'"), InterpretResult::RuntimeError;
                push(it->second);
                break;
            }
            case Op::SET_GLOBAL: {
                const Value& name = frame->fn->constants[readU16(*frame)];
                auto it = globals_.find(*name.s);
                if (it == globals_.end())
                    return runtimeError("присвоєння невизначеній '" + *name.s + "'"), InterpretResult::RuntimeError;
                it->second = peek(0);
                break;
            }
            case Op::GET_FUNC:   push(Value::function(readU16(*frame))); break;

            case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV: case Op::MOD:
                if (!binaryNumeric(op)) return InterpretResult::RuntimeError;
                break;
            case Op::NEG: {
                if (!peek(0).isNumber())
                    return runtimeError("унарний мінус лише для чисел"), InterpretResult::RuntimeError;
                Value a = pop();
                push(a.type == ValueType::Int ? Value::integer(wrapNeg(a.i)) : Value::number(-a.d));
                break;
            }
            case Op::EQ:  { Value b = pop(), a = pop(); push(Value::boolean(valuesEqual(a, b))); break; }
            case Op::NEQ: { Value b = pop(), a = pop(); push(Value::boolean(!valuesEqual(a, b))); break; }
            case Op::LT: case Op::LE: case Op::GT: case Op::GE:
                if (!compare(op)) return InterpretResult::RuntimeError;
                break;
            case Op::NOT: { Value a = pop(); push(Value::boolean(!a.isTruthy())); break; }

            case Op::JMP: { int16_t off = readI16(*frame); frame->ip += off; break; }
            case Op::JMP_IF_FALSE: {
                int16_t off = readI16(*frame);
                if (!peek(0).isTruthy()) frame->ip += off;
                break;
            }
            case Op::LOOP: { int16_t off = readI16(*frame); frame->ip -= off; break; }

            case Op::CALL: {
                uint8_t argc = readByte(*frame);
                Value callee = peek(argc);
                if (callee.type == ValueType::Native) {
                    if (!callNative(callee.nativeIndex, argc))
                        return InterpretResult::RuntimeError;
                    break;
                }
                if (callee.type != ValueType::Func)
                    return runtimeError("можна викликати лише функцію"), InterpretResult::RuntimeError;
                if (!callFunction(callee.funcIndex, argc))
                    return InterpretResult::RuntimeError;
                break;  // loop re-fetches frame
            }
            case Op::RET: {
                Value result = pop();
                Frame f = frames_.back();
                frames_.pop_back();
                stack_.resize(f.slotBase - 1);  // drop locals/args + callee slot
                push(std::move(result));
                if (frames_.empty()) return InterpretResult::Ok;
                break;
            }
            case Op::PRINT: { Value v = pop(); out_ << v.toString() << "\n"; break; }

            case Op::BUILD_LIST: {
                uint16_t n = readU16(*frame);
                Value list = Value::newList();
                list.list->reserve(n);
                // Elements were pushed left-to-right; they sit n-deep on stack.
                for (size_t k = stack_.size() - n; k < stack_.size(); ++k)
                    list.list->push_back(stack_[k]);
                stack_.resize(stack_.size() - n);
                push(std::move(list));
                break;
            }
            case Op::BUILD_DICT: {
                uint16_t n = readU16(*frame);   // number of key/value pairs
                Value dict = Value::newDict();
                size_t base = stack_.size() - size_t(n) * 2;
                for (uint16_t k = 0; k < n; ++k) {
                    const Value& key = stack_[base + k * 2];
                    const Value& val = stack_[base + k * 2 + 1];
                    if (key.type != ValueType::Str)
                        return runtimeError("ключ словника має бути рядком"), InterpretResult::RuntimeError;
                    (*dict.dict)[*key.s] = val;
                }
                stack_.resize(base);
                push(std::move(dict));
                break;
            }
            case Op::INDEX_GET: {
                Value index = pop();
                Value cont  = pop();
                if (cont.type == ValueType::List) {
                    if (index.type != ValueType::Int)
                        return runtimeError("індекс списку має бути цілим"), InterpretResult::RuntimeError;
                    if (!cont.list || index.i < 0 || size_t(index.i) >= cont.list->size())
                        return runtimeError("індекс за межами списку"), InterpretResult::RuntimeError;
                    push((*cont.list)[size_t(index.i)]);
                } else if (cont.type == ValueType::Dict) {
                    if (index.type != ValueType::Str)
                        return runtimeError("ключ словника має бути рядком"), InterpretResult::RuntimeError;
                    if (!cont.dict)
                        return runtimeError("немає ключа '" + *index.s + "' у словнику"), InterpretResult::RuntimeError;
                    auto it = cont.dict->find(*index.s);
                    if (it == cont.dict->end())
                        return runtimeError("немає ключа '" + *index.s + "' у словнику"), InterpretResult::RuntimeError;
                    push(it->second);
                } else if (cont.type == ValueType::Str) {
                    if (index.type != ValueType::Int)
                        return runtimeError("індекс рядка має бути цілим"), InterpretResult::RuntimeError;
                    bool ok = false;
                    std::string ch = cont.s ? utf8At(*cont.s, size_t(index.i), ok) : std::string();
                    if (!cont.s || index.i < 0 || !ok)
                        return runtimeError("індекс за межами рядка"), InterpretResult::RuntimeError;
                    push(Value::str(ch));   // one UTF-8 character, not one byte
                } else {
                    return runtimeError("індексувати можна лише список, словник або рядок"), InterpretResult::RuntimeError;
                }
                break;
            }
            case Op::INDEX_SET: {
                Value value = pop();
                Value index = pop();
                Value cont  = pop();
                if (cont.type == ValueType::List) {
                    if (index.type != ValueType::Int)
                        return runtimeError("індекс списку має бути цілим"), InterpretResult::RuntimeError;
                    if (!cont.list || index.i < 0 || size_t(index.i) >= cont.list->size())
                        return runtimeError("індекс за межами списку"), InterpretResult::RuntimeError;
                    (*cont.list)[size_t(index.i)] = value;
                } else if (cont.type == ValueType::Dict) {
                    if (index.type != ValueType::Str)
                        return runtimeError("ключ словника має бути рядком"), InterpretResult::RuntimeError;
                    if (cont.dict->size() >= config_.maxDictSize && !cont.dict->count(*index.s))
                        return runtimeError("словник завеликий"), InterpretResult::RuntimeError;
                    (*cont.dict)[*index.s] = value;   // insert or overwrite
                } else {
                    return runtimeError("присвоювати за індексом можна лише список або словник"), InterpretResult::RuntimeError;
                }
                push(std::move(value));   // assignment expression leaves the value
                break;
            }
            case Op::HALT:  return InterpretResult::Ok;
            default:
                return runtimeError("невідомий опкод"), InterpretResult::RuntimeError;
        }
    }
}

} // namespace vasyan
