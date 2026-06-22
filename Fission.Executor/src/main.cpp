#include "include/WebsocketServer.hpp"
#include "include/FlagHelper.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>


int main(int argc, char *argv[]) {
    Decompiler decompiler{};

    if (argc > 1) {
        
        const std::string_view first = argv[1];
        if (first == "-h" || first == "--help") {
            Fission::Executor::PrintUsage(std::cout, argv[0]);
            return EXIT_SUCCESS;
        };

        const std::filesystem::path path = argv[1];
        std::ifstream file(path, std::ios::binary);

        if (!file.is_open()) {
            std::cerr << "failed to open file: " << path << std::endl;
            return EXIT_FAILURE;
        };
        file.close();

        if (std::filesystem::file_size(path) == 0) {
            std::cerr << "file is empty: " << path << std::endl;
            return EXIT_FAILURE;
        };

        const auto flags = Fission::Executor::ParseFlags(argc, argv, 2);
        if (!flags) {
            Fission::Executor::PrintUsage(std::cerr, argv[0]);
            return EXIT_FAILURE;
        };

        auto decompileResult = decompiler.DecompileRobloxBytecodeFromFile(path.string(), *flags);

        if (decompileResult.resultCode != DecompileResult::Success) {
            std::cerr << "decompilation failed: " << static_cast<int>(decompileResult.resultCode) << std::endl;
            return EXIT_FAILURE;
        };

        std::ofstream out(path.stem().string() + "_decompiled.luau", std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "failed to create output file" << std::endl;
            return EXIT_FAILURE;
        };

        out << decompileResult.decompilationOutput;
        out.close();

        std::cout << "decompiled written to: " << (path.parent_path() / (path.stem().string() + "_decompiled.luau")) << std::endl;
        Sleep(1000);
        return EXIT_SUCCESS;
    };

    Fission::Executor::WebsocketServer::Start(7219);

    return EXIT_SUCCESS;
};
