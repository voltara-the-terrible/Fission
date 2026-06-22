#pragma once
#include "DecompilerFlags.hpp"
#include <array>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

namespace Fission {
      namespace Executor {
            
            constexpr auto kCliFlags = std::to_array<std::pair<std::string_view, DecompilerFlags>>({

                  {"print-ir", DecompilerFlags::PrintIR},
                  {"write-ir-to-file", DecompilerFlags::WriteIRToFile},
                  {"generate-ir-graph", DecompilerFlags::GenerateIRGraph},
                  {"generate-ssa-ir-graph", DecompilerFlags::GenerateSSAIRGraph},
                  {"print-timing-breakdown", DecompilerFlags::PrintTimingBreakdown},
                  {"infer-types", DecompilerFlags::InferTypes},
                  {"optimize-ir", DecompilerFlags::OptimizeIR},
                  {"infer-roblox-types", DecompilerFlags::InferRobloxTypes},
                  {"auto-name-variables", DecompilerFlags::AutoNameVariables},
                  {"omit-fission-comments", DecompilerFlags::OmitFissionComments},
                  {"upvalues", DecompilerFlags::Upvalues},
                  {"minify-upvalues", DecompilerFlags::MinifyUpvalues},
                  {"debug-info", DecompilerFlags::DebugInfo},
                  {"function-info", DecompilerFlags::FunctionInfo},
                  {"constants", DecompilerFlags::Constants},
                  {"minify-constants", DecompilerFlags::MinifyConstants},
                  {"globals", DecompilerFlags::Globals},
                  {"minify-globals", DecompilerFlags::MinifyGlobals},
                  {"protos", DecompilerFlags::Protos},
                  {"minify-protos", DecompilerFlags::MinifyProtos},
                  {"semicolons", DecompilerFlags::Semicolons},
                  {"call-line-info", DecompilerFlags::CallLineInfo}

            });

            inline std::optional<DecompilerFlags> FlagFromName(std::string_view name) {
                  for (const auto &[candidate, value] : kCliFlags)
                  if (candidate == name)
                        return value;
                  return std::nullopt;
            };

            inline void PrintUsage(std::ostream &os, const char *exe) {
                  os << "usage: " << exe << " <bytecode-file> [--flag ...]\n\navailable flags:\n";
                  for (const auto &entry : kCliFlags)
                  os << "  --" << entry.first << "\n";
            };

            inline std::optional<DecompilerFlags> ParseFlags(int argc, char *argv[], int start) {
                  DecompilerFlags flags = DecompilerFlags::None;
                  for (int i = start; i < argc; ++i) {
                  std::string_view name = argv[i];
                  while (!name.empty() && name.front() == '-')
                        name.remove_prefix(1);

                  const auto parsed = FlagFromName(name);
                  if (!parsed) {
                        std::cerr << "unknown flag: " << argv[i] << std::endl;
                        return std::nullopt;
                  };
                  flags |= *parsed;
                  };
                  return flags;
            };
      };
};
