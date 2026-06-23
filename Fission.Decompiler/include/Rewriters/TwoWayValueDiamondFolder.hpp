//
// Created by voltara on 6/23/2026.
//
// Folds a residual 2-way value diamond back into a single declaration:
//
//     local V
//     if C then V = A else V = B end          -->  local V = if C then A else B
//                                                   (or `C and A or B` when A is provably truthy
//                                                    and if-else expressions are disabled)
//
// DetectIfElseExpression already collapses these at lift time when each arm is a single value-load
// instruction. But `A and B or C` lowers each arm with a trailing `or`-recheck jump, so the arm is
// *two* instructions and the lift-time detector bails; the recheck is then simplified away, leaving
// this clean 2-way statement form that no other pass revisits. This catches it on the AST.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class TwoWayValueDiamondFolder : public ASTRewriter {
  public:
    explicit TwoWayValueDiamondFolder(bool useIfElseExpressions) : m_useIfElseExpressions(useIfElseExpressions) {}

  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (size_t i = 0; i + 1 < stmts.size();) {
            if (TryFoldAt(stmts, i))
                continue; // folded in place; the new decl can't fold again, but indices shifted — re-check.
            ++i;
        }
    }

  private:
    static std::optional<std::string> SimpleIdentName(const std::shared_ptr<Expression> &expr) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); id && id->identifier)
            return id->identifier->name;
        return std::nullopt;
    }

    // The single value `branch` assigns to `v` (`v = rhs`), or null if it is not exactly that.
    static std::shared_ptr<Expression> SoleAssignmentTo(const std::vector<std::shared_ptr<Statement>> &branch, const std::string &v) {
        if (branch.size() != 1)
            return nullptr;
        auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(branch[0]);
        if (!asn)
            return nullptr;
        if (SimpleIdentName(asn->left).value_or("") != v)
            return nullptr;
        return asn->right;
    }

    // A value that can never be nil/false, so `cond and value or other` matches `if cond then value
    // else other`. Mirrors ASTLifter::IsProvablyTruthy.
    static bool IsProvablyTruthy(const std::shared_ptr<Expression> &e) {
        if (std::dynamic_pointer_cast<NumberLiteralNode>(e) || std::dynamic_pointer_cast<IntegerLiteralNode>(e) ||
            std::dynamic_pointer_cast<StringLiteralNode>(e) || std::dynamic_pointer_cast<TableLiteralNode>(e) ||
            std::dynamic_pointer_cast<VectorNode>(e) || std::dynamic_pointer_cast<FunctionDeclarationNode>(e))
            return true;
        if (auto b = std::dynamic_pointer_cast<BooleanLiteralNode>(e))
            return b->value;
        return false;
    }

    bool TryFoldAt(std::vector<std::shared_ptr<Statement>> &stmts, size_t i) {
        auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
        if (!decl || decl->value != nullptr)
            return false;
        auto declName = SimpleIdentName(decl->identifier);
        if (!declName)
            return false;

        // Only fold when something follows the `if` in this same block. A genuine value diamond is
        // consumed by code after it; a diamond that is the entire content of a block is typically a
        // nested phi-merge whose real consumer lives in an outer scope (HoistPhiLocals re-declares
        // the register there). Folding the latter produces a `local V = ...` the dead-local pass then
        // drops, silently losing both arm values — so leave those as the statement form.
        if (i + 2 >= stmts.size())
            return false;

        auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmts[i + 1]);
        if (!ifS || !ifS->thenBranch || !ifS->elseBranch || !ifS->condition)
            return false;

        auto thenV = SoleAssignmentTo(ifS->thenBranch->body, *declName);
        auto elseV = SoleAssignmentTo(ifS->elseBranch->body, *declName);
        if (!thenV || !elseV)
            return false;

        // A genuine condition only — a folded boolean would be nonsense as a ternary.
        auto cond = ifS->condition;
        if (std::dynamic_pointer_cast<BooleanLiteralNode>(cond))
            return false;

        // `if not X then A else B` reads cleaner as `if X then B else A`.
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(cond); un && un->op == "not ") {
            cond = un->operand;
            std::swap(thenV, elseV);
        }

        std::shared_ptr<Expression> value;
        if (!m_useIfElseExpressions && IsProvablyTruthy(thenV)) {
            auto andExpr = std::make_shared<BinaryExpressionNode>("and", cond, thenV);
            value = std::make_shared<BinaryExpressionNode>("or", andExpr, elseV);
        } else {
            value = std::make_shared<IfElseExpressionNode>(cond, thenV, elseV);
        }

        decl->value = value;                                             // local V = <folded>
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1); // drop the diamond
        return true;
    }

    bool m_useIfElseExpressions = false;
};
