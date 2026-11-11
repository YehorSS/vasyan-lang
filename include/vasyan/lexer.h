#pragma once
#include <string>
#include <vector>
#include "vasyan/token.h"

namespace vasyan {

// Split UTF-8 source text into tokens. The result always ends with a single
// Eof token. Throws SyntaxError on malformed input.
std::vector<Token> tokenize(const std::string& source);

} // namespace vasyan
