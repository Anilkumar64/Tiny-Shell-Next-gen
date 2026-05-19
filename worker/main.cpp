#include "../common/SecureChannel.h"
#include "../common/transport/TcpTransport.h"
#include "../server/AstSerializer.h"
#include "../server/ExecutionTimeout.h"
#include "../server/Pipeline.h"
#include "../server/PipelineValidator.h"
#include "../server/TaintTracker.h"
#include "../server/ThreadPool.h"
#include "../server/UnitValidator.h"
#include "../spine/common/JobSigner.h"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> g_running{true};
std::atomic<int> g_server_fd{-1};

void handle_signal(int) {
  g_running.store(false);
  const int fd = g_server_fd.exchange(-1);
  if (fd >= 0)
    close(fd);
}

int env_int(const char *name, int fallback) {
  if (const char *value = std::getenv(name)) {
    try {
      size_t consumed = 0;
      const int parsed = std::stoi(value, &consumed, 10);
      if (consumed == std::strlen(value) && parsed > 0) {
        return parsed;
      }
    } catch (...) {
    }
    std::cerr << "[TinyShell Worker] ignoring invalid " << name << "=" << value
              << "\n";
  }
  return fallback;
}

std::string env_string(const char *name, std::string fallback) {
  if (const char *value = std::getenv(name)) {
    return value;
  }
  return fallback;
}

void send_text(SecureChannel &channel, const std::string &text,
               SecureChannel::MsgType type) {
  const std::vector<uint8_t> bytes(text.begin(), text.end());
  channel.send_message(bytes, type);
}

void send_stdout_chunks(SecureChannel &channel, const std::string &output) {
  constexpr std::size_t chunk_size = 2048;
  for (std::size_t offset = 0; offset < output.size(); offset += chunk_size) {
    send_text(channel, output.substr(offset, chunk_size),
              SecureChannel::MsgType::COMMAND);
  }
}

void handle_job(int client_fd) {
  try {
    auto transport = std::make_unique<TcpTransport>(client_fd);
    SecureChannel channel(std::move(transport));
    if (!channel.handshake(SecureChannel::Mode::SERVER)) {
      return;
    }
    const auto signing_key = env_string("TSH_JOB_SIGNING_KEY", "");
    if (signing_key.empty()) {
      send_text(channel, "ERR worker has no signing key configured",
                SecureChannel::MsgType::AUDIT_LOG);
      return;
    }
    tsh::spine::JobSigner signer("worker", signing_key);

    try {
      std::vector<uint8_t> payload;
      SecureChannel::MsgType type;
      if (!channel.receive_message(payload, type) ||
          type != SecureChannel::MsgType::COMMAND) {
        send_text(channel, "ERR invalid job payload",
                  SecureChannel::MsgType::AUDIT_LOG);
        return;
      }

      tinyshell::v1::SignedJobSpec signed_spec;
      if (!signed_spec.ParseFromArray(payload.data(), payload.size()) ||
          !signer.verify(signed_spec)) {
        send_text(channel, "ERR invalid job signature",
                  SecureChannel::MsgType::AUDIT_LOG);
        return;
      }

      const auto &cmd = signed_spec.spec().command();
      auto ast = tsh::AstSerializer::deserialize(
          std::vector<uint8_t>(cmd.begin(), cmd.end()));
      tsh::PipelineValidator validator;
      validator.validate(ast);
      tsh::TaintTracker::enforce_data_flow(ast);
      tsh::UnitValidator::validate_units(ast);

      const auto started = std::chrono::steady_clock::now();
      auto output = tsh::ExecutionGuard::execute_with_timeout(
          std::chrono::milliseconds(env_int("TSH_WORKER_TIMEOUT_MS", 3000)),
          [&]() -> std::string { return tsh::Executor::execute(ast); });
      if (!output.empty() && output.back() != '\n') {
        output.push_back('\n');
      }

      send_stdout_chunks(channel, output);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started)
              .count();
      send_text(channel, "OK duration_ms=" + std::to_string(elapsed),
                SecureChannel::MsgType::AUDIT_LOG);
    } catch (const std::exception &e) {
      send_text(channel, std::string("ERR ") + e.what(),
                SecureChannel::MsgType::AUDIT_LOG);
    }
  } catch (const std::exception &e) {
    std::cerr << "[TinyShell Worker] Job failed: " << e.what() << "\n";
  }
}
} // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  const auto bind_addr = env_string("TSH_WORKER_BIND_ADDR", "127.0.0.1");
  const int port = env_int("TSH_WORKER_PORT", 5555);

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "[TinyShell Worker] socket() failed: " << std::strerror(errno)
              << "\n";
    return 1;
  }
  g_server_fd.store(server_fd);

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  if (inet_pton(AF_INET, bind_addr.c_str(), &address.sin_addr) != 1) {
    std::cerr << "[TinyShell Worker] invalid bind address: " << bind_addr
              << "\n";
    close(server_fd);
    return 1;
  }
  address.sin_port = htons(port);

  if (bind(server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
      0) {
    std::cerr << "[TinyShell Worker] bind() failed on " << bind_addr << ':'
              << port << ": " << std::strerror(errno) << "\n";
    close(server_fd);
    return 1;
  }
  if (listen(server_fd, 32) < 0) {
    std::cerr << "[TinyShell Worker] listen() failed: " << std::strerror(errno)
              << "\n";
    close(server_fd);
    return 1;
  }

  std::cout << "[TinyShell Worker] Listening on " << bind_addr << ':' << port
            << "\n";
  tsh::ThreadPool pool(
      static_cast<std::size_t>(env_int("TSH_WORKER_THREADS", 16)));
  while (g_running.load()) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    const int client_fd = accept(
        server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0) {
      if (!g_running.load())
        break;
      if (errno == EINTR)
        continue;
      std::cerr << "[TinyShell Worker] accept() failed: "
                << std::strerror(errno) << "\n";
      continue;
    }
    if (!pool.submit([client_fd]() { handle_job(client_fd); })) {
      close(client_fd);
      std::cerr
          << "[TinyShell Worker] rejected connection: worker queue full\n";
    }
  }

  pool.shutdown();
  if (g_server_fd.exchange(-1) >= 0)
    close(server_fd);
  std::cout << "[TinyShell Worker] Shutdown complete.\n";
  return 0;
}
