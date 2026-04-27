#pragma once
#include "AstSerializer.h"
#include "SecureChannel.h"
#include "transport/TcpTransport.h"
#include <vector>
#include <string>
#include <future>
#include <iostream>
#include <netdb.h>

namespace tsh {
    struct RemoteNode {
        std::string host;
        int port;
    };

    class DistributedOrchestrator {
    public:
        // Distributes a pipeline to a list of workers and collects results
        static std::vector<std::string> fan_out(std::shared_ptr<AstNode> ast, const std::vector<RemoteNode>& nodes) {
            std::vector<std::future<std::string>> futures;
            auto serialized_ast = AstSerializer::serialize(ast);

            for (const auto& node : nodes) {
                futures.push_back(std::async(std::launch::async, [node, serialized_ast]() {
                    try {
                        // 1. Establish Secure Connection to Worker
                        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
                        struct hostent* server = gethostbyname(node.host.c_str());
                        struct sockaddr_in serv_addr;
                        std::memset(&serv_addr, 0, sizeof(serv_addr));
                        serv_addr.sin_family = AF_INET;
                        std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
                        serv_addr.sin_port = htons(node.port);

                        if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
                            return std::string("[Error] Connection to node " + node.host + " failed.");
                        }

                        auto transport = std::make_unique<TcpTransport>(sockfd);
                        SecureChannel sc(std::move(transport));

                        if (!sc.handshake(SecureChannel::Mode::CLIENT)) {
                            return std::string("[Error] Handshake with " + node.host + " failed.");
                        }

                        // 2. Send Serialized AST (Feature: Distributed Execution)
                        sc.send_message(serialized_ast, SecureChannel::MsgType::COMMAND);

                        // 3. Collect Results
                        std::vector<uint8_t> response_vec;
                        SecureChannel::MsgType type;
                        if (sc.receive_message(response_vec, type)) {
                            return std::string(response_vec.begin(), response_vec.end());
                        }
                    } catch (...) {
                        return std::string("[Error] Distributed node exception.");
                    }
                    return std::string("[Error] Unknown failure.");
                }));
            }

            std::vector<std::string> results;
            for (auto& f : futures) {
                results.push_back(f.get());
            }
            return results;
        }
    };
}
