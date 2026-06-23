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

// Mini viewmodel controller. Compiled with debugLevel=1 (no local-variable names), so every
// local below is stripped to a register and the decompiler must re-derive the names. Exercises:
//   GetService / WaitForChild / require    -> service / child / module name
//   field reads (a.Field)                  -> field name  (LocalPlayer->Player, CurrentCamera->Camera, script.Parent->scriptParent)
//   :GetAttribute("X")                     -> X
//   :Clone() / :Connect(fn)                -> clone / connection
//   if-then-else expression, type inference, an upvalue captured by the Heartbeat closure.
const std::string script = R"(

local Players = game:GetService("Players")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")

local player = Players.LocalPlayer       -- -> Player
local camera = workspace.CurrentCamera   -- -> Camera
local container = script.Parent          -- -> scriptParent

local Modules = ReplicatedStorage:WaitForChild("Modules")
local ToolInfo = require(Modules:WaitForChild("ToolInfo"))
local NumberUtil = require(Modules:WaitForChild("NumberUtil"))

local Humanoid = container:WaitForChild("Humanoid")
local recoil = Vector3.new(0, 0, 0)
local a = recoil and Humanoid or ToolInfo
-- Each field-read local below is used 2+ times so it survives inlining and surfaces its name.
local function equip(weapon)
    local handle = weapon.Handle             -- -> Handle
    local config = weapon.Configuration      -- -> Configuration
    local damage = config.Damage             -- -> Damage

    local model = handle:Clone()             -- -> clone
    model.Parent = camera
    model.Name = a

    local equipped = weapon:GetAttribute("Equipped")  -- -> Equipped
    if equipped then
        Humanoid.WalkSpeed = damage * 2
    else
        Humanoid.WalkSpeed = damage
    end
    model:SetAttribute("Active", equipped)

    print(config.Range, ToolInfo[handle.Name])
    return model
end

local connection = RunService.Heartbeat:Connect(function(dt)  -- -> connection
    recoil = recoil * (1 - dt)
    camera.CFrame = camera.CFrame * CFrame.new(recoil.X, recoil.Y, recoil.Z)
end)

for index = 1, 5 do
    local slot = container:WaitForChild("Slot")               -- -> Slot
    slot.Visible = index % 2 == 0
    print(NumberUtil.format(index), slot.Name, player.Name, recoil)
end

connection:Disconnect()
print(player.DisplayName, camera.ViewportSize, Humanoid.Health, equip)

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
        s, DecompilerFlags::InferRobloxTypes | DecompilerFlags::InferTypes | DecompilerFlags::AutoNameVariables | DecompilerFlags::UseIfElseExpressions
    );

    std::cout << decompileResult.decompilationOutput << std::endl;
}
