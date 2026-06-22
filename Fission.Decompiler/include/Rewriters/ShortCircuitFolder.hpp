//
// Created by Dottik on 2/6/2026.
//
// Collapses the diamond Luau lowers `local V = C and P or F` into back to a single
// expression. The three duplicated `tail` copies must be identical (checked by
// rendering) for the fold to be sound.
//
// Also folds the simpler `T = A or B` diamond. When the `or` result is stored to a
// table/upvalue (SETTABLEKS/SETUPVAL) rather than kept in a register, Luau lowers it
// to `local v = A; if v then T = v; <tail> end; local v = B; T = v; <tail>` — the
// store and everything after it are duplicated into the truthy branch. The two tail
// copies must render identically and `v` must die with the diamond for the fold.
//
// And the `local V = A or B` diamond, where the `or` result stays in the register and
// the code that *reads* it is what gets duplicated: `local v = A; if v then <tail> end;
// local v = B; <tail>`. Here both tail copies read `v`, so their identical rendering is
// itself the soundness proof — when A is truthy the tail runs with v = A, otherwise with
// v = B, exactly what `local v = A or B; <tail>` evaluates.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"
#include "SourceGenerator/Generator.hpp"

#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ShortCircuitFolder : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (size_t i = 0; i + 1 < stmts.size();) {
            if (TryFoldAt(stmts, i) || TryFoldOrAt(stmts, i) || TryFoldOrLocalAt(stmts, i))
                continue; // folded in place; re-check the same index
            ++i;
        }
    }

  private:
    static std::optional<std::string> SimpleIdentName(const std::shared_ptr<Expression> &expr) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); id && id->identifier)
            return id->identifier->name;
        return std::nullopt;
    }

    // value written to `v` by `stmt` (`v = rhs`, or a Call/NameCall whose single ret is v), else null.
    static std::shared_ptr<Expression> AsAssignToVar(const std::shared_ptr<Statement> &stmt, const std::string &v) {
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
            if (auto name = SimpleIdentName(asn->left); name && *name == v)
                return asn->right;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression))
                if (nc->rets.size() == 1 && SimpleIdentName(nc->rets[0]).value_or("") == v)
                    return nc;
            if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(es->expression))
                if (c->rets.size() == 1 && SimpleIdentName(c->rets[0]).value_or("") == v)
                    return c;
        }
        return nullptr;
    }

    // a Call/NameCall pulled into a sub-expression must render inline (no `local x =` prefix).
    static void InlineifyValue(const std::shared_ptr<Expression> &e) {
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            nc->rets.clear();
            nc->inlineCall = true;
        } else if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            c->rets.clear();
            c->inlineCall = true;
        }
    }

    static std::string RenderStatements(const std::vector<std::shared_ptr<Statement>> &body) {
        SourceGenerator g;
        for (const auto &s : body)
            if (s)
                s->Accept(&g);
        return g.buffer.str();
    }
    static bool StatementsEqual(const std::vector<std::shared_ptr<Statement>> &a, const std::vector<std::shared_ptr<Statement>> &b) {
        return RenderStatements(a) == RenderStatements(b);
    }

    static bool TryFoldAt(std::vector<std::shared_ptr<Statement>> &stmts, size_t i) {
        auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
        std::string vname;
        if (decl && decl->value == nullptr)
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier)
                vname = id->identifier->name;
        if (vname.empty())
            return false;

        auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmts[i + 1]);
        if (!ifS || !ifS->thenBranch || !ifS->elseBranch)
            return false;

        auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(ifS->condition);
        auto &thenB = ifS->thenBranch->body;
        auto &elseB = ifS->elseBranch->body;
        if (!un || un->op != "not " || thenB.empty() || elseB.size() != 2)
            return false;

        auto fallback = AsAssignToVar(thenB[0], vname);
        auto primary = AsAssignToVar(elseB[0], vname);
        auto innerIf = std::dynamic_pointer_cast<IfStatementNode>(elseB[1]);
        const bool innerOk = innerIf && innerIf->thenBranch && !innerIf->elseBranch && SimpleIdentName(innerIf->condition).value_or("") == vname;
        if (!fallback || !primary || !innerOk)
            return false;

        std::vector<std::shared_ptr<Statement>> tailThen(thenB.begin() + 1, thenB.end());
        std::vector<std::shared_ptr<Statement>> tailInner = innerIf->thenBranch->body;
        std::vector<std::shared_ptr<Statement>> tailAfter(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 2, stmts.end());
        if (!StatementsEqual(tailThen, tailAfter) || !StatementsEqual(tailInner, tailAfter))
            return false;

        InlineifyValue(primary);
        InlineifyValue(fallback);
        auto andExpr = std::make_shared<BinaryExpressionNode>("and", un->operand, primary);
        auto orExpr = std::make_shared<BinaryExpressionNode>("or", andExpr, fallback);
        decl->value = orExpr;                                                  // local V = C and P or F
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1);       // drop the diamond
        return true;
    }

    static bool IsTerminator(const std::shared_ptr<Statement> &s) {
        return std::dynamic_pointer_cast<ReturnStatementNode>(s) != nullptr || std::dynamic_pointer_cast<BreakStatementNode>(s) != nullptr ||
               std::dynamic_pointer_cast<ContinueStatementNode>(s) != nullptr;
    }

    // value bound to local `v` by `local v = <val>` (declaration, plain assignment, or a Call/NameCall ret), else null.
    static std::shared_ptr<Expression> AsDeclOfVar(const std::shared_ptr<Statement> &stmt, const std::string &v) {
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
            if (decl->value && SimpleIdentName(decl->identifier).value_or("") == v)
                return decl->value;
        return AsAssignToVar(stmt, v);
    }

    // write target T of `T = v` (rhs is exactly local v), else null.
    static std::shared_ptr<Expression> AsAssignFromVar(const std::shared_ptr<Statement> &stmt, const std::string &v) {
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
            if (SimpleIdentName(asn->right).value_or("") == v)
                return asn->left;
        return nullptr;
    }

    // render an expression's *value*, ignoring a Call/NameCall's ret binding (which names the temp itself).
    static std::string RenderValue(const std::shared_ptr<Expression> &e) {
        SourceGenerator g;
        if (!e)
            return "";
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            if (nc->calledOn)
                nc->calledOn->Accept(&g);
            g.buffer << ":";
            if (nc->callWhat)
                nc->callWhat->Accept(&g);
            g.buffer << "(";
            for (const auto &a : nc->arguments)
                if (a)
                    a->Accept(&g);
            return g.buffer.str();
        }
        if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            if (c->callee)
                c->callee->Accept(&g);
            g.buffer << "(";
            for (const auto &a : c->arguments)
                if (a)
                    a->Accept(&g);
            return g.buffer.str();
        }
        e->Accept(&g);
        return g.buffer.str();
    }

    // whole-word (identifier-boundary) occurrence of `name` in rendered text.
    static bool NameInText(const std::string &s, const std::string &name) {
        if (name.empty())
            return false;
        const auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (size_t pos = s.find(name); pos != std::string::npos; pos = s.find(name, pos + name.size())) {
            const bool leftOk = pos == 0 || !isIdent(s[pos - 1]);
            const size_t end = pos + name.size();
            const bool rightOk = end >= s.size() || !isIdent(s[end]);
            if (leftOk && rightOk)
                return true;
        }
        return false;
    }

    // Fold `local v = A; if v then T = v; <tail> end; local v = B; T = v; <tail>` into `T = A or B; <tail>`.
    static bool TryFoldOrAt(std::vector<std::shared_ptr<Statement>> &stmts, size_t i) {
        if (i + 3 >= stmts.size())
            return false;

        // [i] local v = A
        auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
        if (!decl || !decl->value)
            return false;
        const auto vOpt = SimpleIdentName(decl->identifier);
        if (!vOpt || vOpt->empty())
            return false;
        const std::string v = *vOpt;
        const auto primary = decl->value; // A

        // [i+1] if v then  T = v ; <tail> (terminating) end   (no else)
        auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmts[i + 1]);
        if (!ifS || !ifS->thenBranch || (ifS->elseBranch && !ifS->elseBranch->body.empty()))
            return false;
        if (SimpleIdentName(ifS->condition).value_or("") != v)
            return false;
        auto &thenB = ifS->thenBranch->body;
        if (thenB.empty() || !AsAssignFromVar(thenB[0], v) || !IsTerminator(thenB.back()))
            return false;

        // [i+2] local v = B
        const auto fallback = AsDeclOfVar(stmts[i + 2], v); // B
        if (!fallback)
            return false;

        // [i+3] T = v
        auto afterAssign = std::dynamic_pointer_cast<AssignmentStatementNode>(stmts[i + 3]);
        if (!afterAssign || SimpleIdentName(afterAssign->right).value_or("") != v)
            return false;

        // the truthy-branch store and the post-diamond store must hit the same target.
        if (RenderStatements({thenB[0]}) != RenderStatements({stmts[i + 3]}))
            return false;

        std::vector<std::shared_ptr<Statement>> tailThen(thenB.begin() + 1, thenB.end());
        std::vector<std::shared_ptr<Statement>> tailAfter(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 4, stmts.end());
        if (!StatementsEqual(tailThen, tailAfter))
            return false;

        // v must die with the diamond: if the target, the fallback, or the shared tail still reads it, the fold is unsound.
        if (NameInText(RenderValue(afterAssign->left), v) || NameInText(RenderValue(fallback), v) || NameInText(RenderStatements(tailAfter), v))
            return false;

        InlineifyValue(fallback);
        auto orExpr = std::make_shared<BinaryExpressionNode>("or", primary, fallback);
        auto folded = std::make_shared<AssignmentStatementNode>(afterAssign->left, orExpr); // T = A or B
        folded->debugLine = afterAssign->debugLine;
        folded->debugReg = afterAssign->debugReg;
        folded->debugOpCode = afterAssign->debugOpCode;

        stmts[i] = folded;
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1, stmts.begin() + static_cast<std::ptrdiff_t>(i) + 4);
        return true;
    }

    // Fold `local v = A; if v then <tail> end; local v = B; <tail>` into `local v = A or B; <tail>`.
    // The `or` result stays in the local and the duplicated tail reads it, so equal tail renderings
    // are the soundness proof (truthy → tail with v = A, falsy → tail with v = B).
    static bool TryFoldOrLocalAt(std::vector<std::shared_ptr<Statement>> &stmts, size_t i) {
        if (i + 2 >= stmts.size())
            return false;

        // [i] local v = A
        auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
        if (!decl || !decl->value)
            return false;
        const auto vOpt = SimpleIdentName(decl->identifier);
        if (!vOpt || vOpt->empty())
            return false;
        const std::string v = *vOpt;
        const auto primary = decl->value; // A

        // [i+1] if v then <tail> (terminating) end   (no else)
        auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmts[i + 1]);
        if (!ifS || !ifS->thenBranch || (ifS->elseBranch && !ifS->elseBranch->body.empty()))
            return false;
        if (SimpleIdentName(ifS->condition).value_or("") != v)
            return false;
        auto &thenB = ifS->thenBranch->body;
        if (thenB.empty() || !IsTerminator(thenB.back()))
            return false;

        // [i+2] local v = B  (redefinition feeding the falsy path)
        const auto fallback = AsDeclOfVar(stmts[i + 2], v); // B
        if (!fallback)
            return false;

        // the truthy branch must be exactly the post-diamond tail (both read v identically).
        std::vector<std::shared_ptr<Statement>> tailAfter(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 3, stmts.end());
        if (tailAfter.empty() || !StatementsEqual(thenB, tailAfter))
            return false;

        InlineifyValue(fallback);
        decl->value = std::make_shared<BinaryExpressionNode>("or", primary, fallback);             // local v = A or B
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1, stmts.begin() + static_cast<std::ptrdiff_t>(i) + 3); // drop if + redecl
        return true;
    }
};
