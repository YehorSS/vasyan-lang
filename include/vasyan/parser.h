#pragma once
#include <vector>
#include "vasyan/ast.h"
#include "vasyan/token.h"

namespace vasyan {

// Build the syntax tree from a token stream produced by tokenize().
// Throws SyntaxError on invalid syntax.
Program parse(const std::vector<Token>& tokens);

} // namespace vasyan
