#pragma once

#include <vector>
#include <string>
#include <map>
#include <cstdint>
#include <memory>
#include <openssl/evp.h>

/**
 * SecureChannel NextGen
 * 
 * Features:
 * - Post-Quantum Key Exchange (Kyber-768)
 * - AEAD Symmetric Encryption (AES-256-GCM)
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
        MEMFD_EXEC = 0x04,
        STENO_PAYLOAD = 0x05,
        AUDIT_LOG = 0x06,
        CAP_REQUEST = 0x07
    };

    struct TypedCommand {
        std::string command;
        std::vector<std::string> args;
        std::map<std::string, std::string> metadata;
    };

    // Handshake using Kyber PQC Ready architecture
    bool handshake(Mode mode);
    bool rotate_keys();

    // Secure message transfer
    bool send_message(const std::vector<uint8_t>& message, MsgType type = MsgType::COMMAND);
    bool receive_message(std::vector<uint8_t>& message, MsgType& type);

    int last_error() const { return error_code; }

private:
    std::unique_ptr<ITransport> transport;
    int error_code = 0;
    bool initialized = false;

    // Symmetric keys derived from PQC handshake
    std::vector<uint8_t> session_key;
    uint32_t send_counter = 0;
    uint32_t recv_counter = 0;

    // OpenSSL contexts for AEAD
    EVP_CIPHER_CTX* encrypt_ctx = nullptr;
    EVP_CIPHER_CTX* decrypt_ctx = nullptr;

    // Internal helpers
    bool send_all(const void* buffer, size_t length);
    bool recv_all(void *buffer, size_t length);
    void set_error(int err) { error_code = err; }

    bool derive_session_keys(const std::vector<uint8_t>& shared_secret);
};
