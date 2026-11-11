#include "vasyan/lexer.h"
#include <unordered_map>

// VasyanLang lexer: source text -> tokens.
//
// The surface language deliberately avoids C-isms: no braces, no semicolons.
// Statements are newline-terminated, so Newline is a real token. Keywords are
// Ukrainian/slang to match the VM's runtime wording (ага/неа/пусто).
// Identifiers may contain Cyrillic (any byte >= 0x80 is treated as an
// identifier character), so `сума`, `лічильник` etc. work.

namespace vasyan {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Identifier start: ASCII letter, underscore, or any UTF-8 continuation/lead
// byte (>= 0x80). This admits Cyrillic identifiers without a full Unicode
// tables dependency.
bool isIdentStart(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u >= 0x80;
}
bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c); }

const std::unordered_map<std::string, Tok>& keywords() {
    static const std::unordered_map<std::string, Tok> kw = {
        {"го", Tok::KGo}, {"верни", Tok::KReturn}, {"хай", Tok::KLet},
        {"фікс", Tok::KFix}, {"кажи", Tok::KSay}, {"якшо", Tok::KIf},
        {"інакше", Tok::KElse}, {"поки", Tok::KWhile}, {"кожен", Tok::KEach},
        {"у", Tok::KIn}, {"кінець", Tok::KEnd},
        {"ага", Tok::KTrue}, {"неа", Tok::KFalse}, {"пусто", Tok::KNil},
        {"і", Tok::KAnd}, {"або", Tok::KOr}, {"не", Tok::KNot},
    };
    return kw;
}

class Lexer {
public:
    explicit Lexer(const std::string& src) : s_(src) {}

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        for (;;) {
            skipInlineSpace();
            tokLine_ = line_;
            tokCol_  = columnAt(at_);
            if (at_ >= s_.size()) { out.push_back(make(Tok::Eof)); break; }

            char c = s_[at_];
            if (c == '\n') { out.push_back(make(Tok::Newline)); ++at_; newLine(); continue; }

            if (isDigit(c)) { out.push_back(number()); continue; }
            if (c == '"')   { out.push_back(string()); continue; }
            if (isIdentStart(c)) { out.push_back(identifier()); continue; }

            out.push_back(symbol());
        }
        return out;
    }

private:
    const std::string& s_;
    size_t at_ = 0;
    int    line_ = 1;
    size_t lineStart_ = 0;   // byte offset where the current line begins
    int    tokLine_ = 1;     // position of the token being scanned
    int    tokCol_  = 1;

    Token make(Tok t, std::string lexeme = "") const {
        return {t, std::move(lexeme), tokLine_, tokCol_};
    }
    [[noreturn]] void fail(const std::string& msg) const {
        throw SyntaxError{msg, tokLine_, tokCol_};
    }

    // Call right after consuming a '\n'.
    void newLine() { ++line_; lineStart_ = at_; }

    // 1-based column of byte offset `pos`, counting UTF-8 characters.
    int columnAt(size_t pos) const {
        int col = 1;
        for (size_t i = lineStart_; i < pos && i < s_.size(); ++i)
            if ((static_cast<unsigned char>(s_[i]) & 0xC0) != 0x80) ++col;
        return col;
    }

    // Spaces, tabs, CR, `--` line comments, and `--[ ... ]--` block comments
    // (which may span newlines) — but plain newlines stay significant.
    void skipInlineSpace() {
        for (;;) {
            if (at_ >= s_.size()) return;
            char c = s_[at_];
            if (c == ' ' || c == '\t' || c == '\r') { ++at_; continue; }
            if (c == '-' && at_ + 1 < s_.size() && s_[at_ + 1] == '-') {
                if (at_ + 2 < s_.size() && s_[at_ + 2] == '[') {
                    at_ += 3;   // block comment: consume until ]--
                    while (at_ < s_.size()) {
                        if (s_[at_] == ']' && at_ + 2 < s_.size() &&
                            s_[at_ + 1] == '-' && s_[at_ + 2] == '-') {
                            at_ += 3;
                            break;
                        }
                        if (s_[at_++] == '\n') newLine();
                    }
                    continue;
                }
                at_ += 2;   // line comment
                while (at_ < s_.size() && s_[at_] != '\n') ++at_;
                continue;
            }
            return;
        }
    }

    Token number() {
        size_t start = at_;
        while (at_ < s_.size() && isDigit(s_[at_])) ++at_;
        if (at_ < s_.size() && s_[at_] == '.' &&
            at_ + 1 < s_.size() && isDigit(s_[at_ + 1])) {
            ++at_;  // consume '.'
            while (at_ < s_.size() && isDigit(s_[at_])) ++at_;
        }
        return make(Tok::Number, s_.substr(start, at_ - start));
    }

    Token string() {
        ++at_;  // opening quote
        std::string value;
        while (at_ < s_.size() && s_[at_] != '"') {
            char c = s_[at_++];
            if (c == '\n') { newLine(); value.push_back(c); continue; }
            if (c == '\\' && at_ < s_.size()) {
                char e = s_[at_++];
                switch (e) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case 'r': value.push_back('\r'); break;
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    default: value.push_back(e); break;
                }
                continue;
            }
            value.push_back(c);
        }
        if (at_ >= s_.size()) fail("незакритий рядок (немає \")");
        ++at_;  // closing quote
        return make(Tok::String, std::move(value));
    }

    Token identifier() {
        size_t start = at_;
        while (at_ < s_.size() && isIdentCont(s_[at_])) ++at_;
        std::string text = s_.substr(start, at_ - start);
        auto it = keywords().find(text);
        if (it != keywords().end()) return make(it->second, std::move(text));
        return make(Tok::Ident, std::move(text));
    }

    Token symbol() {
        char c = s_[at_++];
        auto two = [&](char next, Tok ifTwo, Tok ifOne) -> Token {
            if (at_ < s_.size() && s_[at_] == next) { ++at_; return make(ifTwo); }
            return make(ifOne);
        };
        switch (c) {
            case '(': return make(Tok::LParen);
            case ')': return make(Tok::RParen);
            case '[': return make(Tok::LBracket);
            case ']': return make(Tok::RBracket);
            case '{': return make(Tok::LBrace);
            case '}': return make(Tok::RBrace);
            case ':': return make(Tok::Colon);
            case ',': return make(Tok::Comma);
            case '+': return two('=', Tok::PlusEq, Tok::Plus);
            case '-': return two('=', Tok::MinusEq, Tok::Minus);
            case '*': return two('=', Tok::StarEq, Tok::Star);
            case '/': return two('=', Tok::SlashEq, Tok::Slash);
            case '%': return make(Tok::Percent);
            case '.':
                if (at_ < s_.size() && s_[at_] == '.') { ++at_; return make(Tok::DotDot); }
                fail("несподівана крапка '.'");
            case '=': return two('=', Tok::EqualEqual, Tok::Equal);
            case '<': return two('=', Tok::LessEqual, Tok::Less);
            case '>': return two('=', Tok::GreaterEqual, Tok::Greater);
            case '!':
                if (at_ < s_.size() && s_[at_] == '=') { ++at_; return make(Tok::BangEqual); }
                fail("несподіваний символ '!' (можливо, ви мали на увазі '!=')");
            default:
                fail(std::string("невідомий символ '") + c + "'");
        }
    }
};

} // namespace

std::vector<Token> tokenize(const std::string& source) {
    return Lexer(source).tokenize();
}

} // namespace vasyan
