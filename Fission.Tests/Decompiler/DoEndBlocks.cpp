//
// do...end scope detection. The compiler frees a block's locals at `end`, so the register-stack
// top resets and the registers are reused afterwards; Fission reconstructs those scopes from that
// reuse signal. These tests pin the detected wrapping and guard against false positives.
//

#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <regex>
#include <set>
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

    // debugLevel 2 emits locvars (exact scopes); debugLevel 1 omits them, exercising the register-
    // reuse heuristic that runs on stripped (real Roblox) bytecode.
    std::string DecompileOrFail(const std::string &source, DecompilerFlags flags = static_cast<DecompilerFlags>(0), int debugLevel = 2, int optLevel = 1) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = debugLevel;
        auto result = decompiler.DecompileTestCode(source, flags, opts);
        REQUIRE(result.resultCode == DecompileResult::Success);
        return std::move(result.decompilationOutput);
    }

    // counts lines that are exactly a `do` block opener (excludes `for/while ... do`).
    size_t CountDoBlocks(const std::string &out) {
        std::regex opener(R"((?:^|\n)[ \t]*do[ \t]*(?:\n|$))");
        return static_cast<size_t>(std::distance(std::sregex_iterator(out.begin(), out.end(), opener), std::sregex_iterator()));
    }

    bool HasDoBlock(const std::string &out) { return CountDoBlocks(out) > 0; }

} // namespace

// Two sibling `do ... end` blocks. Their locals (kept live by multiple uses, fed by a non-constant
// so the optimiser cannot fold them away) reuse the same register, which is the detection signal.
TEST_CASE("Lift: sibling do...end blocks are reconstructed from register reuse", "[Decompiler][DoEnd]") {
    const auto out = DecompileOrFail(R"(
        local hh = math.random(1, 100)
        do
            local a = hh + 1
            print(a)
            print(a * 2)
        end
        do
            local c = hh + 3
            print(c)
            print(c * 4)
        end
        print(hh)
    )");

    INFO("decompile:\n" << out);
    CHECK(CountDoBlocks(out) == 2);
    // each block re-declares its own scoped local (not a bare reassignment that would leak a global).
    CHECK(std::regex_search(out, std::regex(R"(do[\s\S]*?local v\d+ = [\s\S]*?end)")));
}

// do...end blocks that follow a value-materialization (the merge block has live-in registers like
// `hh` / `a`) must still be detected — the detector accounts for live-ins and dead-register gaps.
TEST_CASE("Lift: do...end blocks after a value materialization are detected", "[Decompiler][DoEnd]") {
    const auto out = DecompileOrFail(R"(
        local hh = math.random(1, 100)
        local a = hh and 1 or 6
        do
            local c = hh + 3
            print(c)
            print(c * 4)
        end
        do
            local c = hh + 5
            print(c)
            print(c * 6)
        end
        print(hh)
        print(a)
    )");

    INFO("decompile:\n" << out);
    CHECK(CountDoBlocks(out) == 2);
}

// Sibling do-blocks reuse the same register for their locals, so register-based naming would give
// them the same `vN`. The renamer must make each scope's local distinct (and non-shadowing).
TEST_CASE("Lift: sibling do-block locals get distinct names", "[Decompiler][DoEnd]") {
    const auto out = DecompileOrFail(R"(
        local hh = math.random(1, 100)
        do
            local c = hh + 1
            print(c)
            print(c * 2)
        end
        do
            local c = hh + 3
            print(c)
            print(c * 4)
        end
        print(hh)
    )");

    INFO("decompile:\n" << out);
    CHECK(CountDoBlocks(out) == 2);
    // every `local vN` in this script (no functions) should be a unique name.
    std::set<std::string> names;
    size_t count = 0;
    const std::regex declRe(R"(local (v\d+))");
    for (auto it = std::sregex_iterator(out.begin(), out.end(), declRe); it != std::sregex_iterator(); ++it) {
        names.insert((*it)[1].str());
        ++count;
    }
    CHECK(count >= 3); // hh + the two do-block locals
    CHECK(names.size() == count);
}

// A do-block with several locals collapses to a single `do ... end` (not one per local), and the
// base register being reused by the next block is what confirms the scope.
TEST_CASE("Lift: a do-block with multiple locals is one block", "[Decompiler][DoEnd]") {
    const auto out = DecompileOrFail(R"(
        local hh = math.random(1, 100)
        do
            local x = hh + 1
            local y = hh + 2
            print(x + y)
            print(x * y)
        end
        do
            local x = hh + 3
            local y = hh + 4
            print(x + y)
            print(x * y)
        end
        print(hh)
    )");

    INFO("decompile:\n" << out);
    CHECK(CountDoBlocks(out) == 2);
}

// Three consecutive do-blocks reusing the same registers are all detected and all distinctly named.
TEST_CASE("Lift: three consecutive do-blocks are all detected", "[Decompiler][DoEnd]") {
    const auto out = DecompileOrFail(R"(
        local hh = math.random(1, 100)
        do local c = hh + 1 print(c) print(c * 2) end
        do local c = hh + 3 print(c) print(c * 4) end
        do local c = hh + 5 print(c) print(c * 6) end
        print(hh)
    )");

    INFO("decompile:\n" << out);
    CHECK(CountDoBlocks(out) == 3);
}

// A do-block that declares no local of its own (it only reassigns an outer local) leaves no register
// signal — its bytecode is identical to having no `do ... end`, so it is intentionally not recovered.
TEST_CASE("Lift: a do-block that only reassigns an outer local is not recoverable", "[Decompiler][DoEnd]") {
    const auto out = DecompileOrFail(R"(
        local a = math.random(1, 100)
        do
            a = a + 1
            print(a)
            print(a * 2)
        end
        print(a)
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(HasDoBlock(out));
}

// The same statements without `do ... end`: both locals are simultaneously in scope, so the
// compiler assigns them distinct registers — no reuse, no scope. Must not be wrapped.
TEST_CASE("Lift: sequential locals without do...end are not wrapped", "[Decompiler][DoEnd]") {
    const auto out = DecompileOrFail(R"(
        local hh = math.random(1, 100)
        local a = hh + 1
        print(a)
        print(a * 2)
        local c = hh + 3
        print(c)
        print(c * 4)
        print(hh)
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(HasDoBlock(out));
}

// With debug info (debugLevel >= 2) do...end scopes come from `locvars`, which is exact: it detects a
// block-scoped local even when its register is never reused afterwards (the register heuristic would
// miss this, since there is no "jump back" to confirm it).
TEST_CASE("Lift: locvars detect a do-block whose local is never reused", "[Decompiler][DoEnd]") {
    const auto out = DecompileOrFail(R"(
        local hh = math.random(1, 100)
        do
            local x = hh + 1
            print(x)
            print(x * 2)
        end
        return hh
    )");

    INFO("decompile:\n" << out);
    CHECK(CountDoBlocks(out) == 1);
}

// On stripped bytecode (no locvars, debugLevel 1) the register heuristic must still tell the two
// apart: sibling `do local c ... end` blocks reuse one register (detected), whereas two top-level
// `local c` coexist in different registers (not detected). This is the with/end-vs-without contrast.
TEST_CASE("Lift: register heuristic distinguishes do-end from plain locals (stripped bytecode)", "[Decompiler][DoEnd]") {
    const char *withDo = R"(
        local hh = math.random(1, 100)
        do local c = hh + 3 print(c) print(c * 4) end
        do local c = hh + 3 print(c) print(c * 4) end
        print(hh)
    )";
    const char *withoutDo = R"(
        local hh = math.random(1, 100)
        local c = hh + 3 print(c) print(c * 4)
        local c = hh + 3 print(c) print(c * 4)
        print(hh)
    )";

    const auto wrapped = DecompileOrFail(withDo, static_cast<DecompilerFlags>(0), /*debugLevel=*/1);
    const auto flat = DecompileOrFail(withoutDo, static_cast<DecompilerFlags>(0), /*debugLevel=*/1);
    INFO("with do/end:\n" << wrapped << "\nwithout:\n" << flat);
    CHECK(CountDoBlocks(wrapped) == 2);
    CHECK(CountDoBlocks(flat) == 0);
}

// A do-block that only reassigns an outer local leaves no register/scope signal, but the `do`/`end`
// lines leave gaps in the line info. With RecoverDoEndFromLineInfo (opt-in, needs line debug info)
// those are recovered; without it, they're correctly absent.
TEST_CASE("Lift: RecoverDoEndFromLineInfo recovers do-end blocks that only reassign an outer local", "[Decompiler][DoEnd]") {
    const char *src = R"(
local hh = math.random(1, 100)
local c = 1
do
    c = hh + 3
    print(c)
    print(c * 4)
end
do
    c = hh + 3
    print(c)
    print(c * 4)
end
print(hh)
)";

    const auto without = DecompileOrFail(src);
    const auto with = DecompileOrFail(src, DecompilerFlags::RecoverDoEndFromLineInfo);
    INFO("without flag:\n" << without << "\nwith flag:\n" << with);
    CHECK_FALSE(HasDoBlock(without)); // no local declared inside → invisible by default
    CHECK(CountDoBlocks(with) == 2);  // recovered from the do/end line gaps
}

// When the reassigned outer var is used after the blocks, it must be hoisted (declared before) so it
// stays in scope — not re-localised inside a block.
TEST_CASE("Lift: RecoverDoEndFromLineInfo hoists an outer var used after the do-end blocks", "[Decompiler][DoEnd]") {
    const char *src = R"(
local hh = math.random(1, 100)
local c = 1
do
    c = hh + 3
    print(c)
    print(c * 4)
end
do
    c = hh + 3
    print(c)
    print(c * 4)
end
print(hh, c)
)";

    const auto out = DecompileOrFail(src, DecompilerFlags::RecoverDoEndFromLineInfo);
    INFO("decompile:\n" << out);
    CHECK(CountDoBlocks(out) == 2);
    // the shared var is declared once before the first block (uninitialised), then assigned inside.
    CHECK(std::regex_search(out, std::regex(R"((?:^|\n)\s*local v\d+\s*\n)")));
    // and it is still in scope for the trailing use.
    CHECK(std::regex_search(out, std::regex(R"(print\(v\d+, v\d+\))")));
}

// Line info exists at debugLevel >= 1, so recovery works without locvars too (debugLevel 1 — what
// Roblox bytecode carries). It only fails at debugLevel 0, where there is no line info at all.
TEST_CASE("Lift: RecoverDoEndFromLineInfo works at debugLevel 1 (no locvars)", "[Decompiler][DoEnd]") {
    const char *src = R"(
local hh = math.random(1, 100)
local c = 1
do
    c = hh + 3
    print(c)
    print(c * 4)
end
do
    c = hh + 3
    print(c)
    print(c * 4)
end
print(hh, c)
)";

    const auto lvl1 = DecompileOrFail(src, DecompilerFlags::RecoverDoEndFromLineInfo, /*debugLevel=*/1);
    INFO("debugLevel 1:\n" << lvl1);
    CHECK(CountDoBlocks(lvl1) == 2);
}

// The DebugInfo flag tags each lifted statement with its source `Line/Register/OpCode`.
TEST_CASE("Lift: DebugInfo annotates statements with line/register/opcode", "[Decompiler][DebugInfo]") {
    const auto out = DecompileOrFail(
        R"(
        local hh = math.random(1, 100)
        print(hh)
    )",
        DecompilerFlags::DebugInfo
    );

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(--\[\[ Line: \d+, Register: R\d+-R\d+, OpCode: \w+ \]\])")));
}
