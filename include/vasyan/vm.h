#pragma once
#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>
#include "vasyan/module.h"
#include "vasyan/opcodes.h"   // Op — used by binaryNumeric()/compare() below
#include "vasyan/value.h"

namespace vasyan {

class VM;

// A built-in function implemented in C++. Returns true on success (with the
// result in `result`); on failure returns false and fills `err`.
using NativeFn = std::function<bool(VM& vm, std::vector<Value>& args,
                                    Value& result, std::string& err)>;

enum class InterpretResult { Ok, CompileError, RuntimeError };

// Resource limits for one run(). Exceeding any of them stops the program with
// a runtime error instead of exhausting the host.
struct VMConfig {
    size_t   maxStackSize    = 1'000'000;          // values on the operand stack
    size_t   maxCallDepth    = 1024;               // nested calls
    size_t   maxListSize     = 10'000'000;         // elements per list
    size_t   maxDictSize     = 10'000'000;         // keys per dict
    size_t   maxStringSize   = 64 * 1024 * 1024;   // bytes per string
    uint64_t maxInstructions = 0;                  // 0 = unlimited
    uint64_t maxExecutionMs  = 0;                  // 0 = unlimited
    bool     allowInput      = true;               // provide `ввід` (reads the input stream)
};

// The VasyanLang virtual machine: a stack-based bytecode interpreter.
class VM {
public:
    explicit VM(std::ostream& out);
    VM(std::ostream& out, std::istream& in);

    std::istream& input();   // stream used by the `ввід` native

    void setConfig(const VMConfig& config) { config_ = config; }
    const VMConfig& config() const { return config_; }

    // Host functions: the only way a script can reach outside the VM. A script
    // sees exactly the built-ins plus what the host adds here — bytecode has no
    // instruction that calls into the process any other way. Add before run().
    void addNative(const std::string& name, NativeFn fn) {
        hostNatives_.emplace_back(name, std::move(fn));
    }

    // The module is untrusted: each function is run through the bytecode
    // validator (validator.h) before its first instruction executes, and a
    // function that fails is never run.
    InterpretResult run(Module& module);

    const std::string& lastError() const { return error_; }

private:
    struct Frame {
        const FunctionDef* fn;
        size_t             ip;         // instruction pointer within fn->code
        size_t             slotBase;   // index in stack_ of this frame's locals
    };

    Module*                          module_ = nullptr;
    std::vector<Value>               stack_;
    std::vector<Frame>               frames_;
    std::unordered_map<std::string, Value> globals_;
    std::vector<NativeFn>            natives_;   // built-in + host functions
    std::vector<std::pair<std::string, NativeFn>> hostNatives_;   // from addNative()
    std::ostream&                    out_;
    std::istream*                    in_;
    std::string                      error_;
    VMConfig                         config_;

    // Per-function validation state, filled lazily on first call.
    struct Checked {
        bool     done = false;
        uint32_t maxStack = 0;
    };
    std::vector<Checked>             checked_;

    InterpretResult execute();       // the interpreter loop

    // Byte/operand readers advance the top frame's ip.
    uint8_t  readByte(Frame& f);
    uint16_t readU16(Frame& f);
    int16_t  readI16(Frame& f);

    void push(Value v) { stack_.push_back(std::move(v)); }
    Value pop() { Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
    Value& peek(int distance = 0) { return stack_[stack_.size() - 1 - distance]; }

    void registerNatives();          // populate globals_ with built-ins
    bool callNative(int nativeIndex, uint8_t argc);

    bool callFunction(int funcIndex, uint8_t argc);
    bool runtimeError(const std::string& msg);
    bool binaryNumeric(Op op);
    bool compare(Op op);
};

} // namespace vasyan
