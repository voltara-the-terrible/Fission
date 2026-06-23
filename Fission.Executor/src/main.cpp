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

warn((function()
return 'a'
end))

do
    local c = hh + 3
    print(c)
    print(c * 4)
end

print(hh)

)";

int main() {
    Decompiler decompiler{};

    std::string s = "____";
    uintptr_t size = 0;
    auto sz = luau_compile(script.c_str(), script.size(), nullptr, &size);
    s.resize(size);
    memcpy(s.data(), sz, size);

    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;

    auto decompileResult = decompiler.DecompileLuauBytecode(
        s, DecompilerFlags::InferRobloxTypes | DecompilerFlags::InferTypes | DecompilerFlags::AutoNameVariables | DecompilerFlags::UseIfElseExpressions
    );

    std::cout << decompileResult.decompilationOutput << std::endl;
}