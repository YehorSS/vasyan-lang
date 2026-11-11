#pragma once
#include <string>

namespace vasyan {

enum class Tok {
    LParen, RParen, Comma,
    LBracket, RBracket, LBrace, RBrace, Colon, DotDot,
    Plus, Minus, Star, Slash, Percent,
    PlusEq, MinusEq, StarEq, SlashEq,
    Equal, EqualEqual, BangEqual,
    Less, LessEqual, Greater, GreaterEqual,
    Ident, Number, String,
    KGo, KReturn, KLet, KFix, KSay, KIf, KElse, KWhile, KEach, KIn, KEnd,
    KTrue, KFalse, KNil, KAnd, KOr, KNot,
    Newline, Eof
};

struct Token {
    Tok         type;
    std::string lexeme;   // for Ident/Number/String and keywords
    int         line;
    int         column;   // 1-based, counted in characters (not bytes)
};

// Raised by the lexer, parser and code generator to abort compilation;
// compile() turns it into a CompileError.
struct SyntaxError {
    std::string message;
    int         line;
    int         column;
};

// `і`, `або` and `у` only mean something between operands, so wherever a name
// is expected they are ordinary identifiers (`хай і = 1`, `кожен і у 0..н`).
inline bool isNameTok(Tok t) {
    return t == Tok::Ident || t == Tok::KAnd || t == Tok::KOr || t == Tok::KIn;
}

} // namespace vasyan
