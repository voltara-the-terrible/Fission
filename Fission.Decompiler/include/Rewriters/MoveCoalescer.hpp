//
// Created by yeno on 21/6/2026.
//
// Coalesces a single-use temporary into the copy that consumes it. Luau emits
// `CALL Rt; MOVE Rd <- Rt` (and similar register shuffles) as
// `local vT = <expr>; vD = vT`. When the temp `vT` is read exactly once — by that
// immediately-following copy — the temp is redundant, so we fold it into the copy
// and drop the temp: `vD = <expr>`. The copy's debug info (the MOVE) is kept.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"
#include "SourceGenerator/Generator.hpp"

#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class MoveCoalescer : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (size_t i = 0; i + 1 < stmts.size();) {
            if (TryCoalesceAt(stmts, i))
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

    // `local v = E`: a VariableDeclarationNode with a value, or a Call/NameCall whose
    // single return is a freshly-declared local v. Sets isCall when the value is a call.
    static bool AsLocalInit(const std::shared_ptr<Statement> &stmt, std::string &name, std::shared_ptr<Expression> &value, bool &isCall) {
        isCall = false;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            auto n = SimpleIdentName(decl->identifier);
            if (decl->value && n && !n->empty()) {
                name = *n;
                value = decl->value;
                return true;
            }
            return false;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            const auto retName = [](const std::vector<std::shared_ptr<Expression>> &rets) -> std::optional<std::string> {
                return rets.size() == 1 ? SimpleIdentName(rets[0]) : std::nullopt;
            };
            if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression); nc && nc->bIsLocalDeclaration)
                if (auto n = retName(nc->rets); n && !n->empty()) {
                    name = *n;
                    value = nc;
                    isCall = true;
                    return true;
                }
            if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(es->expression); c && c->bIsLocalDeclaration)
                if (auto n = retName(c->rets); n && !n->empty()) {
                    name = *n;
                    value = c;
                    isCall = true;
                    return true;
                }
        }
        return false;
    }

    // `T = v` or `local T = v`, where T is a simple identifier (a MOVE target) and the
    // right-hand side is exactly local v.
    static bool AsMoveFromVar(const std::shared_ptr<Statement> &stmt, const std::string &v, std::shared_ptr<Expression> &target, bool &targetIsLocal) {
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
            if (SimpleIdentName(asn->right).value_or("") == v && SimpleIdentName(asn->left)) {
                target = asn->left;
                targetIsLocal = false;
                return true;
            }
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
            if (decl->value && SimpleIdentName(decl->value).value_or("") == v && SimpleIdentName(decl->identifier)) {
                target = decl->identifier;
                targetIsLocal = true;
                return true;
            }
        return false;
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

    static std::string RenderStatement(const std::shared_ptr<Statement> &s) {
        SourceGenerator g;
        if (s)
            s->Accept(&g);
        return g.buffer.str();
    }

    // whole-word (identifier-boundary) occurrences of `name` in rendered text.
    static int CountName(const std::string &s, const std::string &name) {
        if (name.empty())
            return 0;
        const auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        int count = 0;
        for (size_t pos = s.find(name); pos != std::string::npos; pos = s.find(name, pos + name.size())) {
            const bool leftOk = pos == 0 || !isIdent(s[pos - 1]);
            const size_t end = pos + name.size();
            const bool rightOk = end >= s.size() || !isIdent(s[end]);
            if (leftOk && rightOk)
                ++count;
        }
        return count;
    }

    static bool TryCoalesceAt(std::vector<std::shared_ptr<Statement>> &stmts, size_t i) {
        // A: local v = E
        std::string v;
        std::shared_ptr<Expression> value;
        bool isCall = false;
        if (!AsLocalInit(stmts[i], v, value, isCall))
            return false;

        // B: T = v   (the immediately-following copy)
        std::shared_ptr<Expression> target;
        bool targetIsLocal = false;
        if (!AsMoveFromVar(stmts[i + 1], v, target, targetIsLocal))
            return false;
        if (SimpleIdentName(target).value_or("") == v) // degenerate self-move
            return false;

        // v must be read exactly once — declared in A, consumed in B, nowhere else
        // (descends into nested blocks via rendering). Reused register names → skip.
        int uses = 0;
        for (const auto &s : stmts) {
            uses += CountName(RenderStatement(s), v);
            if (uses > 2)
                return false;
        }
        if (uses != 2)
            return false;

        if (isCall)
            InlineifyValue(value);

        std::shared_ptr<Statement> folded;
        if (targetIsLocal)
            folded = std::make_shared<VariableDeclarationNode>(target, value); // local T = E
        else
            folded = std::make_shared<AssignmentStatementNode>(target, value); // T = E

        // carry the copy's (MOVE) debug info onto the coalesced statement.
        folded->debugLine = stmts[i + 1]->debugLine;
        folded->debugReg = stmts[i + 1]->debugReg;
        folded->debugOpCode = stmts[i + 1]->debugOpCode;

        stmts[i] = folded;
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        return true;
    }
};
