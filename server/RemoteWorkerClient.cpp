#include "RemoteWorkerClient.h"
#include "../common/SecureChannel.h"
#include "../common/transport/TcpTransport.h"
#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tsh {
namespace {

class Fd {
public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd &operator=(Fd &&other) noexcept {
    if (this != &other) {
      reset(other.fd_);
      other.fd_ = -1;
    }
    return *this;
  }
  int get() const { return fd_; }
  int release() {
    const int out = fd_;
    fd_ = -1;
    return out;
  }
  void reset(int fd = -1) {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = fd;
  }

private:
  int fd_;
};

struct AddrInfoDeleter {
  void operator()(addrinfo *info) const {
    if (info) {
      freeaddrinfo(info);
    }
  }
};

std::string gai_message(int rc) {
  return rc == EAI_SYSTEM ? std::strerror(errno) : gai_strerror(rc);
}

Fd connect_worker_socket(const RemoteNode &node, std::string *error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo *raw_results = nullptr;
  const std::string port = std::to_string(node.port);
  const int rc =
      getaddrinfo(node.host.c_str(), port.c_str(), &hints, &raw_results);
  std::unique_ptr<addrinfo, AddrInfoDeleter> results(raw_results);
  if (rc != 0) {
    *error = "[Error] Could not resolve node " + node.host + ": " +
             gai_message(rc) + ".";
    return Fd();
  }

  std::string last_error = "no address returned";
  for (addrinfo *ai = results.get(); ai != nullptr; ai = ai->ai_next) {
    Fd fd(
        socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol));
    if (fd.get() < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    if (connect(fd.get(), ai->ai_addr, ai->ai_addrlen) == 0) {
      return fd;
    }
    last_error = std::strerror(errno);
  }

  *error = "[Error] Connection to node " + node.host +
           " failed: " + last_error + ".";
  return Fd();
}

} // namespace

std::string RemoteWorkerClient::execute_node_stream(
    const std::vector<uint8_t> &serialized_ast, const RemoteNode &node,
    const std::function<void(const std::string &, bool)> &on_chunk) {
  try {
    std::string connect_error;
    Fd sockfd = connect_worker_socket(node, &connect_error);
    if (sockfd.get() < 0) {
      return connect_error;
    }

    auto transport = std::make_unique<TcpTransport>(sockfd.release());
    SecureChannel channel(std::move(transport));
    channel.set_peer_identity(worker_node_id(node));

    if (!channel.handshake(SecureChannel::Mode::CLIENT)) {
      return "[Error] Secure handshake with " + worker_node_id(node) +
             " failed.";
    }

    if (!channel.send_message(serialized_ast,
                              SecureChannel::MsgType::COMMAND)) {
      return "[Error] Failed to send job to " + worker_node_id(node) + ".";
    }

    std::string final_status;
    while (true) {
      std::vector<uint8_t> response_vec;
      SecureChannel::MsgType type;
      if (!channel.receive_message(response_vec, type)) {
        return final_status.empty()
                   ? "[Error] Worker connection closed before completion."
                   : final_status;
      }

      const std::string payload(response_vec.begin(), response_vec.end());
      if (type == SecureChannel::MsgType::COMMAND) {
        on_chunk(payload, false);
      } else if (type == SecureChannel::MsgType::WASM_PAYLOAD) {
        on_chunk(payload, true);
      } else if (type == SecureChannel::MsgType::AUDIT_LOG) {
        if (payload.rfind("OK", 0) == 0)
          return "";
        if (payload.rfind("ERR ", 0) == 0)
          return payload.substr(4);
        final_status = payload;
      }
    }
  } catch (...) {
    return "[Error] Distributed worker client exception.";
  }
}

} // namespace tsh
