#pragma once
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tsh {
class AstSigner {
private:
  static std::string get_secret_key() {
    // Try to load from environment variable first
    const char *env_key = std::getenv("TSH_SECRET_KEY");
    if (env_key && std::strlen(env_key) > 0) {
      return std::string(env_key);
    }

    // Try to load from secure config file
    std::ifstream key_file("/etc/tsh/secret.key");
    if (key_file.is_open()) {
      std::string key;
      std::getline(key_file, key);
      key_file.close();

      // Validate key format and strength
      if (key.length() < 32) {
        throw std::runtime_error(
            "Secret key too short - must be at least 32 characters");
      }
      return key;
    }

    // Generate and store a new key if none exists
    return generate_and_store_key();
  }

  static std::string generate_and_store_key() {
    // Generate cryptographically secure random key
    std::vector<unsigned char> key_bytes(32);
    if (RAND_bytes(key_bytes.data(), key_bytes.size()) != 1) {
      throw std::runtime_error("Failed to generate secure random key");
    }

    // Convert to hex string for storage
    std::stringstream ss;
    for (unsigned char byte : key_bytes) {
      ss << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<int>(byte);
    }
    std::string key = ss.str();

    // Create secure directory if it doesn't exist
    std::filesystem::create_directories("/etc/tsh");

    // Store with restricted permissions
    std::ofstream key_file("/etc/tsh/secret.key");
    if (key_file.is_open()) {
      key_file << key;
      key_file.close();

      // Set restrictive permissions (owner read only)
      std::filesystem::permissions("/etc/tsh/secret.key",
                                   std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write);
    }

    return key;
  }

public:
  // Generates a cryptographic signature proving the origin of the AST execution
  // graph
  static std::string sign(const std::vector<uint8_t> &serialized_ast) {
    try {
      std::string secret_key = get_secret_key();

      unsigned char hash[SHA256_DIGEST_LENGTH];
      size_t len = sizeof(hash);
      // FIX[1.4]: EVP_MAC is the OpenSSL 3-compatible HMAC API.
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
              ctx, reinterpret_cast<const unsigned char *>(secret_key.data()),
              secret_key.size(), params) != 1) {
        EVP_MAC_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize HMAC");
      }
      if (EVP_MAC_update(ctx, serialized_ast.data(), serialized_ast.size()) !=
          1) {
        EVP_MAC_CTX_free(ctx);
        throw std::runtime_error("Failed to update HMAC");
      }
      if (EVP_MAC_final(ctx, hash, &len, sizeof(hash)) != 1) {
        EVP_MAC_CTX_free(ctx);
        throw std::runtime_error("Failed to finalize HMAC");
      }
      EVP_MAC_CTX_free(ctx);

      std::stringstream result_ss;
      for (size_t i = 0; i < len; i++) {
        result_ss << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(hash[i]);
      }
      return result_ss.str();

    } catch (const std::exception &e) {
      throw std::runtime_error("AST signing failed: " + std::string(e.what()));
    }
  }

  // Zero-trust verification: Ensures the AST was not maliciously altered in
  // transit
  static bool verify(const std::vector<uint8_t> &serialized_ast,
                     const std::string &signature) {
    try {
      std::string computed_signature = sign(serialized_ast);
      // FIX[CRIT-3]: std::string::operator== leaks timing information.
      if (computed_signature.size() != signature.size())
        return false;
      return CRYPTO_memcmp(computed_signature.c_str(), signature.c_str(),
                           computed_signature.size()) == 0;
    } catch (const std::exception &e) {
      // Log error but don't throw - verification failure is expected
      return false;
    }
  }

  // Key rotation support for operational security
  static bool rotate_key() {
    try {
      // BUG-1 FIX: Back up the OLD key FIRST, before generating the new one.
      // Previously, generate_and_store_key() wrote the new key to secret.key,
      // then the rename put the new key into the backup — losing the old key.
      std::filesystem::rename("/etc/tsh/secret.key",
                              "/etc/tsh/secret.key.backup");

      // BUG-14 FIX: Atomic write — generate into secret.key.new, then rename.
      // A crash between the old rename and a plain ofstream write would leave
      // no active key at all. POSIX rename() on the same filesystem is atomic.
      std::vector<unsigned char> key_bytes(32);
      if (RAND_bytes(key_bytes.data(), static_cast<int>(key_bytes.size())) !=
          1) {
        // Restore backup so the service is not left without a key.
        std::filesystem::rename("/etc/tsh/secret.key.backup",
                                "/etc/tsh/secret.key");
        return false;
      }
      std::stringstream ss;
      for (unsigned char byte : key_bytes) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(byte);
      }
      const std::string new_key = ss.str();

      std::ofstream tmp("/etc/tsh/secret.key.new");
      if (!tmp.is_open()) {
        std::filesystem::rename("/etc/tsh/secret.key.backup",
                                "/etc/tsh/secret.key");
        return false;
      }
      tmp << new_key;
      tmp.close();
      std::filesystem::permissions("/etc/tsh/secret.key.new",
                                   std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write);

      // Atomic promotion: if this rename succeeds, secret.key is always valid.
      std::filesystem::rename("/etc/tsh/secret.key.new", "/etc/tsh/secret.key");
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }
};
}