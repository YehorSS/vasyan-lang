#include "vasyan/parser.h"
#include <cstdlib>

// VasyanLang parser: tokens -> AST.
//
// Blocks open with a keyword line and close with `кінець`.
//
// Grammar (informal):
//   program   := (funcDecl | statement)*
//   funcDecl  := 'го' IDENT '(' params ')' NEWLINE block 'кінець'
//   statement := letStmt | sayStmt | retStmt | ifStmt | whileStmt | eachStmt
//              | assignStmt | exprStmt
//   letStmt   := ('хай' | 'фікс') IDENT '=' expr NEWLINE
//   sayStmt   := 'кажи' expr NEWLINE
//   retStmt   := 'верни' [expr] NEWLINE
//   ifStmt    := 'якшо' expr NEWLINE block ['інакше' NEWLINE block] 'кінець'
//   whileStmt := 'поки' expr NEWLINE block 'кінець'
//   eachStmt  := 'кожен' IDENT 'у' expr '..' expr NEWLINE block 'кінець'

namespace vasyan {
namespace {

enum Prec {
    P_None, P_Assignment, P_Or, P_And, P_Equality, P_Comparison,
    P_Term, P_Factor, P_Unary, P_Call, P_Primary
};

// Nesting limit for expressions and blocks, so hostile or generated source
// cannot overflow the native stack through recursion.
constexpr int kMaxDepth = 256;

SourceLoc locOf(const Token& t) { return {t.line, t.column}; }

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : toks_(tokens) {}

    Program run() {
        Program program;
        for (;;) {
            skipNewlines();
            if (check(Tok::Eof)) break;
            if (check(Tok::KGo)) program.statements.push_back(functionDecl());
            else                 program.statements.push_back(statement());
        }
        return program;
    }

private:
    const std::vector<Token>& toks_;
    size_t                    pos_ = 0;
    int                       depth_ = 0;

    // --- token cursor ---
    // The token stream always ends with a single Eof token; the cursor clamps
    // to it so over-reading past the end keeps returning Eof rather than
    // indexing out of bounds.
    const Token& cur() const {
        return toks_[pos_ < toks_.size() ? pos_ : toks_.size() - 1];
    }
    bool check(Tok t) const { return cur().type == t; }
    bool match(Tok t) { if (check(t)) { advance(); return true; } return false; }
    const Token& advance() {
        const Token& t = cur();
        if (pos_ < toks_.size()) ++pos_;   // may reach size(); cur() clamps reads
        return t;
    }
    [[noreturn]] void fail(const Token& at, const std::string& msg) const {
        throw SyntaxError{msg, at.line, at.column};
    }
    const Token& expect(Tok t, const char* what) {
        if (!check(t)) fail(cur(), std::string("очікував ") + what);
        return advance();
    }
    const Token& expectName(const char* what) {
        if (!isNameTok(cur().type)) fail(cur(), std::string("очікував ") + what);
        return advance();
    }
    void skipNewlines() { while (check(Tok::Newline)) ++pos_; }
    bool atStatementEnd() const {
        return check(Tok::Newline) || check(Tok::Eof) ||
               check(Tok::KEnd) || check(Tok::KElse);
    }
    void endStatement() {
        if (atStatementEnd()) { skipNewlines(); return; }
        fail(cur(), "очікував кінець рядка після інструкції");
    }
    void enter() {
        if (++depth_ > kMaxDepth) fail(cur(), "код надто глибоко вкладений");
    }
    void leave() { --depth_; }

    // --- declarations ---
    StmtPtr functionDecl() {
        SourceLoc loc = locOf(expect(Tok::KGo, "'го'"));
        std::string name = expectName("назву функції").lexeme;

        expect(Tok::LParen, "'(' після назви функції");
        std::vector<std::string> params;
        if (!check(Tok::RParen)) {
            do {
                params.push_back(expectName("назву параметра").lexeme);
            } while (match(Tok::Comma));
        }
        expect(Tok::RParen, "')' після параметрів");
        endStatement();

        Block body = block();
        expect(Tok::KEnd, "'кінець' наприкінці функції");
        endStatement();
        return std::make_unique<FunctionStmt>(loc, std::move(name), std::move(params),
                                              std::move(body));
    }

    // A block runs until `кінець`, `інакше`, or EOF (the caller consumes the
    // terminator).
    Block block() {
        Block stmts;
        for (;;) {
            skipNewlines();
            if (check(Tok::KEnd) || check(Tok::KElse) || check(Tok::Eof)) return stmts;
            stmts.push_back(statement());
        }
    }

    // --- statements ---
    StmtPtr statement() {
        enter();
        StmtPtr s = statementInner();
        leave();
        return s;
    }

    StmtPtr statementInner() {
        SourceLoc loc = locOf(cur());
        if (match(Tok::KLet))    return letStatement(loc, false);
        if (match(Tok::KFix))    return letStatement(loc, true);
        if (match(Tok::KSay))    return sayStatement(loc);
        if (match(Tok::KReturn)) return returnStatement(loc);
        if (match(Tok::KIf))     return ifStatement(loc);
        if (match(Tok::KWhile))  return whileStatement(loc);
        if (match(Tok::KEach))   return forEachStatement(loc);
        if (check(Tok::KGo))
            fail(cur(), "функцію можна оголошувати лише на верхньому рівні");

        // Any other statement is an expression (possibly an assignment, which
        // is handled inside the expression parser). Its value is discarded.
        ExprPtr e = expression();
        endStatement();
        return std::make_unique<ExprStmt>(loc, std::move(e));
    }

    // Shared by `хай` (mutable) and `фікс` (immutable).
    StmtPtr letStatement(SourceLoc loc, bool immutable) {
        std::string name = expectName(immutable ?
            "назву змінної після 'фікс'" : "назву змінної після 'хай'").lexeme;
        expect(Tok::Equal, "'=' у оголошенні змінної");
        ExprPtr init = expression();
        endStatement();
        return std::make_unique<LetStmt>(loc, std::move(name), immutable, std::move(init));
    }

    StmtPtr sayStatement(SourceLoc loc) {
        ExprPtr value = expression();
        endStatement();
        return std::make_unique<SayStmt>(loc, std::move(value));
    }

    StmtPtr returnStatement(SourceLoc loc) {
        ExprPtr value;
        if (!atStatementEnd()) value = expression();
        endStatement();
        return std::make_unique<ReturnStmt>(loc, std::move(value));
    }

    StmtPtr ifStatement(SourceLoc loc) {
        // Supports `якшо ... інакше якшо ... інакше ... кінець` as one block
        // closed by a single `кінець`.
        auto node = std::make_unique<IfStmt>(loc);
        for (;;) {
            IfStmt::Arm arm;
            arm.cond = expression();
            endStatement();                       // newline after condition
            arm.body = block();
            node->arms.push_back(std::move(arm));

            if (match(Tok::KElse)) {
                if (match(Tok::KIf)) continue;    // `інакше якшо` -> next arm
                endStatement();                   // plain `інакше`
                node->elseBody = block();
            }
            break;
        }
        expect(Tok::KEnd, "'кінець' наприкінці 'якшо'");
        endStatement();
        return node;
    }

    StmtPtr forEachStatement(SourceLoc loc) {
        std::string var = expectName("назву змінної після 'кожен'").lexeme;
        expect(Tok::KIn, "'у' після змінної циклу");
        ExprPtr start = parsePrecedence(P_Or);
        expect(Tok::DotDot, "'..' у діапазоні");
        ExprPtr end = parsePrecedence(P_Or);
        endStatement();

        Block body = block();
        expect(Tok::KEnd, "'кінець' наприкінці 'кожен'");
        endStatement();
        return std::make_unique<ForRangeStmt>(loc, std::move(var), std::move(start),
                                              std::move(end), std::move(body));
    }

    StmtPtr whileStatement(SourceLoc loc) {
        ExprPtr cond = expression();
        endStatement();
        Block body = block();
        expect(Tok::KEnd, "'кінець' наприкінці 'поки'");
        endStatement();
        return std::make_unique<WhileStmt>(loc, std::move(cond), std::move(body));
    }

    // --- expressions (Pratt) ---
    ExprPtr expression() { return parsePrecedence(P_Assignment); }

    static bool isAssignOp(Tok t) {
        return t == Tok::Equal || t == Tok::PlusEq || t == Tok::MinusEq ||
               t == Tok::StarEq || t == Tok::SlashEq;
    }

    ExprPtr parsePrecedence(Prec prec) {
        enter();
        bool canAssign = prec <= P_Assignment;
        ExprPtr left = prefix(advance(), canAssign);
        while (prec <= infixPrec(cur().type))
            left = infix(std::move(left), advance(), canAssign);
        if (canAssign && isAssignOp(cur().type))
            fail(cur(), "невірна ціль присвоєння");
        leave();
        return left;
    }

    Prec infixPrec(Tok t) {
        switch (t) {
            case Tok::KOr:            return P_Or;
            case Tok::KAnd:           return P_And;
            case Tok::EqualEqual:
            case Tok::BangEqual:      return P_Equality;
            case Tok::Less: case Tok::LessEqual:
            case Tok::Greater: case Tok::GreaterEqual: return P_Comparison;
            case Tok::Plus: case Tok::Minus: return P_Term;
            case Tok::Star: case Tok::Slash: case Tok::Percent: return P_Factor;
            case Tok::LParen:         return P_Call;
            case Tok::LBracket:       return P_Call;   // indexing
            default:                  return P_None;
        }
    }

    ExprPtr prefix(const Token& t, bool canAssign) {
        SourceLoc loc = locOf(t);
        switch (t.type) {
            case Tok::Number:  return numberLiteral(t);
            case Tok::String:  return std::make_unique<LiteralExpr>(loc, Value::str(t.lexeme));
            case Tok::KTrue:   return std::make_unique<LiteralExpr>(loc, Value::boolean(true));
            case Tok::KFalse:  return std::make_unique<LiteralExpr>(loc, Value::boolean(false));
            case Tok::KNil:    return std::make_unique<LiteralExpr>(loc, Value::nil());
            case Tok::Ident:
            case Tok::KAnd: case Tok::KOr: case Tok::KIn:   // names in operand position
                return namedVariable(t, canAssign);
            case Tok::LBracket: return listLiteral(loc);
            case Tok::LBrace:   return dictLiteral(loc);
            case Tok::LParen: {
                ExprPtr inner = expression();
                expect(Tok::RParen, "')' після виразу");
                return inner;
            }
            case Tok::Minus:
                return std::make_unique<UnaryExpr>(loc, UnaryOp::Neg, parsePrecedence(P_Unary));
            case Tok::KNot:
                return std::make_unique<UnaryExpr>(loc, UnaryOp::Not, parsePrecedence(P_Unary));
            default:
                fail(t, "очікував вираз");
        }
    }

    ExprPtr binary(SourceLoc loc, BinaryOp op, ExprPtr left, Prec rhsPrec) {
        ExprPtr right = parsePrecedence(rhsPrec);
        return std::make_unique<BinaryExpr>(loc, op, std::move(left), std::move(right));
    }

    ExprPtr infix(ExprPtr left, const Token& t, bool canAssign) {
        SourceLoc loc = locOf(t);
        switch (t.type) {
            case Tok::Plus:    return binary(loc, BinaryOp::Add, std::move(left), P_Factor);
            case Tok::Minus:   return binary(loc, BinaryOp::Sub, std::move(left), P_Factor);
            case Tok::Star:    return binary(loc, BinaryOp::Mul, std::move(left), P_Unary);
            case Tok::Slash:   return binary(loc, BinaryOp::Div, std::move(left), P_Unary);
            case Tok::Percent: return binary(loc, BinaryOp::Mod, std::move(left), P_Unary);
            case Tok::EqualEqual:   return binary(loc, BinaryOp::Eq,  std::move(left), P_Comparison);
            case Tok::BangEqual:    return binary(loc, BinaryOp::Neq, std::move(left), P_Comparison);
            case Tok::Less:         return binary(loc, BinaryOp::Lt, std::move(left), P_Term);
            case Tok::LessEqual:    return binary(loc, BinaryOp::Le, std::move(left), P_Term);
            case Tok::Greater:      return binary(loc, BinaryOp::Gt, std::move(left), P_Term);
            case Tok::GreaterEqual: return binary(loc, BinaryOp::Ge, std::move(left), P_Term);
            case Tok::KAnd: {
                ExprPtr right = parsePrecedence(P_And);
                return std::make_unique<LogicalExpr>(loc, true, std::move(left), std::move(right));
            }
            case Tok::KOr: {
                ExprPtr right = parsePrecedence(P_Or);
                return std::make_unique<LogicalExpr>(loc, false, std::move(left), std::move(right));
            }
            case Tok::LParen:   return call(loc, std::move(left));
            case Tok::LBracket: return indexAccess(loc, std::move(left), canAssign);
            default: fail(t, "невірний оператор");
        }
    }

    ExprPtr numberLiteral(const Token& t) {
        if (t.lexeme.find('.') != std::string::npos)
            return std::make_unique<LiteralExpr>(
                locOf(t), Value::number(std::strtod(t.lexeme.c_str(), nullptr)));
        long long v = std::strtoll(t.lexeme.c_str(), nullptr, 10);
        return std::make_unique<LiteralExpr>(locOf(t), Value::integer(static_cast<int64_t>(v)));
    }

    // `[a, b, c]`
    ExprPtr listLiteral(SourceLoc loc) {
        std::vector<ExprPtr> elements;
        if (!check(Tok::RBracket)) {
            do {
                if (check(Tok::RBracket)) break;   // allow trailing comma
                elements.push_back(expression());
            } while (match(Tok::Comma));
        }
        expect(Tok::RBracket, "']' наприкінці списку");
        return std::make_unique<ListExpr>(loc, std::move(elements));
    }

    // `{ключ: значення, "інший": значення}`
    ExprPtr dictLiteral(SourceLoc loc) {
        std::vector<std::pair<std::string, ExprPtr>> entries;
        if (!check(Tok::RBrace)) {
            do {
                if (check(Tok::RBrace)) break;     // allow trailing comma
                if (!check(Tok::String) && !isNameTok(cur().type))
                    fail(cur(), "ключ словника має бути рядком або словом");
                std::string key = advance().lexeme;
                expect(Tok::Colon, "':' між ключем і значенням");
                entries.emplace_back(std::move(key), expression());
            } while (match(Tok::Comma));
        }
        expect(Tok::RBrace, "'}' наприкінці словника");
        return std::make_unique<DictExpr>(loc, std::move(entries));
    }

    // Container is already parsed. `[index]`, optionally an assignment.
    ExprPtr indexAccess(SourceLoc loc, ExprPtr object, bool canAssign) {
        ExprPtr index = expression();
        expect(Tok::RBracket, "']' після індексу");
        if (canAssign && match(Tok::Equal)) {
            ExprPtr value = expression();
            return std::make_unique<IndexAssignExpr>(loc, std::move(object), std::move(index),
                                                     std::move(value));
        }
        if (canAssign && isAssignOp(cur().type))
            fail(cur(), "складене присвоєння (+= тощо) не підтримується для індексу");
        return std::make_unique<IndexExpr>(loc, std::move(object), std::move(index));
    }

    // Variable read / assignment / compound assignment.
    ExprPtr namedVariable(const Token& t, bool canAssign) {
        SourceLoc loc = locOf(t);
        if (canAssign && check(Tok::Equal)) {
            advance();                   // '='
            return std::make_unique<AssignExpr>(loc, t.lexeme, AssignOp::Set, expression());
        }
        if (canAssign && isAssignOp(cur().type)) {
            AssignOp op = AssignOp::Add;
            switch (advance().type) {    // += -= *= /=
                case Tok::PlusEq:  op = AssignOp::Add; break;
                case Tok::MinusEq: op = AssignOp::Sub; break;
                case Tok::StarEq:  op = AssignOp::Mul; break;
                default:           op = AssignOp::Div; break;
            }
            // right-hand side (no chained assignment)
            return std::make_unique<AssignExpr>(loc, t.lexeme, op, parsePrecedence(P_Or));
        }
        return std::make_unique<VariableExpr>(loc, t.lexeme);
    }

    ExprPtr call(SourceLoc loc, ExprPtr callee) {
        std::vector<ExprPtr> args;
        if (!check(Tok::RParen)) {
            do {
                args.push_back(expression());
            } while (match(Tok::Comma));
        }
        expect(Tok::RParen, "')' після аргументів");
        return std::make_unique<CallExpr>(loc, std::move(callee), std::move(args));
    }
};

} // namespace

Program parse(const std::vector<Token>& tokens) {
    if (tokens.empty()) return Program{};
    return Parser(tokens).run();
}

} // namespace vasyan
