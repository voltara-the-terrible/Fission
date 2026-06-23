//
// Created by Dottik on 5/10/2025.
//

#include "Decompiler.hpp"
#include "libassert/assert.hpp"
#include "luacode.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#pragma comment(lib, "crypt32.lib")

const std::string script = R"(

local hh = math.random(1, 100)
local c = 1

do
    c += hh + 3
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

int main() {
    Decompiler decompiler{};

    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;

    std::string s = "____";
    uintptr_t size = 0;
    lua_CompileOptions opts{};
    //opts.optimizationLevel = 2;
    opts.debugLevel = 1; // line info (no locvars) — what Roblox bytecode carries; line-gap recovery works here
    auto sz = luau_compile(script.c_str(), script.size(), &opts, &size);
    s.resize(size);
    memcpy(s.data(), sz, size);

    auto decompileResult = decompiler.DecompileLuauBytecode(
        s, DecompilerFlags::InferRobloxTypes | DecompilerFlags::InferTypes | DecompilerFlags::AutoNameVariables | DecompilerFlags::UseIfElseExpressions |
               DecompilerFlags::RecoverDoEndFromLineInfo
    );

    std::cout << decompileResult.decompilationOutput << std::endl;
}
