//
// Created by yeno on 21/6/2026.
//
// Names a `local vN = t and t.Field` (the nil-guard field-read idiom) after the field it
// guards, mirroring the plain `local Field = t.Field` naming the lifter already applies.
//
// The short-circuit fold runs on generic register names (it matches statements by name), so
// this naming has to happen *after* folding: naming the register during lifting disrupts the
// fold and desyncs the phi. Here the fold has already collapsed the idiom into a single
// coherent local, so we find a freshly-declared generic temp whose initializer reduces (through
// `and`-chains) to a field read, then rename every reference to that local within its scope.
//
// Guards keep the string rename sound:
//   - the field name must be a valid identifier and not a Luau keyword;
//   - it must not already be a live variable in scope (no shadowing/collision);
//   - the temp must not be re-declared elsewhere in scope (one binding only);
//   - the temp must not be captured by a nested closure (a closure has its own `vN` register
//     namespace, so a textual rename can't safely cross into it).
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"

#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class NilGuardFieldNamer : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (size_t i = 0; i < stmts.size(); ++i) {
            auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
            if (!decl || !decl->value)
                continue;

            const auto oldName = IdentNameOf(decl->identifier);
            if (!oldName || !IsGenericTemp(*oldName))
                continue;

            auto key = GuardedFieldKey(decl->value);
            if (!key)
                continue;
            const std::string newName = key->value;
            if (newName == *oldName || !IsValidIdent(newName) || IsKeyword(newName))
                continue;

            // The local's scope is its declaration onward within this block (nested blocks included).
            std::vector<std::shared_ptr<Statement>> scope(stmts.begin() + static_cast<std::ptrdiff_t>(i), stmts.end());

            bool collides = false, captured = false;
            int declCount = 0;
            ForEachIdentifier(scope, /*enterClosures=*/true, [&](IdentifierExpressionNode *id, bool inClosure) {
                if (!id->identifier)
                    return;
                if (id->identifier->name == newName)
                    collides = true; // field name already names a live variable/global here
                if (inClosure && id->identifier->name == *oldName)
                    captured = true; // captured by a closure with its own register namespace
            });
            CountDeclarations(scope, *oldName, declCount);
            if (collides || captured || declCount != 1)
                continue;

            ForEachIdentifier(scope, /*enterClosures=*/false, [&](IdentifierExpressionNode *id, bool) {
                if (id->identifier && id->identifier->name == *oldName)
                    id->identifier->name = newName;
            });
        }
    }

  private:
    using IdentFn = std::function<void(IdentifierExpressionNode *, bool)>;

    static std::optional<std::string> IdentNameOf(const std::shared_ptr<Expression> &expr) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); id && id->identifier)
            return id->identifier->name;
        if (auto id = std::dynamic_pointer_cast<Identifier>(expr))
            return id->name;
        return std::nullopt;
    }

    // A lifter-generated temporary name: `v` followed by one or more digits (e.g. `v3`).
    static bool IsGenericTemp(const std::string &s) {
        if (s.size() < 2 || s[0] != 'v')
            return false;
        for (size_t i = 1; i < s.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[i])))
                return false;
        return true;
    }

    // The field key of a nil-guard initializer: `t.Key`, or the right arm of an `and`-chain
    // (`a and a.Key`, `a and a.b and a.b.Key`). Returns null when the value is not a field read.
    static std::shared_ptr<StringLiteralNode> GuardedFieldKey(const std::shared_ptr<Expression> &expr) {
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr))
            return std::dynamic_pointer_cast<StringLiteralNode>(mem->key);
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr); bin && bin->op == "and")
            return GuardedFieldKey(bin->right);
        return nullptr;
    }

    static bool IsValidIdent(const std::string &s) {
        if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])))
            return false;
        for (char c : s)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                return false;
        return true;
    }

    static bool IsKeyword(const std::string &s) {
        static const std::unordered_set<std::string> kKeywords{"and",   "break", "do",    "else", "elseif", "end",   "false",  "for",
                                                               "function", "if",  "in",    "local", "nil",   "not",   "or",     "repeat",
                                                               "return",   "then", "true",  "until", "while", "continue"};
        return kKeywords.contains(s);
    }

    // Visit every IdentifierExpressionNode reachable from `stmts`, reporting whether each is inside
    // a nested closure body. Closures are entered only when `enterClosures` is set.
    static void ForEachIdentifier(const std::vector<std::shared_ptr<Statement>> &stmts, bool enterClosures, const IdentFn &fn) {
        for (const auto &s : stmts)
            WalkNode(s, false, enterClosures, fn);
    }

    static void WalkBody(const std::shared_ptr<BlockStatementNode> &block, bool inClosure, bool enterClosures, const IdentFn &fn) {
        if (block)
            for (const auto &s : block->body)
                WalkNode(s, inClosure, enterClosures, fn);
    }

    static void WalkNode(const std::shared_ptr<Statement> &node, bool inClosure, bool enterClosures, const IdentFn &fn) {
        if (!node)
            return;

        // Expressions (Expression derives from Statement, so a node may be either).
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(node)) {
            fn(id.get(), inClosure);
            return;
        }
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(node)) {
            WalkNode(mem->table, inClosure, enterClosures, fn);
            WalkNode(mem->key, inClosure, enterClosures, fn);
            return;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(node)) {
            WalkNode(idx->left, inClosure, enterClosures, fn);
            WalkNode(idx->right, inClosure, enterClosures, fn);
            return;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(node)) { // also TableBinaryExpressionNode
            WalkNode(bin->left, inClosure, enterClosures, fn);
            WalkNode(bin->right, inClosure, enterClosures, fn);
            return;
        }
        if (auto cmp = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(node)) {
            WalkNode(cmp->left, inClosure, enterClosures, fn);
            WalkNode(cmp->right, inClosure, enterClosures, fn);
            return;
        }
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(node)) {
            WalkNode(un->operand, inClosure, enterClosures, fn);
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(node)) {
            WalkNode(call->callee, inClosure, enterClosures, fn);
            for (const auto &a : call->arguments)
                WalkNode(a, inClosure, enterClosures, fn);
            for (const auto &r : call->rets)
                WalkNode(r, inClosure, enterClosures, fn);
            return;
        }
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(node)) {
            WalkNode(nc->calledOn, inClosure, enterClosures, fn);
            WalkNode(nc->callWhat, inClosure, enterClosures, fn);
            for (const auto &a : nc->arguments)
                WalkNode(a, inClosure, enterClosures, fn);
            for (const auto &r : nc->rets)
                WalkNode(r, inClosure, enterClosures, fn);
            return;
        }
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(node)) {
            for (const auto &e : tbl->expressions)
                WalkNode(e, inClosure, enterClosures, fn);
            return;
        }
        if (auto fn2 = std::dynamic_pointer_cast<FunctionDeclarationNode>(node)) {
            if (enterClosures)
                WalkBody(fn2->lpFunctionBody, /*inClosure=*/true, enterClosures, fn);
            return;
        }

        // Statements.
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(node)) {
            WalkNode(decl->identifier, inClosure, enterClosures, fn);
            WalkNode(decl->value, inClosure, enterClosures, fn);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(node)) {
            WalkNode(asn->left, inClosure, enterClosures, fn);
            WalkNode(asn->right, inClosure, enterClosures, fn);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(node)) {
            WalkNode(es->expression, inClosure, enterClosures, fn);
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(node)) {
            for (const auto &v : ret->returnValues)
                WalkNode(v, inClosure, enterClosures, fn);
            return;
        }
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(node)) {
            WalkNode(ifS->condition, inClosure, enterClosures, fn);
            WalkBody(ifS->thenBranch, inClosure, enterClosures, fn);
            WalkBody(ifS->elseBranch, inClosure, enterClosures, fn);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(node)) {
            WalkNode(w->condition, inClosure, enterClosures, fn);
            WalkBody(w->body, inClosure, enterClosures, fn);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(node)) {
            WalkNode(r->condition, inClosure, enterClosures, fn);
            WalkBody(r->body, inClosure, enterClosures, fn);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(node)) {
            WalkNode(fnum->loopVariable, inClosure, enterClosures, fn);
            WalkNode(fnum->startVariable, inClosure, enterClosures, fn);
            WalkNode(fnum->increaseBy, inClosure, enterClosures, fn);
            WalkNode(fnum->maxIncreased, inClosure, enterClosures, fn);
            WalkBody(fnum->lpLoopBody, inClosure, enterClosures, fn);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(node)) {
            for (const auto &v : fgen->loopVariables)
                WalkNode(v, inClosure, enterClosures, fn);
            WalkNode(fgen->generator, inClosure, enterClosures, fn);
            WalkNode(fgen->state, inClosure, enterClosures, fn);
            WalkNode(fgen->index, inClosure, enterClosures, fn);
            WalkBody(fgen->body, inClosure, enterClosures, fn);
            return;
        }
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(node)) {
            WalkBody(blk, inClosure, enterClosures, fn);
            return;
        }
        if (auto arg = std::dynamic_pointer_cast<FunctionArgumentExpression>(node)) {
            WalkNode(arg->argumentName, inClosure, enterClosures, fn);
            return;
        }
        // Literals, VarArg, Nil, NoExpression, Break/Continue/Comment, Vector: no identifiers.
    }

    // Count places that *bind* `name` (declaration, loop variable). Descends everywhere, including
    // closures, so any re-binding aborts the rename. Our own `local name = ...` counts as one.
    static void CountDeclarations(const std::vector<std::shared_ptr<Statement>> &stmts, const std::string &name, int &count) {
        for (const auto &s : stmts)
            CountDeclarationsNode(s, name, count);
    }

    static void CountDeclarationsBody(const std::shared_ptr<BlockStatementNode> &block, const std::string &name, int &count) {
        if (block)
            CountDeclarations(block->body, name, count);
    }

    static void CountDeclarationsNode(const std::shared_ptr<Statement> &node, const std::string &name, int &count) {
        if (!node)
            return;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(node)) {
            if (IdentNameOf(decl->identifier).value_or("") == name)
                ++count;
            return;
        }
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(node)) {
            CountDeclarationsBody(ifS->thenBranch, name, count);
            CountDeclarationsBody(ifS->elseBranch, name, count);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(node)) {
            CountDeclarationsBody(w->body, name, count);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(node)) {
            CountDeclarationsBody(r->body, name, count);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(node)) {
            if (IdentNameOf(fnum->loopVariable).value_or("") == name)
                ++count;
            CountDeclarationsBody(fnum->lpLoopBody, name, count);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(node)) {
            for (const auto &v : fgen->loopVariables)
                if (IdentNameOf(v).value_or("") == name)
                    ++count;
            CountDeclarationsBody(fgen->body, name, count);
            return;
        }
        if (auto fn2 = std::dynamic_pointer_cast<FunctionDeclarationNode>(node)) {
            CountDeclarationsBody(fn2->lpFunctionBody, name, count);
            return;
        }
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(node)) {
            CountDeclarationsBody(blk, name, count);
            return;
        }
    }
};
