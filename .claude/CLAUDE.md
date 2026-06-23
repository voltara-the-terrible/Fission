# CLAUDE.md

Guidance for working in **Fission**, a from-scratch Luau decompiler (the decompiler backend for the closed-source project *RbxCli*). C++23, CMake, Windows-first.

## What Fission does

Takes compiled Luau bytecode (v6) — either Luau-compiled test code or encrypted Roblox client bytecode — and reconstructs readable Luau source, with optional type inference and variable-name inference.

## Build & test

The project uses CMake + CPM (dependencies are fetched at configure time: **Luau 0.718**, **Boost 1.86.0**, **libassert 2.2.0**, **Catch2 3.14.0** — the first configure is slow). C++23. Compile with **clang (GNU frontend)** or **MSVC**; `clang-cl` (clang with the MSVC frontend) is explicitly rejected in `CMakeLists.txt`.

```sh
# Configure (an MSVC multi-config build tree already lives in ./cmake-build)
cmake -B cmake-build -S .

# Build a target (multi-config => pass --config)
cmake --build cmake-build --config Debug --target Fission.Tests
cmake --build cmake-build --config Debug --target Fission.Executor

# Run the test suite (Catch2, registered via catch_discover_tests)
ctest --test-dir cmake-build -C Debug --output-on-failure
# ...or run the Catch2 binary directly to use tag filters, e.g. "[Decompiler][Types]"
```

Notable CMake switches at the top of `CMakeLists.txt`: `PRODUCTION_BUILD` (strips internal strings/error text to avoid leaking internals), `ENABLE_ASAN`/`ENABLE_UBSAN`, `USE_LTO`, PGO toggles. `BUILD_TESTING` gates the `Fission.Tests` target.

## Targets (see `CMakeLists.txt`)

- **Fission.Common** (static lib) — low-level bytecode primitives: `InstructionDecoder` (and `RobloxClientDecoder`, which un-shuffles Roblox's opcode encoding), `BinaryReader`. Lives in `namespace Fission`.
- **Fission.Decompiler** (static lib) — the entire pipeline. This is where almost all real work happens.
- **Fission.CLI** (executable) — local dev harness. Reads `test.txt` from the working dir, dumps IR/SSA, and shells out to `graph_generator.bat` (needs Graphviz `dot`). **Windows-only** (`<Windows.h>`, crypt32 for base64). Not the production entry point.
- **Fission.Executor** (executable, output name `FissionDecompiler`) — the RbxCli integration surface. With a file argument it decompiles to `<stem>_decompiled.luau`; with no arguments it starts a WebSocket server on port **7219**. Lives in `namespace Fission::Executor`.
- **Fission.Tests** (Catch2) — regression + semantics tests.

`Samples/` and `Samples/StressTests/` hold `.lua`/`.luau` inputs used as regression fodder (`Samples/StressTests/run_all.ps1` runs them all).

## The pipeline (`Decompiler::CommonDecompilerEntryImpl`)

`Decompiler.cpp` is the spine — read it first. Order:

1. **Deserialize** bytecode → `Deserializer`
2. **Lift to IR** → `BytecodeLifter` produces `LiftedFunction`s of `LiftedInstruction`s
3. **Control-flow analysis** → `ControlFlowAnalyzer`: `DetermineBasicBlocks` → `OptimizeGraph` → `IdentifyStructures` (loops/conditionals) → `PruneUnreachable`; dominators via `DenominatorAnalysis`
4. **SSA** → `SSABuilder`
5. **IR(SSA) → AST** → `ASTLifter` yields an `ASTFunction` of `ASTNode`s
6. **AST rewriting** → ordered `ASTRewriter` passes: `ShortCircuitFolder`, `IfChainSimplifier`, `MoveCoalescer`, `DeadLocalEliminator`, `NilGuardFieldNamer`
7. **Optional IR optimization** (`OptimizeIR` flag) — constant-folds dead `if`/`while`
8. **Type inference** (`InferTypes` flag, in `Decompiler.cpp`) and **Roblox type/name inference** (`InferRobloxTypes`/`AutoNameVariables` → `RobloxTypeInferer`)
9. **Source generation** → `SourceGenerator::GenerateSource` walks the AST via the Visitor pattern from a `RootNode`

Behavior is driven by `DecompilerFlags` (bitflag enum in `DecompilerFlags.hpp`, with `|`/`&`/`~` operators defined).

### Two important mechanisms

- **Safety boundary**: `CommonDecompilerEntry` installs a `ScopedThrowingAssertHandler` (`SafetyGuard.hpp`) and wraps the pipeline in try/catch, so a failed assertion or exception on malformed/hostile bytecode becomes `DecompileResult::FailedToDecompile` instead of aborting. Keep new pipeline code exception-safe and rely on `ASSERT`/`DEBUG_ASSERT` (libassert) for invariants.
- **Luau FFlags**: experimental Luau features (e.g. native `integer`) require enabling all `Luau*` fflags before compiling. The CLI and tests do this (`flag->value = true` loop over `Luau::FValue<bool>::list`). Mirror that in any new entry point.

### AST rewriters

Structural post-passes subclass `ASTRewriter` (`Rewriters/ASTRewriter.hpp`) and implement `RewriteStatements`. The base walks every block (including closures, loop/if bodies, reconstructed `do…end` scopes) **post-order** and hands each owning `std::vector<std::shared_ptr<Statement>>&` to the subclass — the Visitor pattern can't restructure a parent's statement list, which is why rewriters take the vector directly. Add new structural passes the same way and slot them into the ordered run in `Decompiler.cpp`.

## Conventions

### Naming

- **Types** (class/struct/enum/enum class/alias): `PascalCase` — `Decompiler`, `DecompilationResult`, `LiftedFunction`, `TypeFact`, `FunctionMap`.
- **AST nodes**: concrete statement/expression classes take a `Node` suffix — `BinaryExpressionNode`, `IfStatementNode`, `VariableDeclarationNode`, `StringLiteralNode`. Abstract bases drop it: `ASTNode`, `Statement`, `Expression`, `Declaration`, `Identifier`. The kind enum is `ASTNodeKind`.
- **Functions & methods**: `PascalCase` — `DecompileTestCode`, `DetermineBasicBlocks`, `GenerateSource`, `Run`, `Accept`/`Visit`. Free helpers too (`FormatIntList`, `InferExpressionType`). *(A couple of legacy lowercase helpers — `readfile`, `writefile` — exist; don't propagate that style.)*
- **Locals & parameters**: `camelCase` — `indentationLevel`, `deserializedBytecode`, `controlFlowAnalyzedFunction`.
- **Member variables**: `camelCase`, no `m_` prefix — `nodeKind`, `argumentCount`, `statements`, `deserializer`.
- **Enum members**: `PascalCase` — `Success`, `FailedToDeserialize`, `ImmediateNil`.
- **Globals**: `g_` prefix (`g_cliHandlerInit`). **Local constants**: `SCREAMING_SNAKE_CASE` (`MULTIPLICATION_MAGIC`).
- **Legacy Win32-flavored prefixes** still appear on many fields — keep using them when editing code that already does, for consistency: `b` for booleans (`bIsVarArg`, `bOmitInformationalComments`, `bType`), `lp` ("long pointer") for owning/raw pointers (`lpFunctionBody`, `lpLoopBody`, `lpHead`, `lpTail`), `dw` ("dword") for some integer IDs (`dwBlockId`).
- **Namespaces**: `Fission.Common` and the executor use `namespace Fission` / `Fission::Executor`; the decompiler core (AST nodes, `Decompiler`, lifters) is largely in the **global namespace**. Follow the file you're in.
- **Files**: `PascalCase.hpp` / `PascalCase.cpp`; headers under `<Target>/include/`, sources under `<Target>/src/`, with mirrored subfolders (`Analysis/`, `Rewriters/`, `AbstractSyntaxTree/Nodes/`, `SourceGenerator/`). Guard with `#pragma once`. Each file opens with a `// Created by <Author> on <date>.` banner.

### Indentation & formatting

**Indentation is 6 spaces** in the core decompiler sources (e.g. `Decompiler.cpp` function bodies) — match the surrounding lines when editing those. This is **mid-migration**: `.clang-format` declares `IndentWidth: 4`, and newer code and most headers (e.g. `ASTNode.hpp`, `ASTRewriter.hpp`, the executor/CLI mains) already follow the 4-space style. Rule of thumb: **Change to 6 space Indentation where possible**. Use tabs nowhere (`UseTab: Never`). Column limit is 160.

**Brace style** (`BreakBeforeBraces: Stroustrup`): opening braces always go on the **same line** as the function/control-flow keyword; `else`/`else if`/`catch` go on a **new line** after the closing `}`:

```cpp
void Method() {
    if (x) {
        // ...
    }
    else if (b) {
        // ...
    }
    else {
        // ...
    }
}
```

**Always use braces** — `InsertBraces: true` in `.clang-format` (requires clang-format 15+) enforces this automatically for control-flow bodies. Never write a braceless `if`, `else`, `for`, or `while`, even for single-statement bodies.

The active `.clang-format` settings relevant to braces and style:

```yaml
IndentWidth: 6
UseTab: Never
ColumnLimit: 160
BreakBeforeBraces: Stroustrup
InsertBraces: true
```

**Use guard clauses** to reduce nesting — prefer early `return`/`continue` over wrapping the happy path in an `if` body:

```cpp
// Preferred
for (auto& node : nodes) {
    if (!node) {
        continue;
    }

    Process(node);
}

std::string GetName(Symbol* symbol) {
    if (!symbol) {
        return {};
    }

    return symbol->name;
}

// Avoid
for (auto& node : nodes) {
    if (node) {
        Process(node);
    }
}

std::string GetName(Symbol* symbol) {
    if (symbol) {
        return symbol->name;
    }

    return {};
}
```

There are `.clang-format` and `.clang-tidy` files at the repo root; prefer running clang-format on lines you touch rather than reformatting wholesale.

## Gotchas

- The CLI reads a hard-coded `test.txt` in the working directory; root-level scratch files like `test.txt`, `__text.txt`, `bytecode_encoded.txt` are dev inputs, not source.
- `Fission.CLI` and parts of the executor are Windows-specific (`<Windows.h>`, `Sleep`, crypt32). Cross-platform work belongs in `Fission.Decompiler`/`Fission.Common`, which stay portable.
- Roblox bytecode is opcode-shuffled; decode it through `RobloxClientDecoder`, not the base `InstructionDecoder` (which is the identity decoder used for plain Luau-compiled bytecode).