#pragma once
#include <string>
#include "vasyan/module.h"

namespace vasyan {

struct CompileError {
    bool        ok = true;
    std::string message;
    int         line = 0;
    int         column = 0;   // 1-based, in characters; 0 if unknown
};

// Compile VasyanLang source text into a Module.
// On failure, err.ok is false and err.message/line describe the problem;
// the returned Module is then meaningless.
Module compile(const std::string& source, CompileError& err);

} // namespace vasyan
