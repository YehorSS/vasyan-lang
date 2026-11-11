#include "vasyan/compiler.h"
#include "vasyan/ast.h"
#include "vasyan/lexer.h"
#include "vasyan/opcodes.h"
#include "vasyan/parser.h"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// VasyanLang code generator: AST -> Module (bytecode).
//
// compile() runs the whole front end: tokenize (lexer.cpp) -> parse
// (parser.cpp) -> generate. The generator walks the tree once and emits
// directly into the VM's instruction set.

namespace vasyan {
namespace {

struct Local {
    std::string name;
    int         depth;       // -1 = declared but not yet initialized
    bool        immutable;   // declared with `фікс`
};

// Per-function emission context.
struct FuncCtx {
    FunctionDef*        fn;
    std::vector<Local>  locals;
    int                 scopeDepth = 0;
};

template <typename T, typename Base>
const T& as(const Base& node) { return static_cast<const T&>(node); }

class CodeGen {
public:
    CodeGen(const Program& program, Module& module)
        : program_(program), module_(module) {}

    void run() {
        prescanFunctions();

        // functions[0] is the implicit script/entry.
        module_.functions.clear();
        module_.functions.resize(1 + funcOrder_.size());
        module_.functions[0].name  = "<script>";
        module_.functions[0].arity = 0;
        module_.entry = 0;
        for (size_t i = 0; i < funcOrder_.size(); ++i)
            module_.functions[funcIndex_[funcOrder_[i]]].name = funcOrder_[i];

        FuncCtx script;
        script.fn = &module_.functions[0];
        cur_ = &script;

        for (const StmtPtr& s : program_.statements) {
            if (s->kind == StmtKind::Function) {
                functionDecl(as<FunctionStmt>(*s));
                cur_ = &script;
            } else {
                statement(*s);
            }
        }

        // Script falls through to an implicit `верни пусто`.
        emit(Op::NIL);
        emit(Op::RET);
    }

private:
    const Program&            program_;
    Module&                   module_;
    FuncCtx*                  cur_ = nullptr;
    SourceLoc                 loc_{1, 1};     // node being generated

    std::unordered_map<std::string, int> funcIndex_;  // name -> module index
    std::vector<std::string>             funcOrder_;   // declaration order
    std::unordered_map<std::string, bool> immutableGlobals_;  // name -> is `фікс`
    int                       forTemp_ = 0;   // counter for hidden loop temporaries

    [[noreturn]] void fail(const std::string& msg) const {
        throw SyntaxError{msg, loc_.line, loc_.column};
    }
    void at(const Expr& e) { loc_ = e.loc; }
    void at(const Stmt& s) { loc_ = s.loc; }

    // --- pre-scan: assign a module index to every function declaration, so
    // calls can reference functions declared later in the file ---
    void prescanFunctions() {
        for (const StmtPtr& s : program_.statements) {
            if (s->kind != StmtKind::Function) continue;
            const std::string& name = as<FunctionStmt>(*s).name;
            at(*s);
            if (funcIndex_.count(name)) fail("функцію '" + name + "' оголошено двічі");
            if (funcOrder_.size() >= 0xFFFE) fail("забагато функцій у модулі");
            int index = 1 + static_cast<int>(funcOrder_.size());
            funcIndex_[name] = index;
            funcOrder_.push_back(name);
        }
    }

    // --- emit helpers (into current function) ---
    std::vector<uint8_t>& code() { return cur_->fn->code; }
    // Every emitted byte records the current source line in a parallel table,
    // so runtime errors can report where they happened.
    void pushByte(uint8_t b) {
        code().push_back(b);
        cur_->fn->lines.push_back(uint32_t(loc_.line));
    }
    void emit(Op op)     { pushByte(static_cast<uint8_t>(op)); }
    void emit(uint8_t b) { pushByte(b); }
    void emitU16(uint16_t v) {
        pushByte(uint8_t(v & 0xFF));
        pushByte(uint8_t((v >> 8) & 0xFF));
    }

    int addConstant(const Value& v) {
        auto& consts = cur_->fn->constants;
        for (size_t i = 0; i < consts.size(); ++i)
            if (valuesEqual(consts[i], v) && consts[i].type == v.type)
                return static_cast<int>(i);
        if (consts.size() >= 0xFFFF) fail("забагато констант у функції");
        consts.push_back(v);
        return static_cast<int>(consts.size() - 1);
    }
    void emitConst(const Value& v) {
        emit(Op::CONST);
        emitU16(static_cast<uint16_t>(addConstant(v)));
    }

    // Forward jump: emit placeholder, return operand offset to patch later.
    size_t emitJump(Op op) {
        emit(op);
        size_t operand = code().size();
        emitU16(0);
        return operand;
    }
    void patchJump(size_t operand) {
        // VM: after reading the i16 operand, ip = operand+2; ip += jump.
        long jump = static_cast<long>(code().size()) - static_cast<long>(operand + 2);
        if (jump > INT16_MAX || jump < INT16_MIN)
            fail("стрибок надто далеко (тіло блоку завелике)");
        code()[operand]     = uint8_t(jump & 0xFF);
        code()[operand + 1] = uint8_t((jump >> 8) & 0xFF);
    }
    void emitLoop(size_t loopStart) {
        emit(Op::LOOP);
        long off = static_cast<long>(code().size()) - static_cast<long>(loopStart) + 2;
        if (off > INT16_MAX) fail("тіло циклу завелике");
        emitU16(static_cast<uint16_t>(off));
    }

    // --- scopes / locals ---
    void beginScope() { ++cur_->scopeDepth; }
    void endScope() {
        --cur_->scopeDepth;
        while (!cur_->locals.empty() &&
               cur_->locals.back().depth > cur_->scopeDepth) {
            emit(Op::POP);
            cur_->locals.pop_back();
        }
    }
    void addLocal(const std::string& name, bool immutable = false) {
        for (auto it = cur_->locals.rbegin(); it != cur_->locals.rend(); ++it) {
            if (it->depth != -1 && it->depth < cur_->scopeDepth) break;
            if (it->name == name)
                fail("змінна '" + name + "' вже є в цій області");
        }
        if (cur_->locals.size() >= 256)
            fail("забагато локальних змінних у функції (максимум 256)");
        cur_->locals.push_back({name, -1, immutable});
    }
    void markInitialized() { cur_->locals.back().depth = cur_->scopeDepth; }
    int resolveLocal(const std::string& name) {
        for (int i = static_cast<int>(cur_->locals.size()) - 1; i >= 0; --i) {
            if (cur_->locals[i].name == name) {
                if (cur_->locals[i].depth == -1)
                    fail("не можна читати '" + name + "' у власному ініціалізаторі");
                return i;
            }
        }
        return -1;
    }

    // --- declarations ---
    void functionDecl(const FunctionStmt& f) {
        at(f);
        FuncCtx ctx;
        ctx.fn = &module_.functions[funcIndex_.at(f.name)];
        cur_ = &ctx;
        beginScope();

        if (f.params.size() > 255) fail("забагато параметрів (максимум 255)");
        for (const std::string& p : f.params) {
            addLocal(p);
            markInitialized();
        }
        ctx.fn->arity = static_cast<uint8_t>(f.params.size());

        block(f.body);

        // Implicit `верни пусто` on fall-through.
        emit(Op::NIL);
        emit(Op::RET);
    }

    void block(const Block& stmts) {
        for (const StmtPtr& s : stmts) statement(*s);
    }

    // --- statements ---
    void statement(const Stmt& s) {
        at(s);
        switch (s.kind) {
            case StmtKind::Let:      letStatement(as<LetStmt>(s)); break;
            case StmtKind::Say:
                expression(*as<SayStmt>(s).value);
                emit(Op::PRINT);   // pops
                break;
            case StmtKind::Return: {
                const ReturnStmt& r = as<ReturnStmt>(s);
                if (r.value) expression(*r.value);
                else         emit(Op::NIL);
                emit(Op::RET);
                break;
            }
            case StmtKind::If:       ifStatement(as<IfStmt>(s)); break;
            case StmtKind::While:    whileStatement(as<WhileStmt>(s)); break;
            case StmtKind::ForRange: forEachStatement(as<ForRangeStmt>(s)); break;
            case StmtKind::Expression:
                // The value of an expression statement is discarded.
                expression(*as<ExprStmt>(s).expr);
                emit(Op::POP);
                break;
            case StmtKind::Function:
                fail("функцію можна оголошувати лише на верхньому рівні");
        }
    }

    // Shared by `хай` (mutable) and `фікс` (immutable).
    void letStatement(const LetStmt& s) {
        bool global = (cur_->fn == &module_.functions[0]) && cur_->scopeDepth == 0;

        if (global) {
            expression(*s.init);
            at(s);
            emit(Op::DEF_GLOBAL);
            emitU16(static_cast<uint16_t>(addConstant(Value::str(s.name))));
            immutableGlobals_[s.name] = s.immutable;
        } else {
            addLocal(s.name, s.immutable);   // reserve slot (uninitialized)
            expression(*s.init);             // value lands in the slot
            markInitialized();
        }
    }

    void ifStatement(const IfStmt& s) {
        // Each arm's JMP over the rest of the chain is patched at the end.
        std::vector<size_t> endJumps;
        for (const IfStmt::Arm& arm : s.arms) {
            expression(*arm.cond);
            size_t thenJump = emitJump(Op::JMP_IF_FALSE);
            emit(Op::POP);                        // pop condition (this arm)
            beginScope();
            block(arm.body);
            endScope();
            endJumps.push_back(emitJump(Op::JMP)); // jump past the whole chain
            patchJump(thenJump);
            emit(Op::POP);                        // pop condition (skip this arm)
        }
        beginScope();
        block(s.elseBody);
        endScope();
        for (size_t j : endJumps) patchJump(j);
    }

    void forEachStatement(const ForRangeStmt& s) {
        // `кожен i у START..END` -> desugar to a while over a hidden end temp.
        // Range is half-open [START, END): i takes START, START+1, ..., END-1.
        beginScope();
        addLocal(s.var);
        expression(*s.start);         // START -> lands in the loop variable slot
        markInitialized();
        int varSlot = int(cur_->locals.size()) - 1;

        std::string endName = "0край" + std::to_string(forTemp_++);  // unreachable name
        addLocal(endName);
        expression(*s.end);           // END -> hidden slot
        markInitialized();
        int endSlot = int(cur_->locals.size()) - 1;

        at(s);
        size_t loopStart = code().size();
        emit(Op::GET_LOCAL); emit(uint8_t(varSlot));
        emit(Op::GET_LOCAL); emit(uint8_t(endSlot));
        emit(Op::LT);
        size_t exitJump = emitJump(Op::JMP_IF_FALSE);
        emit(Op::POP);

        beginScope();
        block(s.body);
        endScope();

        // i = i + 1
        at(s);
        emit(Op::GET_LOCAL); emit(uint8_t(varSlot));
        emitConst(Value::integer(1));
        emit(Op::ADD);
        emit(Op::SET_LOCAL); emit(uint8_t(varSlot));
        emit(Op::POP);
        emitLoop(loopStart);

        patchJump(exitJump);
        emit(Op::POP);
        endScope();   // drops the loop variable and the hidden end temp
    }

    void whileStatement(const WhileStmt& s) {
        size_t loopStart = code().size();
        expression(*s.cond);

        size_t exitJump = emitJump(Op::JMP_IF_FALSE);
        emit(Op::POP);                        // pop condition (enter body)
        beginScope();
        block(s.body);
        endScope();
        emitLoop(loopStart);

        patchJump(exitJump);
        emit(Op::POP);                        // pop condition (exit)
    }

    // --- expressions ---
    void expression(const Expr& e) {
        at(e);
        switch (e.kind) {
            case ExprKind::Literal: {
                const Value& v = as<LiteralExpr>(e).value;
                if (v.type == ValueType::Nil)       emit(Op::NIL);
                else if (v.type == ValueType::Bool) emit(v.b ? Op::TRUE : Op::FALSE);
                else                                emitConst(v);
                break;
            }
            case ExprKind::Variable: {
                Target t = resolve(as<VariableExpr>(e).name);
                emitTarget(t.getOp, t.arg);
                break;
            }
            case ExprKind::Unary: {
                const UnaryExpr& u = as<UnaryExpr>(e);
                expression(*u.operand);
                at(e);
                emit(u.op == UnaryOp::Neg ? Op::NEG : Op::NOT);
                break;
            }
            case ExprKind::Binary: {
                const BinaryExpr& b = as<BinaryExpr>(e);
                expression(*b.lhs);
                expression(*b.rhs);
                at(e);
                emit(binaryOp(b.op));
                break;
            }
            case ExprKind::Logical:     logical(as<LogicalExpr>(e)); break;
            case ExprKind::Assign:      assignment(as<AssignExpr>(e)); break;
            case ExprKind::Call: {
                const CallExpr& c = as<CallExpr>(e);
                expression(*c.callee);
                for (const ExprPtr& a : c.args) expression(*a);
                at(e);
                if (c.args.size() > 255) fail("забагато аргументів (максимум 255)");
                emit(Op::CALL);
                emit(static_cast<uint8_t>(c.args.size()));
                break;
            }
            case ExprKind::Index: {
                const IndexExpr& x = as<IndexExpr>(e);
                expression(*x.object);
                expression(*x.index);
                at(e);
                emit(Op::INDEX_GET);
                break;
            }
            case ExprKind::IndexAssign: {
                const IndexAssignExpr& x = as<IndexAssignExpr>(e);
                expression(*x.object);
                expression(*x.index);
                expression(*x.value);
                at(e);
                emit(Op::INDEX_SET);
                break;
            }
            case ExprKind::List: {
                const ListExpr& l = as<ListExpr>(e);
                if (l.elements.size() > 0xFFFF) fail("забагато елементів у списку");
                for (const ExprPtr& el : l.elements) expression(*el);
                at(e);
                emit(Op::BUILD_LIST);
                emitU16(static_cast<uint16_t>(l.elements.size()));
                break;
            }
            case ExprKind::Dict: {
                const DictExpr& d = as<DictExpr>(e);
                if (d.entries.size() > 0xFFFF) fail("забагато пар у словнику");
                for (const auto& entry : d.entries) {
                    at(e);
                    emitConst(Value::str(entry.first));
                    expression(*entry.second);
                }
                at(e);
                emit(Op::BUILD_DICT);
                emitU16(static_cast<uint16_t>(d.entries.size()));
                break;
            }
        }
    }

    static Op binaryOp(BinaryOp op) {
        switch (op) {
            case BinaryOp::Add: return Op::ADD;
            case BinaryOp::Sub: return Op::SUB;
            case BinaryOp::Mul: return Op::MUL;
            case BinaryOp::Div: return Op::DIV;
            case BinaryOp::Mod: return Op::MOD;
            case BinaryOp::Eq:  return Op::EQ;
            case BinaryOp::Neq: return Op::NEQ;
            case BinaryOp::Lt:  return Op::LT;
            case BinaryOp::Le:  return Op::LE;
            case BinaryOp::Gt:  return Op::GT;
            case BinaryOp::Ge:  return Op::GE;
        }
        return Op::HALT;   // unreachable
    }

    // Where a name lives: a local slot, a module function, or a global.
    struct Target {
        Op   getOp, setOp;
        int  arg;
        bool isFunc = false, immutable = false;
    };

    Target resolve(const std::string& name) {
        Target t;
        int slot = resolveLocal(name);
        if (slot >= 0) {
            t.immutable = cur_->locals[slot].immutable;
            t.getOp = Op::GET_LOCAL; t.setOp = Op::SET_LOCAL; t.arg = slot;
        } else if (funcIndex_.count(name)) {
            t.isFunc = true;
            t.getOp = Op::GET_FUNC; t.setOp = Op::GET_FUNC;
            t.arg = funcIndex_[name];
        } else {
            t.getOp = Op::GET_GLOBAL; t.setOp = Op::SET_GLOBAL;
            t.arg = addConstant(Value::str(name));
            auto it = immutableGlobals_.find(name);
            t.immutable = (it != immutableGlobals_.end() && it->second);
        }
        return t;
    }

    void emitTarget(Op op, int arg) {
        emit(op);
        if (op == Op::GET_LOCAL || op == Op::SET_LOCAL) emit(uint8_t(arg));
        else emitU16(uint16_t(arg));
    }

    // Variable assignment / compound assignment.
    void assignment(const AssignExpr& a) {
        Target t = resolve(a.name);
        if (t.isFunc) fail("не можна присвоювати функції '" + a.name + "'");
        if (t.immutable) fail("не можна змінювати незмінну '" + a.name + "' (фікс)");

        if (a.op == AssignOp::Set) {
            expression(*a.value);
        } else {
            emitTarget(t.getOp, t.arg);  // current value
            expression(*a.value);
            at(a);
            switch (a.op) {
                case AssignOp::Add: emit(Op::ADD); break;
                case AssignOp::Sub: emit(Op::SUB); break;
                case AssignOp::Mul: emit(Op::MUL); break;
                case AssignOp::Div: emit(Op::DIV); break;
                default: break;
            }
        }
        at(a);
        emitTarget(t.setOp, t.arg);
    }

    void logical(const LogicalExpr& l) {
        expression(*l.lhs);
        at(l);
        if (l.isAnd) {
            // left is on the stack; if false, short-circuit keeping left.
            size_t endJump = emitJump(Op::JMP_IF_FALSE);
            emit(Op::POP);
            expression(*l.rhs);
            patchJump(endJump);
        } else {
            size_t elseJump = emitJump(Op::JMP_IF_FALSE);
            size_t endJump  = emitJump(Op::JMP);
            patchJump(elseJump);
            emit(Op::POP);
            expression(*l.rhs);
            patchJump(endJump);
        }
    }
};

} // namespace

Module compile(const std::string& source, CompileError& err) {
    Module module;
    try {
        std::vector<Token> tokens = tokenize(source);
        Program program = parse(tokens);
        CodeGen(program, module).run();
        err.ok = true;
    } catch (const SyntaxError& e) {
        err.ok = false;
        err.message = e.message;
        err.line = e.line;
        err.column = e.column;
    } catch (const std::exception& e) {
        err.ok = false;
        err.message = e.what();
        err.line = 0;
    }
    return module;
}

} // namespace vasyan
