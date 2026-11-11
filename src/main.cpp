#include "vasyan/compiler.h"
#include "vasyan/module.h"
#include "vasyan/opcodes.h"
#include "vasyan/validator.h"
#include "vasyan/vm.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vasyan;

namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("не можу відкрити файл: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("не можу записати файл: " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("не можу відкрити файл: " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

Module compileOrThrow(const std::string& source) {
    CompileError err;
    Module m = compile(source, err);
    if (!err.ok)
        throw std::runtime_error("помилка компіляції (рядок " +
                                 std::to_string(err.line) + ", стовпчик " +
                                 std::to_string(err.column) + "): " + err.message);
    return m;
}

// Compile and run a source file.
void cmdExec(const std::string& src) {
    Module m = compileOrThrow(readFile(src));
    VM vm(std::cout);
    if (vm.run(m) != InterpretResult::Ok)
        throw std::runtime_error("рантайм: " + vm.lastError());
}

int cmdSelfTest() {
    int failures = 0;
    auto check = [&](const char* name, bool ok) {
        std::cout << (ok ? "  ok   " : "  FAIL ") << name << "\n";
        if (!ok) ++failures;
    };

    // 5) new language features: lists, dicts, for-range, else-if, natives
    {
        const char* prog =
            "хай нат = [10, 20, 30]\n"
            "додати(нат, 40)\n"
            "хай сума = 0\n"
            "кожен i у 0..довжина(нат)\n"
            "    сума += нат[i]\n"
            "кінець\n"
            "кажи сума\n"
            "хай оцінки = {\"матем\": 5, \"фізика\": 4}\n"
            "оцінки[\"хімія\"] = 3\n"
            "кажи довжина(оцінки)\n"
            "хай x = 7\n"
            "якшо x < 5\n"
            "    кажи \"мало\"\n"
            "інакше якшо x < 10\n"
            "    кажи \"норм\"\n"
            "інакше\n"
            "    кажи \"багато\"\n"
            "кінець\n";
        CompileError ce;
        Module m = compile(prog, ce);
        check("features: compiles", ce.ok);
        if (ce.ok) {
            std::ostringstream out;
            VM vm(out);
            bool ran = vm.run(m) == InterpretResult::Ok;
            check("features: runs", ran);
            check("features: output (100 / 3 / норм)", out.str() == "100\n3\nнорм\n");
        }
    }

    // 6) runtime error carries a source line number
    {
        Module m = compileOrThrow("хай a = 1\nхай b = a / 0\n");
        std::ostringstream out;
        VM vm(out);
        bool failed = vm.run(m) != InterpretResult::Ok;
        bool hasLine = vm.lastError().find("рядок 2") != std::string::npos;
        check("errors: division by zero reported", failed);
        check("errors: error names line 2", hasLine);
    }

    // 7) UTF-8: length and indexing count characters, not bytes
    {
        Module m = compileOrThrow(
            "хай імя = \"Вася\"\nкажи довжина(імя)\nкажи імя[0]\nкажи імя[3]\n");
        std::ostringstream out;
        VM vm(out);
        bool ran = vm.run(m) == InterpretResult::Ok;
        check("utf8: runs", ran);
        check("utf8: length in chars, index in chars", out.str() == "4\nВ\nя\n");
    }

    // 8) `і`/`або`/`у` are names where a name is expected, operators between operands
    {
        const char* prog =
            "хай і = 1\n"
            "хай сума = 0\n"
            "поки і <= 3\n"
            "    сума += і\n"
            "    і = і + 1\n"
            "кінець\n"
            "кажи сума\n"
            "кожен у у 0..2\n"
            "    кажи у\n"
            "кінець\n"
            "кажи ага і неа\n"
            "кажи неа або і == 4\n";
        CompileError ce;
        Module m = compile(prog, ce);
        check("keywords: `і` usable as a name", ce.ok);
        if (ce.ok) {
            std::ostringstream out;
            VM vm(out);
            bool ran = vm.run(m) == InterpretResult::Ok;
            check("keywords: runs", ran);
            check("keywords: output (6 / 0 / 1 / неа / ага)",
                  out.str() == "6\n0\n1\nнеа\nага\n");
        }
    }

    // 9) validator: compiler output passes, malformed bytecode is rejected
    //    statically and the VM refuses to run it (instead of crashing)
    {
        Module good = compileOrThrow(
            "го ф(н)\n    якшо н < 2 або н == 5\n        верни н\n    кінець\n"
            "    верни ф(н - 1) + ф(н - 2)\nкінець\n"
            "хай с = [1, 2]\nкожен к у 0..3\n    додати(с, ф(к))\nкінець\nкажи с\n");
        check("validator: compiler output accepted", validateModule(good).ok);

        auto op = [](Op o) { return static_cast<uint8_t>(o); };
        auto rejected = [&](std::vector<uint8_t> code, bool withConstant = false) {
            Module m;
            m.functions.resize(1);
            m.functions[0].name = "<script>";
            m.functions[0].code = std::move(code);
            if (withConstant) m.functions[0].constants.push_back(Value::integer(7));
            std::ostringstream out;
            VM vm(out);
            return !validateModule(m).ok && vm.run(m) == InterpretResult::RuntimeError;
        };
        check("validator: unknown opcode", rejected({0xFF}));
        check("validator: truncated operand", rejected({op(Op::CONST), 0x00}));
        check("validator: constant index out of range",
              rejected({op(Op::CONST), 5, 0, op(Op::RET)}, true));
        check("validator: stack underflow",
              rejected({op(Op::POP), op(Op::NIL), op(Op::RET)}));
        check("validator: jump into the middle of an instruction",
              rejected({op(Op::JMP), 1, 0, op(Op::CONST), 0, 0, op(Op::RET)}, true));
        check("validator: running off the end", rejected({op(Op::NIL)}));
        check("validator: local slot outside the frame",
              rejected({op(Op::GET_LOCAL), 3, op(Op::RET)}));
        check("validator: inconsistent stack depth",
              rejected({op(Op::TRUE), op(Op::JMP_IF_FALSE), 1, 0, op(Op::NIL), op(Op::RET)}));
    }

    // 10) resource limits stop a runaway program
    {
        Module loop = compileOrThrow("поки ага\nкінець\n");
        std::ostringstream out;
        VM vm(out);
        VMConfig cfg;
        cfg.maxInstructions = 10000;
        vm.setConfig(cfg);
        bool stopped = vm.run(loop) == InterpretResult::RuntimeError;
        check("limits: instruction limit stops an endless loop",
              stopped && vm.lastError().find("ліміт інструкцій") != std::string::npos);

        Module deep = compileOrThrow("го ф(н)\n    верни ф(н + 1)\nкінець\nф(0)\n");
        VM vm2(out);
        check("limits: runaway recursion is an error",
              vm2.run(deep) == InterpretResult::RuntimeError);
    }

    // 11) inputs that used to crash the host
    {
        Module m = compileOrThrow(
            "хай а = [1]\nдодати(а, а)\nкажи довжина(текст(а)) > 0\n"
            "хай м = 0 - 9223372036854775807 - 1\n"
            "кажи м / (0 - 1)\nкажи м % (0 - 1)\n");
        std::ostringstream out;
        VM vm(out);
        bool ran = vm.run(m) == InterpretResult::Ok;
        check("robust: self-containing list and INT64_MIN / -1",
              ran && out.str() == "ага\n-9223372036854775808\n0\n");
    }

    std::cout << (failures == 0 ? "\nусі тести пройдено\n"
                                : "\nпровалено тестів: " + std::to_string(failures) + "\n");
    return failures == 0 ? 0 : 1;
}

void usage() {
    std::cout <<
        "vasyan — мова програмування з українським синтаксисом\n"
        "  vasyan exec <file.vy>\n"
        "      компілювати і виконати\n"
        "  vasyan selftest\n"
        "      самотести\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) { usage(); return 1; }
        std::string cmd = argv[1];

        if (cmd == "selftest") return cmdSelfTest();
        if (cmd == "exec" && argc == 3) { cmdExec(argv[2]); return 0; }
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "помилка: " << e.what() << "\n";
        return 1;
    }
}
