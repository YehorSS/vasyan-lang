#include "vasyan/value.h"
#include <sstream>

namespace vasyan {

bool Value::isTruthy() const {
    switch (type) {
        case ValueType::Nil:    return false;
        case ValueType::Bool:   return b;
        case ValueType::Int:    return i != 0;
        case ValueType::Double: return d != 0.0;
        case ValueType::Str:    return s && !s->empty();
        case ValueType::Func:   return true;
        case ValueType::Native: return true;
        case ValueType::List:   return list && !list->empty();
        case ValueType::Dict:   return dict && !dict->empty();
    }
    return false;
}

namespace {

// A list can contain itself (`додати(а, а)`), so printing stops descending
// after this many nested containers instead of recursing forever.
constexpr int kMaxPrintDepth = 32;

std::string render(const Value& v, int depth);

// Quote nested strings so the printout is unambiguous.
std::string renderElement(const Value& e, int depth) {
    if (e.type == ValueType::Str) return "\"" + render(e, depth) + "\"";
    return render(e, depth);
}

std::string render(const Value& v, int depth) {
    switch (v.type) {
        case ValueType::List: {
            if (depth >= kMaxPrintDepth) return "[...]";
            std::string out = "[";
            if (v.list) {
                for (size_t k = 0; k < v.list->size(); ++k) {
                    if (k) out += ", ";
                    out += renderElement((*v.list)[k], depth + 1);
                }
            }
            return out + "]";
        }
        case ValueType::Dict: {
            if (depth >= kMaxPrintDepth) return "{...}";
            std::string out = "{";
            if (v.dict) {
                bool first = true;
                for (const auto& kv : *v.dict) {
                    if (!first) out += ", ";
                    first = false;
                    out += "\"" + kv.first + "\": " + renderElement(kv.second, depth + 1);
                }
            }
            return out + "}";
        }
        default:
            return v.toString();
    }
}

} // namespace

std::string Value::toString() const {
    switch (type) {
        case ValueType::Nil:  return "пусто";
        case ValueType::Bool: return b ? "ага" : "неа";
        case ValueType::Int:  return std::to_string(i);
        case ValueType::Double: {
            std::ostringstream os;
            os << d;
            return os.str();
        }
        case ValueType::Str:    return s ? *s : std::string();
        case ValueType::Func:   return "<го #" + std::to_string(funcIndex) + ">";
        case ValueType::Native: return "<вбудована #" + std::to_string(nativeIndex) + ">";
        case ValueType::List:
        case ValueType::Dict:   return render(*this, 0);
    }
    return "?";
}

bool valuesEqual(const Value& a, const Value& b) {
    // Numbers compare across Int/Double; everything else must match type.
    if (a.isNumber() && b.isNumber()) {
        if (a.type == ValueType::Int && b.type == ValueType::Int) return a.i == b.i;
        return a.asDouble() == b.asDouble();
    }
    if (a.type != b.type) return false;
    switch (a.type) {
        case ValueType::Nil:    return true;
        case ValueType::Bool:   return a.b == b.b;
        case ValueType::Str:    return (*a.s) == (*b.s);
        case ValueType::Func:   return a.funcIndex == b.funcIndex;
        case ValueType::Native: return a.nativeIndex == b.nativeIndex;
        // Containers compare by identity (same underlying storage), not by
        // contents — cheap, and avoids surprises with cyclic structures.
        case ValueType::List:   return a.list == b.list;
        case ValueType::Dict:   return a.dict == b.dict;
        default:                return false;
    }
}

} // namespace vasyan
