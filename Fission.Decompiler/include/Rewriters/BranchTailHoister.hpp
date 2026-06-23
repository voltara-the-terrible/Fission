//
// Created by voltara on 6/23/2026.
//
// Hoists a run of statements shared by both arms of an `if` out of the branches so it appears once.
//
// A conditional whose two arms reconverge on a block that ends in `return` lifts with that merge
// block *duplicated* into each arm (and emitted once after the `if`) — see LiftControlFlow's
// "return blocks are allowed to be duplicated" rule, which is load-bearing for the short-circuit
// folder and cannot simply be dropped. The result is correct but verbose:
//
//     if c then a() ; tail() ; return x
//     else      b() ; tail() ; return x end
//     tail() ; return x                         -- the merge copy, now unreachable
//
// This pass strips the common suffix from both arms. When that suffix already follows the `if`
// (the lifted merge copy), the arms just fall through to it; otherwise the suffix is re-inserted
// after the `if`. Either way the suffix runs exactly once, which is what the original source did.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"

#include <algorithm>
#include <memory>
#include <vector>

class BranchTailHoister : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (size_t i = 0; i < stmts.size(); ++i) {
            auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmts[i]);
            if (!ifS || !ifS->thenBranch || !ifS->elseBranch)
                continue;

            auto &thenBody = ifS->thenBranch->body;
            auto &elseBody = ifS->elseBranch->body;

            // Need room to keep at least one statement in each arm (an emptied arm would render as
            // `if c then end`) while hoisting at least one.
            size_t cap = std::min(thenBody.size(), elseBody.size());
            if (cap <= 1)
                continue;
            cap -= 1;

            size_t k = 0;
            while (k < cap && StmtEqual(thenBody[thenBody.size() - 1 - k], elseBody[elseBody.size() - 1 - k]))
                ++k;
            if (k == 0)
                continue;

            // The shared suffix, taken from the then-arm.
            std::vector<std::shared_ptr<Statement>> suffix(thenBody.end() - static_cast<long>(k), thenBody.end());

            // Is this suffix already sitting right after the `if` (the lifted merge copy)?
            bool trailingMatches = (i + 1 + k <= stmts.size());
            for (size_t j = 0; trailingMatches && j < k; ++j)
                trailingMatches = StmtEqual(stmts[i + 1 + j], suffix[j]);

            thenBody.erase(thenBody.end() - static_cast<long>(k), thenBody.end());
            elseBody.erase(elseBody.end() - static_cast<long>(k), elseBody.end());

            if (!trailingMatches)
                stmts.insert(stmts.begin() + static_cast<long>(i) + 1, suffix.begin(), suffix.end());
        }
    }

  private:
    static bool VecEqual(const std::vector<std::shared_ptr<Expression>> &a, const std::vector<std::shared_ptr<Expression>> &b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!ExprEqual(a[i], b[i]))
                return false;
        return true;
    }

    // Conservative structural equality: returns true only for node kinds explicitly handled whose
    // children all match. Anything unrecognised compares unequal, so the pass never hoists a tail it
    // cannot prove identical.
    static bool ExprEqual(const std::shared_ptr<Expression> &a, const std::shared_ptr<Expression> &b) {
        if (a == b)
            return true;
        if (!a || !b)
            return false;

        if (auto ea = std::dynamic_pointer_cast<IdentifierExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<IdentifierExpressionNode>(b);
            return eb && ea->identifier && eb->identifier && ea->identifier->name == eb->identifier->name;
        }
        if (auto ea = std::dynamic_pointer_cast<Identifier>(a)) {
            auto eb = std::dynamic_pointer_cast<Identifier>(b);
            return eb && ea->name == eb->name;
        }
        if (auto ea = std::dynamic_pointer_cast<StringLiteralNode>(a)) {
            auto eb = std::dynamic_pointer_cast<StringLiteralNode>(b);
            return eb && ea->value == eb->value;
        }
        if (auto ea = std::dynamic_pointer_cast<NumberLiteralNode>(a)) {
            auto eb = std::dynamic_pointer_cast<NumberLiteralNode>(b);
            return eb && ea->value == eb->value;
        }
        if (auto ea = std::dynamic_pointer_cast<IntegerLiteralNode>(a)) {
            auto eb = std::dynamic_pointer_cast<IntegerLiteralNode>(b);
            return eb && ea->value == eb->value;
        }
        if (auto ea = std::dynamic_pointer_cast<BooleanLiteralNode>(a)) {
            auto eb = std::dynamic_pointer_cast<BooleanLiteralNode>(b);
            return eb && ea->value == eb->value;
        }
        if (std::dynamic_pointer_cast<NilLiteralNode>(a))
            return std::dynamic_pointer_cast<NilLiteralNode>(b) != nullptr;
        if (std::dynamic_pointer_cast<VarArgExpression>(a))
            return std::dynamic_pointer_cast<VarArgExpression>(b) != nullptr;
        if (std::dynamic_pointer_cast<NoExpressionNode>(a))
            return std::dynamic_pointer_cast<NoExpressionNode>(b) != nullptr;
        if (auto ea = std::dynamic_pointer_cast<VectorNode>(a)) {
            auto eb = std::dynamic_pointer_cast<VectorNode>(b);
            return eb && ea->x == eb->x && ea->y == eb->y && ea->z == eb->z && ea->w == eb->w;
        }
        if (auto ea = std::dynamic_pointer_cast<MemberExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<MemberExpressionNode>(b);
            return eb && ExprEqual(ea->table, eb->table) && ExprEqual(ea->key, eb->key);
        }
        if (auto ea = std::dynamic_pointer_cast<IndexExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<IndexExpressionNode>(b);
            return eb && ExprEqual(ea->left, eb->left) && ExprEqual(ea->right, eb->right);
        }
        if (auto ea = std::dynamic_pointer_cast<UnaryExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<UnaryExpressionNode>(b);
            return eb && ea->op == eb->op && ExprEqual(ea->operand, eb->operand);
        }
        // BinaryExpressionNode also covers Table/Compound variants via their shared fields.
        if (auto ea = std::dynamic_pointer_cast<BinaryExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<BinaryExpressionNode>(b);
            return eb && ea->op == eb->op && ExprEqual(ea->left, eb->left) && ExprEqual(ea->right, eb->right);
        }
        if (auto ea = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(b);
            return eb && ea->op == eb->op && ExprEqual(ea->left, eb->left) && ExprEqual(ea->right, eb->right);
        }
        if (auto ea = std::dynamic_pointer_cast<IfElseExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<IfElseExpressionNode>(b);
            return eb && ExprEqual(ea->condition, eb->condition) && ExprEqual(ea->thenExpr, eb->thenExpr) && ExprEqual(ea->elseExpr, eb->elseExpr);
        }
        if (auto ea = std::dynamic_pointer_cast<CallExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<CallExpressionNode>(b);
            return eb && ExprEqual(ea->callee, eb->callee) && VecEqual(ea->arguments, eb->arguments) && VecEqual(ea->rets, eb->rets);
        }
        if (auto ea = std::dynamic_pointer_cast<NameCallExpressionNode>(a)) {
            auto eb = std::dynamic_pointer_cast<NameCallExpressionNode>(b);
            return eb && ExprEqual(ea->calledOn, eb->calledOn) && ExprEqual(ea->callWhat, eb->callWhat) && VecEqual(ea->arguments, eb->arguments) &&
                   VecEqual(ea->rets, eb->rets);
        }
        if (auto ea = std::dynamic_pointer_cast<TableLiteralNode>(a)) {
            auto eb = std::dynamic_pointer_cast<TableLiteralNode>(b);
            return eb && VecEqual(ea->expressions, eb->expressions);
        }
        return false;
    }

    static bool StmtEqual(const std::shared_ptr<Statement> &a, const std::shared_ptr<Statement> &b) {
        if (a == b)
            return true;
        if (!a || !b)
            return false;

        if (auto sa = std::dynamic_pointer_cast<ReturnStatementNode>(a)) {
            auto sb = std::dynamic_pointer_cast<ReturnStatementNode>(b);
            return sb && VecEqual(sa->returnValues, sb->returnValues);
        }
        if (auto sa = std::dynamic_pointer_cast<ExpressionStatementNode>(a)) {
            auto sb = std::dynamic_pointer_cast<ExpressionStatementNode>(b);
            return sb && ExprEqual(sa->expression, sb->expression);
        }
        if (auto sa = std::dynamic_pointer_cast<AssignmentStatementNode>(a)) {
            auto sb = std::dynamic_pointer_cast<AssignmentStatementNode>(b);
            return sb && ExprEqual(sa->left, sb->left) && ExprEqual(sa->right, sb->right);
        }
        if (auto sa = std::dynamic_pointer_cast<VariableDeclarationNode>(a)) {
            auto sb = std::dynamic_pointer_cast<VariableDeclarationNode>(b);
            return sb && ExprEqual(sa->identifier, sb->identifier) && ExprEqual(sa->value, sb->value);
        }
        if (std::dynamic_pointer_cast<BreakStatementNode>(a))
            return std::dynamic_pointer_cast<BreakStatementNode>(b) != nullptr;
        if (std::dynamic_pointer_cast<ContinueStatementNode>(a))
            return std::dynamic_pointer_cast<ContinueStatementNode>(b) != nullptr;
        return false;
    }
};
