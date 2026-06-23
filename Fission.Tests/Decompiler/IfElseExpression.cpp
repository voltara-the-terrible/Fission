//
// Luau `if <cond> then A else B` expression reconstruction. The compiler lowers the value-level
// if to a diamond whose branches assign one register and merge; Fission collapses that back into a
// single `local x = if cond then A else B` instead of a statement `if` (which, when the merge is a
// return block, also duplicated the merge code into both arms).
//

#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <regex>
#include <string>

namespace {

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    std::string DecompileOrFail(const std::string &source, DecompilerFlags flags = static_cast<DecompilerFlags>(0), int optLevel = 1) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 2;
        auto result = decompiler.DecompileTestCode(source, flags, opts);
        REQUIRE(result.resultCode == DecompileResult::Success);
        return std::move(result.decompilationOutput);
    }

    size_t CountOccurrences(const std::string &haystack, const std::string &needle) {
        size_t count = 0, pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    }

} // namespace

// A 2-way value diamond (`if Aa then 'a' else 'asf'`) lowers to one register written on both arms.
// By default it reads back as the short-circuit `Aa and "a" or "asf"` (the then-value is a truthy
// literal, so the and/or form is sound), and the trailing `warn(a)` is emitted exactly once.
TEST_CASE("Lift: 2-way value diamond defaults to a short-circuit expression", "[Decompiler][IfElseExpr]") {
    const auto out = DecompileOrFail(R"(
        local a = if Aa then 'a' else 'asf'
        warn(a)
    )");

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(=\s*Aa and "a" or "asf")")));
    CHECK(CountOccurrences(out, "warn(") == 1);
    CHECK_FALSE(std::regex_search(out, std::regex(R"((?:^|\n)\s*if\s)")));
}

// With UseIfElseExpressions, the same diamond reads back as the `if cond then A else B` expression.
TEST_CASE("Lift: UseIfElseExpressions renders a 2-way diamond as an if-else expression", "[Decompiler][IfElseExpr]") {
    const auto out = DecompileOrFail(
        R"(
        local a = if Aa then 'a' else 'asf'
        warn(a)
    )",
        DecompilerFlags::UseIfElseExpressions
    );

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(=\s*if Aa then "a" else "asf")")));
    CHECK(CountOccurrences(out, "warn(") == 1);
}

// A short-circuit `and`/`or` chain materialised into one register (3+ values sharing one merge) is,
// by default, reconstructed as the chained expression, not a nest of statement `if`s.
TEST_CASE("Lift: and/or short-circuit chain is reconstructed", "[Decompiler][IfElseExpr]") {
    const auto out = DecompileOrFail(R"(
        local a = g and 123 or ad and 12345 or 99
        warn(a)
    )");

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(=\s*g and 123 or ad and 12345 or 99)")));
    CHECK(CountOccurrences(out, "warn(") == 1);
    // no statement-level `if` should remain.
    CHECK_FALSE(std::regex_search(out, std::regex(R"((?:^|\n)\s*if\s)")));
}

// With UseIfElseExpressions, the chain reads back as an `if .. elseif .. else` expression instead.
TEST_CASE("Lift: UseIfElseExpressions renders a chain as if/elseif/else", "[Decompiler][IfElseExpr]") {
    const auto out = DecompileOrFail(
        R"(
        local a = if qwer then 67 elseif assd then 3 else 6
        warn(a)
    )",
        DecompilerFlags::UseIfElseExpressions
    );

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(=\s*if qwer then 67 elseif assd then 3 else 6)")));
    CHECK(CountOccurrences(out, "warn(") == 1);
}

// A genuine statement `if` whose branches do more than assign one register must NOT be collapsed.
TEST_CASE("Lift: multi-statement if branches are not collapsed to an expression", "[Decompiler][IfElseExpr]") {
    const auto out = DecompileOrFail(R"(
        local a
        if Aa then
            print("t")
            a = 1
        else
            print("f")
            a = 2
        end
        warn(a)
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(std::regex_search(out, std::regex(R"(=\s*if\s+.+\s+then)")));
    CHECK(CountOccurrences(out, "print(") == 2);
}

// `local a = r and B or C` whose result is captured by a closure lowers (at O0) to a 2-instruction
// arm (the `or`-recheck), so the lift-time detector bails and the recheck is then simplified away —
// leaving a clean `local a; if not r then a=C else a=B end` that no other pass revisited. The
// TwoWayValueDiamondFolder must fold it back into a single declaration.
TEST_CASE("Lift: a recheck-simplified 2-way diamond is folded on the AST", "[Decompiler][IfElseExpr]") {
    const auto out = DecompileOrFail(R"(
        local Humanoid = game
        local recoil = Vector3.new(0, 0, 0)
        local a = recoil and Humanoid or ToolInfo
        local function f()
            return a, recoil, Humanoid
        end
        for i = 1, 3 do
            warn(a, recoil, i)
        end
        return f
    )", DecompilerFlags::UseIfElseExpressions, 0);
    INFO("decompile:\n" << out);
    // Folded to a single declaration with an if-else expression, not a statement `if` that assigns
    // the same local in both arms.
    CHECK(std::regex_search(out, std::regex(R"(=\s*if\s+\w+\s+then\s+\w+\s+else\s+\w+)")));
    CHECK_FALSE(std::regex_search(out, std::regex(R"((?:^|\n)\s*if not \w+ then)")));
}
