#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <openssl/evp.h>
#include <string>
#include <vector>

/**
 * SecureChannel NextGen
 *
 * Features:
 * - TLS 1.2 minimum, TLS 1.3 preferred
 * - Mutual TLS with authorized client public keys
 * - Ed25519 signed nonce gate before application frames
 * - Polymorphic Transport Ready
 */

#include "transport/ITransport.h"
#include <memory>

class SecureChannel {
public:
  enum class Mode { CLIENT, SERVER };

  explicit SecureChannel(std::unique_ptr<ITransport> transport);
  ~SecureChannel();

  enum class MsgType : uint8_t {
    COMMAND = 0x01,
    KEY_ROTATE = 0x02,
    WASM_PAYLOAD = 0x03,
    CONTROL = 0x04,
    AUDIT_LOG = 0x06,
    CAP_REQUEST = 0x07,
    PTY_INPUT = 0x10,
    PTY_OUTPUT = 0x11,
    PTY_EXIT = 0x12
  };

  struct TypedCommand {
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> metadata;
  };

  void set_peer_identity(std::string peer_identity_key);

  // TLS handshake plus mTLS authorization and signed nonce verification.
  bool handshake(Mode mode);
  bool rotate_keys();

  // Secure message transfer
  bool send_message(const std::vector<uint8_t> &message,
                    MsgType type = MsgType::COMMAND);
  bool receive_message(std::vector<uint8_t> &message, MsgType &type);
  int raw_fd() const { return transport ? transport->raw_fd() : -1; }

  int last_error() const { return error_code; }

private:
  struct TlsState;

  std::unique_ptr<ITransport> transport;
  int error_code = 0;
  bool initialized = false;

  // Symmetric keys derived from PQC handshake
  std::vector<uint8_t> session_key;
  uint32_t send_counter = 0;
  uint32_t recv_counter = 0;

  // OpenSSL contexts for AEAD
  EVP_CIPHER_CTX *encrypt_ctx = nullptr;
  EVP_CIPHER_CTX *decrypt_ctx = nullptr;

  // Internal helpers
  bool send_all(const void *buffer, size_t length);
  bool recv_all(void *buffer, size_t length);
  void set_error(int err) { error_code = err; }

  std::string peer_identity = "127.0.0.1:4444";
  Mode mode_ =
      Mode::CLIENT; // BUG-16 FIX: stored so rotate_keys() uses the correct role
  std::unique_ptr<TlsState> tls_;
  bool derive_session_keys(const std::vector<uint8_t> &shared_secret,
                           const std::vector<uint8_t> &hkdf_salt);
};
