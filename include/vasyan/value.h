#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vasyan {

// The set of runtime types VasyanLang understands: primitives, a first-class
// function reference (by index into the module), a native (built-in) function,
// and two heap containers — List and Dict — held by shared_ptr so assignment
// and passing share the same underlying storage (reference semantics).
enum class ValueType : uint8_t {
    Nil, Bool, Int, Double, Str, Func, Native, List, Dict
};

struct Value {
    ValueType type = ValueType::Nil;
    int64_t     i  = 0;      // Int
    double      d  = 0.0;    // Double
    bool        b  = false;  // Bool
    std::shared_ptr<std::string> s;   // Str (shared so copies are cheap)
    int         funcIndex   = -1;     // Func: index into Module::functions
    int         nativeIndex = -1;     // Native: index into VM's native table

    std::shared_ptr<std::vector<Value>>                     list;  // List
    std::shared_ptr<std::unordered_map<std::string, Value>> dict;  // Dict

    Value() = default;

    static Value nil()                 { return Value{}; }
    static Value boolean(bool v)       { Value x; x.type = ValueType::Bool;   x.b = v; return x; }
    static Value integer(int64_t v)    { Value x; x.type = ValueType::Int;    x.i = v; return x; }
    static Value number(double v)      { Value x; x.type = ValueType::Double; x.d = v; return x; }
    static Value str(std::string v)    { Value x; x.type = ValueType::Str;    x.s = std::make_shared<std::string>(std::move(v)); return x; }
    static Value function(int idx)     { Value x; x.type = ValueType::Func;   x.funcIndex = idx; return x; }
    static Value nativeFn(int idx)     { Value x; x.type = ValueType::Native; x.nativeIndex = idx; return x; }
    static Value newList()             { Value x; x.type = ValueType::List; x.list = std::make_shared<std::vector<Value>>(); return x; }
    static Value newDict()             { Value x; x.type = ValueType::Dict; x.dict = std::make_shared<std::unordered_map<std::string, Value>>(); return x; }

    bool isTruthy() const;
    // Numeric helper: true for Int or Double.
    bool isNumber() const { return type == ValueType::Int || type == ValueType::Double; }
    double asDouble() const { return type == ValueType::Int ? static_cast<double>(i) : d; }

    std::string toString() const;
};

bool valuesEqual(const Value& a, const Value& b);

} // namespace vasyan
