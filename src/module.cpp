#include "vasyan/module.h"
#include <cstring>
#include <stdexcept>

namespace vasyan {
namespace {

// --- little-endian write helpers ---
void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v & 0xFF));
    b.push_back(uint8_t((v >> 8) & 0xFF));
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(uint8_t((v >> (8 * k)) & 0xFF));
}
void putI64(std::vector<uint8_t>& b, int64_t v) {
    uint64_t u; std::memcpy(&u, &v, 8);
    for (int k = 0; k < 8; ++k) b.push_back(uint8_t((u >> (8 * k)) & 0xFF));
}
void putF64(std::vector<uint8_t>& b, double v) {
    uint64_t u; std::memcpy(&u, &v, 8);
    for (int k = 0; k < 8; ++k) b.push_back(uint8_t((u >> (8 * k)) & 0xFF));
}
void putStr(std::vector<uint8_t>& b, const std::string& s) {
    if (s.size() > 0xFFFF) throw std::runtime_error("string too long to serialize");
    putU16(b, uint16_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

// --- little-endian read helpers (bounds-checked) ---
struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    explicit Reader(const std::vector<uint8_t>& buf) : b(buf) {}
    void need(size_t n) const {
        if (n > b.size() - pos) throw std::runtime_error("truncated module");
    }
    uint8_t u8()  { need(1); return b[pos++]; }
    uint16_t u16() { need(2); uint16_t v = b[pos] | (uint16_t(b[pos+1]) << 8); pos += 2; return v; }
    uint32_t u32() { need(4); uint32_t v = 0; for (int k=0;k<4;++k) v |= uint32_t(b[pos+k]) << (8*k); pos += 4; return v; }
    int64_t i64() { need(8); uint64_t u = 0; for (int k=0;k<8;++k) u |= uint64_t(b[pos+k]) << (8*k); pos += 8; int64_t v; std::memcpy(&v,&u,8); return v; }
    double f64()  { need(8); uint64_t u = 0; for (int k=0;k<8;++k) u |= uint64_t(b[pos+k]) << (8*k); pos += 8; double v; std::memcpy(&v,&u,8); return v; }
    std::string str() { uint16_t n = u16(); need(n); std::string s(b.begin()+pos, b.begin()+pos+n); pos += n; return s; }
};

// Constant type tags on the wire.
enum ConstTag : uint8_t { C_NIL = 0, C_BOOL = 1, C_INT = 2, C_DOUBLE = 3, C_STR = 4 };

} // namespace

std::vector<uint8_t> Module::serialize() const {
    std::vector<uint8_t> b;
    b.insert(b.end(), kMagic, kMagic + 4);
    putU16(b, kFormatVersion);
    putU16(b, entry);
    if (functions.size() > 0xFFFF) throw std::runtime_error("too many functions");
    putU16(b, uint16_t(functions.size()));

    for (const auto& fn : functions) {
        putStr(b, fn.name);
        putU8(b, fn.arity);

        if (fn.constants.size() > 0xFFFF) throw std::runtime_error("too many constants");
        putU16(b, uint16_t(fn.constants.size()));
        for (const auto& c : fn.constants) {
            switch (c.type) {
                case ValueType::Nil:    putU8(b, C_NIL); break;
                case ValueType::Bool:   putU8(b, C_BOOL); putU8(b, c.b ? 1 : 0); break;
                case ValueType::Int:    putU8(b, C_INT); putI64(b, c.i); break;
                case ValueType::Double: putU8(b, C_DOUBLE); putF64(b, c.d); break;
                case ValueType::Str:    putU8(b, C_STR); putStr(b, c.s ? *c.s : std::string()); break;
                default:
                    // Func/Native/List/Dict are never produced as constants by
                    // the compiler; guard against it explicitly.
                    throw std::runtime_error("non-primitive value is not a storable constant");
            }
        }

        putU32(b, uint32_t(fn.code.size()));
        b.insert(b.end(), fn.code.begin(), fn.code.end());

        // Line table (v2): one source line per code byte. Padded with zeros if
        // the producer left it short.
        for (size_t k = 0; k < fn.code.size(); ++k)
            putU32(b, k < fn.lines.size() ? fn.lines[k] : 0u);
    }
    return b;
}

Module Module::deserialize(const std::vector<uint8_t>& bytes) {
    if (bytes.size() > kMaxModuleBytes) throw std::runtime_error("module too large");
    Reader r(bytes);
    uint8_t m[4] = { r.u8(), r.u8(), r.u8(), r.u8() };
    if (std::memcmp(m, kMagic, 4) != 0) throw std::runtime_error("bad magic (not a VSNB module)");
    uint16_t ver = r.u16();
    if (ver != kFormatVersion) throw std::runtime_error("unsupported module version");

    Module mod;
    mod.entry = r.u16();
    uint16_t fnCount = r.u16();
    mod.functions.reserve(fnCount);

    for (uint16_t fi = 0; fi < fnCount; ++fi) {
        FunctionDef fn;
        fn.name  = r.str();
        fn.arity = r.u8();

        uint16_t cCount = r.u16();
        fn.constants.reserve(cCount);
        for (uint16_t ci = 0; ci < cCount; ++ci) {
            uint8_t tag = r.u8();
            switch (tag) {
                case C_NIL:    fn.constants.push_back(Value::nil()); break;
                case C_BOOL:   fn.constants.push_back(Value::boolean(r.u8() != 0)); break;
                case C_INT:    fn.constants.push_back(Value::integer(r.i64())); break;
                case C_DOUBLE: fn.constants.push_back(Value::number(r.f64())); break;
                case C_STR:    fn.constants.push_back(Value::str(r.str())); break;
                default: throw std::runtime_error("unknown constant tag");
            }
        }

        uint32_t codeLen = r.u32();
        r.need(codeLen);
        fn.code.assign(bytes.begin() + r.pos, bytes.begin() + r.pos + codeLen);
        r.pos += codeLen;

        fn.lines.reserve(codeLen);
        for (uint32_t k = 0; k < codeLen; ++k) fn.lines.push_back(r.u32());

        mod.functions.push_back(std::move(fn));
    }
    if (mod.entry >= mod.functions.size())
        throw std::runtime_error("entry index out of range");
    return mod;
}

} // namespace vasyan
