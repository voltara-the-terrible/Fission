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

    std::string DecompileOrFail(const std::string &source, int optLevel = 1) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 2;
        auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
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
