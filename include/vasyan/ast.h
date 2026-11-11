#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "vasyan/value.h"

// Syntax tree produced by the parser and consumed by the code generator.
// Nodes are plain structs tagged with a kind; the tree owns its children.

namespace vasyan {

struct SourceLoc {
    int line = 0;
    int column = 0;
};

// --- Expressions -----------------------------------------------------------

enum class ExprKind {
    Literal, Variable, Unary, Binary, Logical, Assign,
    Call, Index, IndexAssign, List, Dict
};

struct Expr {
    ExprKind  kind;
    SourceLoc loc;
    Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

// Number, string, ага/неа, пусто.
struct LiteralExpr : Expr {
    Value value;
    LiteralExpr(SourceLoc l, Value v) : Expr(ExprKind::Literal, l), value(std::move(v)) {}
};

struct VariableExpr : Expr {
    std::string name;
    VariableExpr(SourceLoc l, std::string n)
        : Expr(ExprKind::Variable, l), name(std::move(n)) {}
};

enum class UnaryOp { Neg, Not };

struct UnaryExpr : Expr {
    UnaryOp op;
    ExprPtr operand;
    UnaryExpr(SourceLoc l, UnaryOp o, ExprPtr e)
        : Expr(ExprKind::Unary, l), op(o), operand(std::move(e)) {}
};

enum class BinaryOp { Add, Sub, Mul, Div, Mod, Eq, Neq, Lt, Le, Gt, Ge };

struct BinaryExpr : Expr {
    BinaryOp op;
    ExprPtr  lhs, rhs;
    BinaryExpr(SourceLoc l, BinaryOp o, ExprPtr a, ExprPtr b)
        : Expr(ExprKind::Binary, l), op(o), lhs(std::move(a)), rhs(std::move(b)) {}
};

// Short-circuit `і` / `або`.
struct LogicalExpr : Expr {
    bool    isAnd;
    ExprPtr lhs, rhs;
    LogicalExpr(SourceLoc l, bool andOp, ExprPtr a, ExprPtr b)
        : Expr(ExprKind::Logical, l), isAnd(andOp), lhs(std::move(a)), rhs(std::move(b)) {}
};

enum class AssignOp { Set, Add, Sub, Mul, Div };

// `x = v`, `x += v`, ...
struct AssignExpr : Expr {
    std::string name;
    AssignOp    op;
    ExprPtr     value;
    AssignExpr(SourceLoc l, std::string n, AssignOp o, ExprPtr v)
        : Expr(ExprKind::Assign, l), name(std::move(n)), op(o), value(std::move(v)) {}
};

struct CallExpr : Expr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
    CallExpr(SourceLoc l, ExprPtr c, std::vector<ExprPtr> a)
        : Expr(ExprKind::Call, l), callee(std::move(c)), args(std::move(a)) {}
};

struct IndexExpr : Expr {
    ExprPtr object, index;
    IndexExpr(SourceLoc l, ExprPtr o, ExprPtr i)
        : Expr(ExprKind::Index, l), object(std::move(o)), index(std::move(i)) {}
};

// `obj[i] = v`
struct IndexAssignExpr : Expr {
    ExprPtr object, index, value;
    IndexAssignExpr(SourceLoc l, ExprPtr o, ExprPtr i, ExprPtr v)
        : Expr(ExprKind::IndexAssign, l), object(std::move(o)), index(std::move(i)),
          value(std::move(v)) {}
};

struct ListExpr : Expr {
    std::vector<ExprPtr> elements;
    ListExpr(SourceLoc l, std::vector<ExprPtr> e)
        : Expr(ExprKind::List, l), elements(std::move(e)) {}
};

// Keys are strings (a bareword key is taken as its literal text).
struct DictExpr : Expr {
    std::vector<std::pair<std::string, ExprPtr>> entries;
    DictExpr(SourceLoc l, std::vector<std::pair<std::string, ExprPtr>> e)
        : Expr(ExprKind::Dict, l), entries(std::move(e)) {}
};

// --- Statements ------------------------------------------------------------

enum class StmtKind { Let, Say, Return, If, While, ForRange, Expression, Function };

struct Stmt {
    StmtKind  kind;
    SourceLoc loc;
    Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;
using Block   = std::vector<StmtPtr>;

// `хай` (mutable) and `фікс` (immutable).
struct LetStmt : Stmt {
    std::string name;
    bool        immutable;
    ExprPtr     init;
    LetStmt(SourceLoc l, std::string n, bool imm, ExprPtr i)
        : Stmt(StmtKind::Let, l), name(std::move(n)), immutable(imm), init(std::move(i)) {}
};

struct SayStmt : Stmt {
    ExprPtr value;
    SayStmt(SourceLoc l, ExprPtr v) : Stmt(StmtKind::Say, l), value(std::move(v)) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;   // null for a bare `верни`
    ReturnStmt(SourceLoc l, ExprPtr v) : Stmt(StmtKind::Return, l), value(std::move(v)) {}
};

// `якшо ... інакше якшо ... інакше ... кінець`: one arm per condition.
struct IfStmt : Stmt {
    struct Arm {
        ExprPtr cond;
        Block   body;
    };
    std::vector<Arm> arms;
    Block            elseBody;
    explicit IfStmt(SourceLoc l) : Stmt(StmtKind::If, l) {}
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    Block   body;
    WhileStmt(SourceLoc l, ExprPtr c, Block b)
        : Stmt(StmtKind::While, l), cond(std::move(c)), body(std::move(b)) {}
};

// `кожен i у START..END` — half-open range [START, END).
struct ForRangeStmt : Stmt {
    std::string var;
    ExprPtr     start, end;
    Block       body;
    ForRangeStmt(SourceLoc l, std::string v, ExprPtr s, ExprPtr e, Block b)
        : Stmt(StmtKind::ForRange, l), var(std::move(v)), start(std::move(s)),
          end(std::move(e)), body(std::move(b)) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(SourceLoc l, ExprPtr e) : Stmt(StmtKind::Expression, l), expr(std::move(e)) {}
};

// `го NAME(params) ... кінець` — top level only.
struct FunctionStmt : Stmt {
    std::string              name;
    std::vector<std::string> params;
    Block                    body;
    FunctionStmt(SourceLoc l, std::string n, std::vector<std::string> p, Block b)
        : Stmt(StmtKind::Function, l), name(std::move(n)), params(std::move(p)),
          body(std::move(b)) {}
};

// Top-level statements and function declarations, in source order.
struct Program {
    Block statements;
};

} // namespace vasyan
