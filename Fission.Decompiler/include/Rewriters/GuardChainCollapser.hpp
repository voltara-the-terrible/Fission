//
// Created by Claude on 22/6/2026.
//
// Luau lowers `if A and B and C then S end` into a chain of single-condition guard
// blocks whose false edge all target the shared post-compound merge. The lifter
// (ASTLifter::LiftControlFlow, the `mergeIdx == falseIdx` arm) reconstructs each
// guard as its own nested `if`, yielding `if A then if B then if C then S end end end`
// — semantically correct but one nesting level per `and`-operand.
//
// This pass re-folds that shape: an else-less `if` whose then-branch is EXACTLY one
// else-less `if` collapses into `if A and B then <inner body> end`, iterating so a whole
// chain folds back to a single compound condition. The fold is value-preserving — Luau's
// `and` short-circuits left-to-right exactly as the nested guards did, and neither arm has
// an `else` whose side effects could be lost.
//
// Soundness boundary: the `then`-branch must hold *exactly* the inner `if` and nothing
// else. A trailing statement after the inner `if` (e.g. the body an `or`-within-`and`
// guard leaves unguarded) means the inner `if` does NOT gate the whole then-branch, so the
// fold must not fire — that case is left untouched. Runs after ShortCircuitFolder so value
// diamonds (which carry an else / a following redeclaration) are already collapsed and are
// never of this shape.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"

#include <memory>
#include <vector>

class GuardChainCollapser : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (auto &stmt : stmts) {
            auto outer = std::dynamic_pointer_cast<IfStatementNode>(stmt);
            if (!outer) {
                continue;
            }

            while (CollapseOnce(outer)) {
                // keep folding `if A and B then [if C then ...]` deeper into the chain.
            }
        }
    }

  private:
    // Fold one level: `outer` must be else-less with a then-branch of exactly one else-less
    // `if`. On success `outer` is mutated to `if (A) and (B) then <inner then> end` and true
    // is returned. Precedence parens are added by the source generator.
    static bool CollapseOnce(const std::shared_ptr<IfStatementNode> &outer) {
        if (!outer->condition || !outer->thenBranch || HasElse(outer)) {
            return false;
        }
        if (outer->thenBranch->body.size() != 1) {
            return false;
        }

        auto inner = std::dynamic_pointer_cast<IfStatementNode>(outer->thenBranch->body.front());
        if (!inner || !inner->condition || !inner->thenBranch || HasElse(inner)) {
            return false;
        }

        outer->condition = std::make_shared<BinaryExpressionNode>("and", outer->condition, inner->condition);
        outer->thenBranch = inner->thenBranch;
        return true;
    }

    // a non-null else with statements blocks the fold (an empty else container is equivalent
    // to none, so it does not).
    static bool HasElse(const std::shared_ptr<IfStatementNode> &ifS) { return ifS->elseBranch && !ifS->elseBranch->body.empty(); }
};
