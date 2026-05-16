#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace tsh {

struct Config {
  int server_port = 4444;
  int http_port = 8080;
  std::string server_bind_addr = "127.0.0.1";
  std::string api_bind_addr = "127.0.0.1";
  std::string api_token;
  std::string zk_secret;

  // Load configuration from environment variables.
  // Optional variables fall back to their defaults when unset.
  static Config load_from_env() {
    Config cfg;

    cfg.server_port = read_int("TSH_PORT", cfg.server_port, 1, 65535);
    cfg.http_port = read_int("TSH_API_PORT", cfg.http_port, 1, 65535);
    cfg.server_bind_addr = read_string("TSH_BIND_ADDR", cfg.server_bind_addr);
    cfg.api_bind_addr = read_string("TSH_API_BIND_ADDR", cfg.api_bind_addr);
    cfg.api_token = read_string("TSH_API_TOKEN", "");
    cfg.zk_secret = read_string("TSH_ZK_SECRET", "");

    return cfg;
  }

  static std::string read_string(const char *name,
                                 const std::string &fallback) {
    if (const char *file_var =
            std::getenv((std::string(name) + "_FILE").c_str()))
      return read_secret_file(name, file_var);
    if (const char *value = std::getenv(name))
      return value;
    return fallback;
  }

  static int read_int(const char *name, int fallback, int min_value,
                      int max_value) {
    const auto raw = read_string(name, "");
    if (raw.empty())
      return fallback;

    size_t consumed = 0;
    long parsed = 0;
    try {
      parsed = std::stol(raw, &consumed, 10);
    } catch (...) {
      throw std::runtime_error(std::string(name) +
                               " must be an integer, got: " + raw);
    }

    if (consumed != raw.size() || parsed < min_value || parsed > max_value) {
      throw std::runtime_error(std::string(name) + " must be in range " +
                               std::to_string(min_value) + ".." +
                               std::to_string(max_value));
    }
    return static_cast<int>(parsed);
  }

  static std::string read_secret_file(const char *name,
                                      const std::string &path) {
    std::ifstream in(path);
    if (!in) {
      throw std::runtime_error(std::string(name) +
                               "_FILE is not readable: " + path);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto value = buffer.str();
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                              value.back() == ' ' || value.back() == '\t')) {
      value.pop_back();
    }
    return value;
  }

  // Throw if required secrets are absent so the server never starts
  // with empty credentials.
  void require_server_secrets() const {
    if (api_token.empty())
      throw std::runtime_error("TSH_API_TOKEN is not set. "
                               "Set it before starting the server:\n"
                               "  export TSH_API_TOKEN=your-secret-token");
    if (api_token == "dev-token-1234") {
      // BUG: the shipped development bearer token could start production.
      // FIX: fail closed when the known default token is configured.
      throw std::runtime_error(
          "ERROR: Default dev token in use. Generate a real token.");
    }

    if (zk_secret.size() < 32)
      throw std::runtime_error(
          "TSH_ZK_SECRET is not set or is shorter than 32 bytes. "
          "Generate one with:\n"
          "  export TSH_ZK_SECRET=$(openssl rand -hex 32)");
  }
};

} // namespace tsh
