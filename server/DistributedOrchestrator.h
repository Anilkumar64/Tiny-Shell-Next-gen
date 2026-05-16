#pragma once
#include "../common/SecureChannel.h"
#include "../common/transport/TcpTransport.h"
#include "AstSerializer.h"
#include "WorkerTypes.h"
#include <cstring>
#include <functional>
#include <iostream>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace tsh {
class DistributedOrchestrator {
public:
  // Distributes a pipeline to a list of workers and collects results
  static std::vector<std::string>
  fan_out(std::shared_ptr<AstNode> ast, const std::vector<RemoteNode> &nodes) {
    // FIX[SCALE-3]: Avoid unbounded background thread creation in the
    // distributed prototype.
    std::vector<std::string> results;
    auto serialized_ast = AstSerializer::serialize(ast);

    for (const auto &node : nodes) {
      results.push_back(execute_node(serialized_ast, node));
    }
    return results;
  }

  static std::string node_id(const RemoteNode &node) {
    return worker_node_id(node);
  }

  static std::string execute_node(const std::vector<uint8_t> &serialized_ast,
                                  const RemoteNode &node) {
    std::string output;
    const auto result = execute_node_stream(
        serialized_ast, node,
        [&output](const std::string &chunk, bool is_stderr) {
          if (is_stderr)
            output += "[stderr] ";
          output += chunk;
        });
    return result.empty() ? output : result;
  }

  static std::string execute_node_stream(
      const std::vector<uint8_t> &serialized_ast, const RemoteNode &node,
      const std::function<void(const std::string &, bool)> &on_chunk) {
    try {
      addrinfo hints{};
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_family = AF_UNSPEC;

      addrinfo *resolved = nullptr;
      const std::string port = std::to_string(node.port);
      const int gai =
          getaddrinfo(node.host.c_str(), port.c_str(), &hints, &resolved);
      if (gai != 0) {
        return "[Error] Could not resolve node " + node.host + ": " +
               gai_strerror(gai) + ".";
      }

      int sockfd = -1;
      for (addrinfo *rp = resolved; rp != nullptr; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) {
          continue;
        }
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
          break;
        }
        close(sockfd);
        sockfd = -1;
      }
      freeaddrinfo(resolved);
      if (sockfd < 0) {
        return "[Error] Connection to node " + node.host + " failed.";
      }

      auto transport = std::make_unique<TcpTransport>(sockfd);
      SecureChannel sc(std::move(transport));
      sc.set_peer_identity(node.host + ":" + std::to_string(node.port));

      if (!sc.handshake(SecureChannel::Mode::CLIENT)) {
        return "[Error] Handshake with " + node.host + " failed.";
      }

      if (!sc.send_message(serialized_ast, SecureChannel::MsgType::COMMAND)) {
        return "[Error] Failed to send job to " + node.host + ".";
      }

      std::string final_status;
      while (true) {
        std::vector<uint8_t> response_vec;
        SecureChannel::MsgType type;
        if (!sc.receive_message(response_vec, type)) {
          return final_status.empty()
                     ? "[Error] Worker connection closed before completion."
                     : final_status;
        }
        const std::string payload(response_vec.begin(), response_vec.end());
        if (type == SecureChannel::MsgType::COMMAND) {
          on_chunk(payload, false);
        } else if (type == SecureChannel::MsgType::WASM_PAYLOAD) {
          on_chunk(payload, true);
        } else if (type == SecureChannel::MsgType::CONTROL ||
                   type == SecureChannel::MsgType::AUDIT_LOG) {
          if (payload.rfind("OK", 0) == 0) {
            return "";
          }
          if (payload.rfind("ERR ", 0) == 0) {
            return payload.substr(4);
          }
          final_status = payload;
        }
      }
    } catch (...) {
      return "[Error] Distributed node exception.";
    }
  }
};
} // namespace tsh
