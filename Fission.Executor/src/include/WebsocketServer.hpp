#pragma once

#include "Decompiler.hpp"
#include "libassert/assert.hpp"
#include "luacode.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <wincrypt.h>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <boost/throw_exception.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <atomic>

#pragma comment(lib, "crypt32.lib")

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace Fission {
    namespace Executor {
            class WebsocketServer {
            private:

                  static std::string Base64Decode(const std::string &base64) {
                        std::string clean;
                        clean.reserve(base64.size());

                        for (unsigned char c : base64) {

                              if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
                                    clean.push_back(static_cast<char>(c));
                              }
                        }

                        while (clean.size() % 4 != 0) {
                              clean.push_back('=');
                        }

                        if (clean.empty()) {
                              return {};
                        }
                        
                        
                        DWORD decodedLength = 0;
                        if (!CryptStringToBinaryA(clean.c_str(), static_cast<DWORD>(clean.size()), CRYPT_STRING_BASE64, nullptr, &decodedLength, nullptr, nullptr)) {
                              throw std::runtime_error("Base64 size decode failed");
                        }

                        std::vector<BYTE> decoded(decodedLength);
                        if (!CryptStringToBinaryA(clean.c_str(), static_cast<DWORD>(clean.size()), CRYPT_STRING_BASE64, decoded.data(), &decodedLength, nullptr, nullptr)) {
                              throw std::runtime_error("Base64 decode failed");
                        }

                        decoded.resize(decodedLength);
                        return std::string(reinterpret_cast<const char *>(decoded.data()), decoded.size());
                  }

                  static std::optional<DecompilerFlags> FlagFromName(std::string_view name) {
                        static const std::unordered_map<std::string_view, DecompilerFlags> kMap = {
                              {"InferRobloxTypes", DecompilerFlags::InferRobloxTypes},
                              {"InferTypes", DecompilerFlags::InferTypes},
                              {"DebugInfo", DecompilerFlags::DebugInfo},
                              {"FunctionInfo", DecompilerFlags::FunctionInfo},
                              {"AutoNameVariables", DecompilerFlags::AutoNameVariables},
                              {"OmitFissionComments", DecompilerFlags::OmitFissionComments},
                              {"Upvalues", DecompilerFlags::Upvalues},
                              {"MinifyUpvalues", DecompilerFlags::MinifyUpvalues},
                              {"Constants", DecompilerFlags::Constants},
                              {"MinifyConstants", DecompilerFlags::MinifyConstants},
                              {"Globals", DecompilerFlags::Globals},
                              {"MinifyGlobals", DecompilerFlags::MinifyGlobals},
                              {"Protos", DecompilerFlags::Protos},
                              {"MinifyProtos", DecompilerFlags::MinifyProtos},
                              {"Semicolons", DecompilerFlags::Semicolons},
                              {"CallLineInfo", DecompilerFlags::CallLineInfo}
                        };
                        auto it = kMap.find(name);
                        return it != kMap.end() ? std::optional<DecompilerFlags>(it->second) : std::nullopt;
                  }

                  static DecompilerFlags ParseFlags(const boost::json::value &flags) {
                        if (flags.is_int64()) {
                              return static_cast<DecompilerFlags>(static_cast<uint32_t>(flags.as_int64()));
                        }
                        if (flags.is_uint64()) {
                              return static_cast<DecompilerFlags>(static_cast<uint32_t>(flags.as_uint64()));
                        }

                        DecompilerFlags result = DecompilerFlags::None;

                        if (flags.is_array()) {
                              for (const auto &v : flags.as_array()) {
                                    if (v.is_string()) {
                                          if (auto f = FlagFromName(v.as_string().c_str())) {
                                                result |= *f;
                                          }
                                    }
                              }
                                    
                        } else if (flags.is_object()) {
                              
                              for (const auto &kv : flags.as_object()) {
                                    if (kv.value().is_bool() && kv.value().as_bool()) {
                                          if (auto f = FlagFromName(kv.key())) {
                                                result |= *f;
                                          }
                                    }
                              }
                        }
                        return result;
                  }

                  static std::string DecompileFromBytecode(const std::string &bytecode, DecompilerFlags flags, const std::string &mode) {
                        try {
                              Decompiler decompiler;
                              auto result = decompiler.DecompileRobloxBytecode(bytecode, flags);

                              if (result.resultCode != DecompileResult::Success) {
                                    return "-- Fission: decompilation failed (code " + std::to_string(static_cast<int>(result.resultCode)) + ")";
                              }
                              return mode == "IR" ? result.irOutput : result.decompilationOutput;
                        }
                        catch (const std::exception &e) {
                              return std::string("-- Fission: error: ") + e.what();
                        }
                        catch (...) {
                              return "-- Fission: unknown error";
                        }
                  }

                  static std::string DecompileWithTimeout(std::string bytecode, DecompilerFlags flags, std::string mode, unsigned int timeout_seconds = 60) {

                        auto promise = std::make_shared<std::promise<std::string>>();
                        auto future = promise->get_future();
                        auto cancel_flag = std::make_shared<std::atomic<bool>>(false);

                        auto thread_handle = std::make_shared<std::thread>([promise, cancel_flag, bytecode = std::move(bytecode), flags, mode = std::move(mode)]() mutable {
                              try {
                                    if (!cancel_flag->load()) {
                                          promise->set_value(DecompileFromBytecode(bytecode, flags, mode));
                                    }
                              }
                              catch (...) {
                                    try {
                                          if (!cancel_flag->load())
                                                promise->set_value("-- Fission: unknown error");
                                          }
                                    catch (...) { }
                              }
                        });

                        if (future.wait_for(std::chrono::seconds(timeout_seconds)) == std::future_status::timeout) {
                              cancel_flag->store(true);
                              return "-- Fission: decompilation timed out (" + std::to_string(timeout_seconds) + "s)";
                        }
                        
                        if (thread_handle->joinable())
                              thread_handle->join();
                        
                        return future.get();
                        }

                  static std::string HandleRequest(const std::string &message) {
                        std::string guid;
                        std::string result;
                        try {
                              const auto parsed = boost::json::parse(message);
                              const auto &obj = parsed.as_object();

                              if (auto it = obj.find("guid"); it != obj.end() && it->value().is_string()) {
                                    guid = it->value().as_string().c_str();
                              }

                              std::string base64;
                              if (auto it = obj.find("bytecode"); it != obj.end() && it->value().is_string()) {
                                    base64 = it->value().as_string().c_str();
                              }

                              std::string mode = "Luau";
                              if (auto it = obj.find("mode"); it != obj.end() && it->value().is_string())
                                    mode = it->value().as_string().c_str();

                              unsigned int timeout = 60;
                              if (auto it = obj.find("timeout"); it != obj.end() && it->value().is_uint64())
                              timeout = static_cast<unsigned int>(it->value().as_uint64());

                              DecompilerFlags flags = DecompilerFlags::None;

                              if (auto it = obj.find("flags"); it != obj.end() && !it->value().is_null()) {
                                    const DecompilerFlags requested = ParseFlags(it->value());
                                    if (requested != DecompilerFlags::None) {
                                          flags = requested;
                                    }
                              }

                              if (base64.empty()) {
                                    result = "-- Fission: missing bytecode";
                              }
                              else {
                                    result = DecompileWithTimeout(Base64Decode(base64), flags, mode, timeout);
                              }

                        }
                        catch (const std::exception &e) {
                              result = std::string("-- Fission: error: ") + e.what();
                        }
                        catch (...) {
                              result = "-- Fission: unknown error";
                        }

                        boost::json::object reply;
                        reply["guid"] = guid;
                        reply["result"] = result;
                        return boost::json::serialize(reply);
                  }

                  static void session(tcp::socket socket) {
                        try {
                              websocket::stream<tcp::socket> ws{std::move(socket)};
                              ws.accept();

                              while (true) {
                                    beast::flat_buffer buffer;
                                    ws.read(buffer);

                                    std::string input = beast::buffers_to_string(buffer.data());
                                    if (input.size() > 32 * 1024 * 1024) {
                                    ws.text(true);
                                    ws.write(net::buffer(std::string(R"({"guid":"","result":"-- Fission: payload too large"})")));
                                    continue;
                                    }

                                    const std::string reply = HandleRequest(input);
                                    ws.text(true);
                                    ws.write(net::buffer(reply));
                              }

                        }
                        catch (const beast::system_error &se) {
                              if (se.code() != websocket::error::closed)
                                    std::cerr << "session closed: " << se.code().message() << std::endl;
                        } 
                        catch (const std::exception &e) {
                              std::cerr << "session error: " << e.what() << std::endl;
                        };
                  };

                  public:
                        static void Start(unsigned short port = 7219) {
                        try {
                              net::io_context ioc{1};
                              tcp::acceptor acceptor{ioc, tcp::endpoint{tcp::v4(), port}};

                              std::cout << "Fission server running on ws://localhost:" << port << std::endl;

                              Sleep(1000);

                              FreeConsole();

                              for (;;) {
                                    tcp::socket socket{ioc};
                                    acceptor.accept(socket);
                                    std::thread(session, std::move(socket)).detach();
                              }
                        }
                        catch (const std::exception &e) {
                              std::cerr << "error: " << e.what() << std::endl;
                        };
                  };
            };

      };
};
