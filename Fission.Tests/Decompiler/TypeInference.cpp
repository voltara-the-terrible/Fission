//
// Created by Dottik on 2/6/2026.
//
// Type- and name-inference tests (run with InferTypes | InferRobloxTypes).
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

    std::string DecompileTyped(const std::string &source, int optLevel = 2) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 1;
        const auto flags = DecompilerFlags::InferTypes | DecompilerFlags::InferRobloxTypes | DecompilerFlags::AutoNameVariables;
        auto result = decompiler.DecompileTestCode(source, flags, opts);
        REQUIRE(result.resultCode == DecompileResult::Success);
        return std::move(result.decompilationOutput);
    }

    bool ContainsRegex(const std::string &hay, const std::regex &re) { return std::regex_search(hay, re); }

} // namespace

// A type annotation must never be a generated register name (`: v2`, `: uv_0`,
// `: arg1`). `setmetatable(x, <auto-local>)` used to leak the metatable register
// as the declared type, producing uncompilable Luau.
TEST_CASE("Types: setmetatable with an auto-named metatable falls back to table", "[Decompiler][Types]") {
    const auto out = DecompileTyped(R"(
        local function make(mt)
            local self = setmetatable({}, mt)
            return self
        end
        return make
    )");
    INFO("decompile:\n" << out);
    // No annotation may be a generated identifier.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*v\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*arg\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*uv_\d+\b)")));
}

// Whole-program sanity: a class-style module must not emit any generated-name
// type annotation anywhere.
TEST_CASE("Types: class module emits no register-named annotations", "[Decompiler][Types]") {
    const auto out = DecompileTyped(R"(
        local Klass = {}
        Klass.__index = Klass
        function Klass.new()
            local self = setmetatable({}, Klass)
            self.values = {1, 2, 3}
            self.name = "k"
            return self
        end
        function Klass.get(self)
            return self.values
        end
        return Klass
    )");
    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*v\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*uv_\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*arg\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*i_\d+\b)")));
}

// A number-typed local that survives optimisation gets `: number`.
// (Simple literals are inlined at -O2, so keep one alive via accumulation.)
TEST_CASE("Types: surviving numeric local is annotated number", "[Decompiler][Types]") {
    const auto out = DecompileTyped(R"(
        local function f(n)
            local total = 0
            for i = 1, n do
                total = total + i
            end
            return total
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(:\s*number\b)")));
    // Still no register-named annotations.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*v\d+\b)")));
}

// =========================================================================
// Name inference
// =========================================================================

// A multi-use `:WaitForChild("X")` / `:GetService("X")` result is named after
// the child/service instead of `v1`.
TEST_CASE("Names: WaitForChild/GetService results are named after the child", "[Decompiler][Names]") {
    const auto out = DecompileTyped(R"(
        local function f(parent)
            local thing = parent:WaitForChild("MyThing")
            thing.A = 1
            thing.B = 2
            local svc = game:GetService("RunService")
            svc.X = 1
            svc.Y = 2
            return thing, svc
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bMyThing\b\s*[:=])")));
    CHECK(ContainsRegex(out, std::regex(R"(\bRunService\b\s*[:=])")));
}

// `require(path:WaitForChild("Module"))` is named after the module. The child
// string constant is inlined into the call (single-version pure constants inline
// even as call arguments), so the require-naming heuristic can see it.
TEST_CASE("Names: require of a WaitForChild is named after the module", "[Decompiler][Names]") {
    const auto out = DecompileTyped(R"(
        local function f(parent)
            local mod = require(parent:WaitForChild("Helper"))
            mod.A = 1
            mod.B = 2
            return mod
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bHelper\b\s*[:=])")));
}

// A plain field read (`x.PrimaryPart`) names the local after the field, so a member
// access carries its name into the decompiled source instead of a register name.
TEST_CASE("Names: a field read is named after the field", "[Decompiler][Names]") {
    const auto out = DecompileTyped(R"(
        local function f(model)
            local part = model.PrimaryPart
            part.Anchored = true
            part.CanCollide = false
            return part
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bPrimaryPart\b\s*[:=])")));
}

// `:GetAttribute("Name")` names its result after the attribute, mirroring the
// child-lookup heuristic.
TEST_CASE("Names: GetAttribute result is named after the attribute", "[Decompiler][Names]") {
    const auto out = DecompileTyped(R"(
        local function f(inst)
            local dir = inst:GetAttribute("Direction")
            local a = dir .. "x"
            local b = dir .. "y"
            return a, b
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bDirection\b\s*[:=])")));
}

// `:Clone()` carries no naming argument, so the result is named after the method.
TEST_CASE("Names: Clone result is named clone", "[Decompiler][Names]") {
    const auto out = DecompileTyped(R"(
        local function f(template)
            local copy = template:Clone()
            copy.A = 1
            copy.B = 2
            return copy
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bclone\b\s*[:=])")));
}

// `workspace.CurrentCamera` is a near-universal idiom and reads as `Camera`.
TEST_CASE("Names: CurrentCamera field read is named Camera", "[Decompiler][Names]") {
    const auto out = DecompileTyped(R"(
        local function f()
            local cam = workspace.CurrentCamera
            cam.FieldOfView = 80
            cam.CameraType = 1
            return cam
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bCamera\b\s*[:=])")));
}
