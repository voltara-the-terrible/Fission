//
// Do-block locals are named by register (`v{reg}`), so a register reused across sibling do-blocks —
// or one that shadows an outer local — reads as the same `vN`. This pass gives each do-block's
// locals distinct, non-shadowing names by renaming them (declaration + uses within the block) to a
// fresh unique name whenever the register name would collide with an enclosing or already-used name.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class DoBlockLocalRenamer {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &stmts) {
        for (const auto &s : stmts)
            CollectStmt(s);
        std::unordered_set<std::string> enclosing;
        ProcessScope(stmts, enclosing);
    }

  private:
    std::unordered_set<std::string> m_allNames;    // every identifier name in the tree (so fresh names don't clash)
    std::unordered_set<std::string> m_usedDoNames; // names already taken by do-block locals (sibling distinctness)
    int m_counter = 0;

    std::string Fresh() {
        std::string n;
        do {
            n = "v" + std::to_string(m_counter++);
        } while (m_allNames.count(n) || m_usedDoNames.count(n));
        m_allNames.insert(n);
        return n;
    }

    static std::string IdentName(const std::shared_ptr<Expression> &e) {
        if (auto ie = std::dynamic_pointer_cast<IdentifierExpressionNode>(e); ie && ie->identifier)
            return ie->identifier->name;
        return {};
    }

    // names declared by a `local <name> = ...` at the top level of a statement list.
    static std::unordered_set<std::string> DeclaredNames(const std::vector<std::shared_ptr<Statement>> &stmts) {
        std::unordered_set<std::string> names;
        for (const auto &s : stmts)
            if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
                auto n = IdentName(decl->identifier);
                if (!n.empty())
                    names.insert(n);
            }
        return names;
    }

    // ---- name collection (deep) ----
    void CollectExpr(const std::shared_ptr<Expression> &e) {
        if (!e)
            return;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e)) {
            if (id->identifier)
                m_allNames.insert(id->identifier->name);
        } else if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(e)) {
            CollectExpr(bin->left);
            CollectExpr(bin->right);
        } else if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(e)) {
            CollectExpr(un->operand);
        } else if (auto ie = std::dynamic_pointer_cast<IfElseExpressionNode>(e)) {
            CollectExpr(ie->condition);
            CollectExpr(ie->thenExpr);
            CollectExpr(ie->elseExpr);
        } else if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(e)) {
            CollectExpr(idx->left);
            CollectExpr(idx->right);
        } else if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(e)) {
            CollectExpr(mem->table);
            CollectExpr(mem->key);
        } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            CollectExpr(call->callee);
            for (auto &a : call->arguments)
                CollectExpr(a);
        } else if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            CollectExpr(nc->calledOn);
            for (auto &a : nc->arguments)
                CollectExpr(a);
        } else if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(e)) {
            for (auto &x : tbl->expressions)
                CollectExpr(x);
        } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(e); fn && fn->lpFunctionBody) {
            for (const auto &arg : fn->argumentsNames)
                if (arg.second)
                    CollectExpr(arg.second->argumentName);
            for (const auto &b : fn->lpFunctionBody->body)
                CollectStmt(b);
        }
    }
    void CollectStmt(const std::shared_ptr<Statement> &s) {
        if (!s)
            return;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
            CollectExpr(decl->identifier);
            CollectExpr(decl->value);
        } else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
            CollectExpr(asn->left);
            CollectExpr(asn->right);
        } else if (auto cb = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(s)) {
            CollectExpr(cb->left);
            CollectExpr(cb->right);
        } else if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s)) {
            CollectExpr(es->expression);
        } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(s)) {
            for (auto &v : ret->returnValues)
                CollectExpr(v);
        } else if (auto doB = std::dynamic_pointer_cast<DoBlockNode>(s); doB && doB->body) {
            for (auto &b : doB->body->body)
                CollectStmt(b);
        } else if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(s)) {
            CollectExpr(ifS->condition);
            if (ifS->thenBranch)
                for (auto &b : ifS->thenBranch->body)
                    CollectStmt(b);
            if (ifS->elseBranch)
                for (auto &b : ifS->elseBranch->body)
                    CollectStmt(b);
        } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s)) {
            CollectExpr(w->condition);
            if (w->body)
                for (auto &b : w->body->body)
                    CollectStmt(b);
        } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s)) {
            CollectExpr(r->condition);
            if (r->body)
                for (auto &b : r->body->body)
                    CollectStmt(b);
        } else if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(s); fnum && fnum->lpLoopBody) {
            for (auto &b : fnum->lpLoopBody->body)
                CollectStmt(b);
        } else if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(s); fgen && fgen->body) {
            for (auto &b : fgen->body->body)
                CollectStmt(b);
        } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            CollectExpr(std::static_pointer_cast<Expression>(fn));
        }
    }

    // ---- scoped rename of `old` -> `neu`, NOT descending into nested scopes that redeclare `old` ----
    void RenameExpr(const std::shared_ptr<Expression> &e, const std::string &old, const std::string &neu) {
        if (!e)
            return;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e)) {
            if (id->identifier && id->identifier->name == old)
                id->identifier->name = neu;
        } else if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(e)) {
            RenameExpr(bin->left, old, neu);
            RenameExpr(bin->right, old, neu);
        } else if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(e)) {
            RenameExpr(un->operand, old, neu);
        } else if (auto ie = std::dynamic_pointer_cast<IfElseExpressionNode>(e)) {
            RenameExpr(ie->condition, old, neu);
            RenameExpr(ie->thenExpr, old, neu);
            RenameExpr(ie->elseExpr, old, neu);
        } else if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(e)) {
            RenameExpr(idx->left, old, neu);
            RenameExpr(idx->right, old, neu);
        } else if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(e)) {
            RenameExpr(mem->table, old, neu);
            // mem->key is a string literal member name, not an identifier — leave it.
        } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            RenameExpr(call->callee, old, neu);
            for (auto &a : call->arguments)
                RenameExpr(a, old, neu);
        } else if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            RenameExpr(nc->calledOn, old, neu);
            for (auto &a : nc->arguments)
                RenameExpr(a, old, neu);
        } else if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(e)) {
            for (auto &x : tbl->expressions)
                RenameExpr(x, old, neu);
        } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(e); fn && fn->lpFunctionBody) {
            // a function body that declares `old` as a param/local shadows the outer name — skip it.
            bool shadows = DeclaredNames(fn->lpFunctionBody->body).count(old) != 0;
            for (const auto &arg : fn->argumentsNames)
                if (arg.second && IdentName(arg.second->argumentName) == old)
                    shadows = true;
            if (!shadows)
                RenameStmts(fn->lpFunctionBody->body, old, neu);
        }
    }
    void RenameStmts(std::vector<std::shared_ptr<Statement>> &stmts, const std::string &old, const std::string &neu) {
        for (auto &s : stmts) {
            if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
                RenameExpr(decl->identifier, old, neu);
                RenameExpr(decl->value, old, neu);
            } else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
                RenameExpr(asn->left, old, neu);
                RenameExpr(asn->right, old, neu);
            } else if (auto cb = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(s)) {
                RenameExpr(cb->left, old, neu);
                RenameExpr(cb->right, old, neu);
            } else if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s)) {
                RenameExpr(es->expression, old, neu);
            } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(s)) {
                for (auto &v : ret->returnValues)
                    RenameExpr(v, old, neu);
            } else if (auto doB = std::dynamic_pointer_cast<DoBlockNode>(s); doB && doB->body) {
                if (!DeclaredNames(doB->body->body).count(old)) // a nested do-block declaring `old` shadows it
                    RenameStmts(doB->body->body, old, neu);
            } else if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(s)) {
                RenameExpr(ifS->condition, old, neu);
                if (ifS->thenBranch && !DeclaredNames(ifS->thenBranch->body).count(old))
                    RenameStmts(ifS->thenBranch->body, old, neu);
                if (ifS->elseBranch && !DeclaredNames(ifS->elseBranch->body).count(old))
                    RenameStmts(ifS->elseBranch->body, old, neu);
            } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s)) {
                RenameExpr(w->condition, old, neu);
                if (w->body && !DeclaredNames(w->body->body).count(old))
                    RenameStmts(w->body->body, old, neu);
            } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s)) {
                RenameExpr(r->condition, old, neu);
                if (r->body && !DeclaredNames(r->body->body).count(old))
                    RenameStmts(r->body->body, old, neu);
            } else if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(s); fnum && fnum->lpLoopBody) {
                if (!DeclaredNames(fnum->lpLoopBody->body).count(old))
                    RenameStmts(fnum->lpLoopBody->body, old, neu);
            } else if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(s); fgen && fgen->body) {
                if (!DeclaredNames(fgen->body->body).count(old))
                    RenameStmts(fgen->body->body, old, neu);
            } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
                RenameExpr(std::static_pointer_cast<Expression>(fn), old, neu);
            }
        }
    }

    void ProcessScope(std::vector<std::shared_ptr<Statement>> &stmts, const std::unordered_set<std::string> &enclosing) {
        std::unordered_set<std::string> visible = enclosing;
        for (const auto &n : DeclaredNames(stmts))
            visible.insert(n);

        for (auto &s : stmts) {
            if (auto doB = std::dynamic_pointer_cast<DoBlockNode>(s); doB && doB->body) {
                // rename this block's own locals that would collide, then recurse for nested blocks.
                std::unordered_set<std::string> ownNames;
                for (auto &inner : doB->body->body) {
                    auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(inner);
                    if (!decl)
                        continue;
                    const std::string name = IdentName(decl->identifier);
                    if (name.empty())
                        continue;
                    if (visible.count(name) || m_usedDoNames.count(name) || ownNames.count(name)) {
                        const std::string neu = Fresh();
                        RenameStmts(doB->body->body, name, neu);
                        m_usedDoNames.insert(neu);
                        ownNames.insert(neu);
                    } else {
                        m_usedDoNames.insert(name);
                        ownNames.insert(name);
                    }
                }
                ProcessScope(doB->body->body, visible);
            } else if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(s)) {
                if (ifS->thenBranch)
                    ProcessScope(ifS->thenBranch->body, visible);
                if (ifS->elseBranch)
                    ProcessScope(ifS->elseBranch->body, visible);
            } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s); w && w->body) {
                ProcessScope(w->body->body, visible);
            } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s); r && r->body) {
                ProcessScope(r->body->body, visible);
            } else if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(s); fnum && fnum->lpLoopBody) {
                ProcessScope(fnum->lpLoopBody->body, visible);
            } else if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(s); fgen && fgen->body) {
                ProcessScope(fgen->body->body, visible);
            } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s); fn && fn->lpFunctionBody) {
                ProcessScope(fn->lpFunctionBody->body, visible);
            } else if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s)) {
                ProcessExprScopes(es->expression, visible);
            } else if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
                ProcessExprScopes(decl->value, visible);
            } else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
                ProcessExprScopes(asn->right, visible);
            }
        }
    }

    // descend into function bodies reachable through expressions (e.g. inline call-arg closures).
    void ProcessExprScopes(const std::shared_ptr<Expression> &e, const std::unordered_set<std::string> &visible) {
        if (!e)
            return;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(e); fn && fn->lpFunctionBody) {
            ProcessScope(fn->lpFunctionBody->body, visible);
        } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            ProcessExprScopes(call->callee, visible);
            for (auto &a : call->arguments)
                ProcessExprScopes(a, visible);
        } else if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            ProcessExprScopes(nc->calledOn, visible);
            for (auto &a : nc->arguments)
                ProcessExprScopes(a, visible);
        } else if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(e)) {
            ProcessExprScopes(bin->left, visible);
            ProcessExprScopes(bin->right, visible);
        } else if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(e)) {
            ProcessExprScopes(un->operand, visible);
        } else if (auto ie = std::dynamic_pointer_cast<IfElseExpressionNode>(e)) {
            ProcessExprScopes(ie->condition, visible);
            ProcessExprScopes(ie->thenExpr, visible);
            ProcessExprScopes(ie->elseExpr, visible);
        } else if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(e)) {
            for (auto &x : tbl->expressions)
                ProcessExprScopes(x, visible);
        }
    }
};
