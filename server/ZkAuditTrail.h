#pragma once
#include "../config/tsh_config.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <vector>

namespace tsh {
struct ZkLogEntry {
  std::string timestamp;
  std::string user_id;
  std::string commitment; // HMAC(Secret, Command)
  std::string proof;      // SHA256(Commitment + PrevHash)
};

class ZkAuditTrail {
  std::string prev_hash; // BUG-17 FIX: seeded from ledger on startup, not
                         // hardcoded zeros
  std::string cluster_secret;
  std::string log_path;
  mutable std::mutex
      log_mutex_; // BUG-6 FIX: protect rotate_if_needed from concurrent callers

public:
  explicit ZkAuditTrail(std::string path = "tsh_zk_audit.ledger")
      : log_path(std::move(path)) {
    // FIX[CRIT-2]: Secret must be injected at runtime, never baked into binary.
    const auto raw = Config::load_from_env().zk_secret;
    if (raw.size() < 32) {
      throw std::runtime_error("TSH_ZK_SECRET must be set and at least 32 "
                               "bytes. Refusing to start.");
    }
    cluster_secret = raw;

    // BUG-17 FIX: Seed prev_hash from the last PROOF: field in the ledger so
    // the tamper-evidence chain survives server restarts.  Fall back to zeros
    // only when no ledger exists yet.
    prev_hash = load_last_proof();
  }

  // F-2: Generates a ZK-style commitment and sequence proof
  void log_secure_event(const std::string &user, const std::string &command) {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    std::tm tm_buf{};
    localtime_r(&in_time_t, &tm_buf);
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");

    std::lock_guard<std::mutex> lock(log_mutex_);
    rotate_if_needed(); // called inside the lock (BUG-6 FIX)
    unsigned char c_hash[SHA256_DIGEST_LENGTH];
    size_t c_len = sizeof(c_hash);
    hmac_sha256(command, c_hash, c_len);

    std::string commitment = to_hex(c_hash, c_len);

    // 2. Generate Sequence Proof: SHA256(Commitment + PrevHash)
    std::string combined = commitment + prev_hash;
    unsigned char p_hash[EVP_MAX_MD_SIZE];
    unsigned int p_len = 0;
    // BUG: SHA256() is deprecated and can fail on hardened OpenSSL builds.
    // FIX: use provider-backed EVP_Digest with EVP_sha256().
    if (EVP_Digest(combined.data(), combined.size(), p_hash, &p_len,
                   EVP_sha256(), nullptr) != 1) {
      throw std::runtime_error("Failed to generate audit proof hash");
    }

    std::string proof = to_hex(p_hash, p_len);
    prev_hash = proof;

    // 3. Persist to immutable-style ledger
    std::ofstream ledger(log_path, std::ios::app);
    ledger << ss.str() << " | USER:" << user << " | COMMITMENT:" << commitment
           << " | PROOF:" << proof << "\n";
    std::cout << "[ZK-Audit] F-2 Proof Generated: " << proof.substr(0, 12)
              << "...\n";
  }

private:
  void hmac_sha256(const std::string &command, unsigned char *out,
                   size_t &out_len) const {
    // FIX[1.4]: EVP_MAC avoids deprecated OpenSSL 1.x HMAC APIs.
    EVP_MAC *mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (!mac)
      throw std::runtime_error("Failed to fetch HMAC provider");
    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!ctx)
      throw std::runtime_error("Failed to create HMAC context");
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                         const_cast<char *>("SHA256"), 0),
        OSSL_PARAM_construct_end()};
    if (EVP_MAC_init(
            ctx, reinterpret_cast<const unsigned char *>(cluster_secret.data()),
            cluster_secret.size(), params) != 1 ||
        EVP_MAC_update(ctx,
                       reinterpret_cast<const unsigned char *>(command.data()),
                       command.size()) != 1 ||
        EVP_MAC_final(ctx, out, &out_len, SHA256_DIGEST_LENGTH) != 1) {
      EVP_MAC_CTX_free(ctx);
      throw std::runtime_error("Failed to generate audit HMAC");
    }
    EVP_MAC_CTX_free(ctx);
  }

  void rotate_if_needed() const {
    // FIX[REL-2]: Rotate the flat audit ledger before it can fill disks
    // indefinitely.
    const std::uintmax_t max_bytes = 100ULL * 1024ULL * 1024ULL;
    if (!std::filesystem::exists(log_path) ||
        std::filesystem::file_size(log_path) < max_bytes)
      return;
    for (int i = 4; i >= 1; --i) {
      const auto from = log_path + "." + std::to_string(i);
      const auto to = log_path + "." + std::to_string(i + 1);
      if (std::filesystem::exists(from))
        std::filesystem::rename(from, to);
    }
    std::filesystem::rename(log_path, log_path + ".1");
  }

  static std::string to_hex(unsigned char *hash, size_t len) {
    std::stringstream hex_ss;
    for (size_t i = 0; i < len; i++) {
      hex_ss << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<int>(hash[i]);
    }
    return hex_ss.str();
  }

  // BUG-17 FIX: Read the last PROOF: field from the ledger so the hash chain
  // continues across server restarts instead of resetting to zeros every time.
  std::string load_last_proof() const {
    static const std::string zero_hash(64, '0');
    if (!std::filesystem::exists(log_path))
      return zero_hash;
    std::ifstream in(log_path);
    if (!in.is_open())
      return zero_hash;
    std::string last_line, line;
    while (std::getline(in, line)) {
      if (!line.empty())
        last_line = line;
    }
    const auto pos = last_line.find("PROOF:");
    if (pos == std::string::npos)
      return zero_hash;
    std::string proof = last_line.substr(pos + 6);
    // Trim trailing whitespace/carriage-returns
    while (!proof.empty() && (proof.back() == '\r' || proof.back() == '\n' ||
                              proof.back() == ' '))
      proof.pop_back();
    return proof.empty() ? zero_hash : proof;
  }
};
} // namespace tsh
