//
// Created by Claude on 22/6/2026.
//
// Undoes the tail duplication the lifter produces for return/terminator-merged diamonds.
//
// To fold value short-circuits (`x = a and b or c`), the lifter deliberately lets a
// return-terminated merge block be re-walked into every arm of an `if` (the Return-block
// exemption in LiftControlFlow). ShortCircuitFolder then collapses the small ones. But the
// SAME exemption duplicates a *large* shared tail (e.g. the whole fire/shoot body) into the
// paths of several nested terminator-merged diamonds — 3 levels → 2^3 = 8 copies — which
// nothing folds. This is the dominant output bloat.
//
// Two sound rewrites collapse it (run post-order, looping each level to a fixpoint, AFTER
// ShortCircuitFolder so value diamonds are already folded, and BEFORE the variable auto-namer
// so the duplicated copies still carry identical register-based names and render identically):
//
//   (1) if/else shared tail:  `if C then A; T else B; T end`  ==>  `if C then A else B end; T`
//       Sound for any T — whichever arm runs, T runs last, exactly as it would after the `if`.
//
//   (2) else-less terminator tail:  `if C then A; T end; T`   ==>  `if C then A end; T`
//       Requires the then-branch to END IN A TERMINATOR (return/break/continue) and the run T
//       it shares with the statements *following* the `if` to be that terminator-ending run.
//       Sound: T terminates, so running it inside the (taken) then-branch is the same as falling
//       through to the single copy after the `if`.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"
#include "SourceGenerator/Generator.hpp"

#include <memory>
#include <string>
#include <vector>

class BranchTailMerger : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        // loop this level to a fixpoint: a hoisted tail can expose another mergeable `if`.
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < stmts.size(); ++i) {
                auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmts[i]);
                if (!ifS || !ifS->thenBranch)
                    continue;
                if (TryMergeIfElseTail(stmts, i, ifS) || TryMergeTerminatorTail(stmts, i, ifS))
                    changed = true;
            }
        }
    }

  private:
    static bool IsTerminator(const std::shared_ptr<Statement> &s) {
        return std::dynamic_pointer_cast<ReturnStatementNode>(s) || std::dynamic_pointer_cast<BreakStatementNode>(s) ||
               std::dynamic_pointer_cast<ContinueStatementNode>(s);
    }

    static std::string RenderStatement(const std::shared_ptr<Statement> &s) {
        SourceGenerator g;
        if (s)
            s->Accept(&g);
        return g.buffer.str();
    }

    static std::vector<std::string> RenderEach(const std::vector<std::shared_ptr<Statement>> &body) {
        std::vector<std::string> out;
        out.reserve(body.size());
        for (const auto &s : body)
            out.push_back(RenderStatement(s));
        return out;
    }

    // (1) `if C then A; T else B; T end` -> `if C then A else B end; T`.
    static bool TryMergeIfElseTail(std::vector<std::shared_ptr<Statement>> &stmts, size_t i, const std::shared_ptr<IfStatementNode> &ifS) {
        if (!ifS->elseBranch)
            return false;
        auto &thenB = ifS->thenBranch->body;
        auto &elseB = ifS->elseBranch->body;
        if (thenB.empty() || elseB.empty())
            return false;

        const std::vector<std::string> thenR = RenderEach(thenB);
        const std::vector<std::string> elseR = RenderEach(elseB);
        size_t n = 0;
        while (n < thenB.size() && n < elseB.size() && thenR[thenR.size() - 1 - n] == elseR[elseR.size() - 1 - n])
            ++n;
        if (n == 0)
            return false;

        std::vector<std::shared_ptr<Statement>> suffix(thenB.end() - static_cast<std::ptrdiff_t>(n), thenB.end());
        thenB.erase(thenB.end() - static_cast<std::ptrdiff_t>(n), thenB.end());
        elseB.erase(elseB.end() - static_cast<std::ptrdiff_t>(n), elseB.end());
        stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1, suffix.begin(), suffix.end());
        return true;
    }

    // (2) `if C then A; T end; T` (no else, then-branch ends in a terminator, T ends with it)
    //     -> `if C then A end; T`.
    static bool TryMergeTerminatorTail(std::vector<std::shared_ptr<Statement>> &stmts, size_t i, const std::shared_ptr<IfStatementNode> &ifS) {
        if (ifS->elseBranch && !ifS->elseBranch->body.empty())
            return false;
        auto &thenB = ifS->thenBranch->body;
        if (thenB.empty() || !IsTerminator(thenB.back()))
            return false;

        // statements that follow the `if` at this level.
        const size_t after = i + 1;
        const size_t avail = stmts.size() - after;
        if (avail == 0)
            return false;

        const std::vector<std::string> thenR = RenderEach(thenB);
        std::vector<std::string> afterR;
        afterR.reserve(avail);
        for (size_t j = after; j < stmts.size(); ++j)
            afterR.push_back(RenderStatement(stmts[j]));

        // longest L such that the then-branch's last L statements (in order) equal the following
        // statements' first L (in order). thenB.back() is the terminator, so any match ends with it.
        const size_t maxL = std::min(thenB.size(), avail);
        size_t bestL = 0;
        for (size_t L = maxL; L >= 1; --L) {
            bool ok = true;
            for (size_t j = 0; j < L; ++j) {
                if (thenR[thenB.size() - L + j] != afterR[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                bestL = L;
                break;
            }
        }
        if (bestL == 0)
            return false;

        thenB.erase(thenB.end() - static_cast<std::ptrdiff_t>(bestL), thenB.end());
        return true;
    }
};
