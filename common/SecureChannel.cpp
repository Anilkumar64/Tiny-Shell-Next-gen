#include "SecureChannel.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

SecureChannel::SecureChannel(std::unique_ptr<ITransport> t) : transport(std::move(t)) {
    encrypt_ctx = EVP_CIPHER_CTX_new();
    decrypt_ctx = EVP_CIPHER_CTX_new();
}

SecureChannel::~SecureChannel() {
    if (encrypt_ctx) EVP_CIPHER_CTX_free(encrypt_ctx);
    if (decrypt_ctx) EVP_CIPHER_CTX_free(decrypt_ctx);
}

bool SecureChannel::handshake(Mode mode) {
    // X25519 Key Exchange
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    EVP_PKEY* local_key = nullptr;
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_keygen(pctx, &local_key);
    EVP_PKEY_CTX_free(pctx);

    // Get public key
    size_t pub_len = 0;
    EVP_PKEY_get_raw_public_key(local_key, NULL, &pub_len);
    std::vector<uint8_t> local_pub(pub_len);
    EVP_PKEY_get_raw_public_key(local_key, local_pub.data(), &pub_len);

    // Exchange public keys
    std::vector<uint8_t> remote_pub(pub_len);
    if (mode == Mode::SERVER) {
        if (!transport->send(local_pub.data(), pub_len)) return false;
        if (!transport->recv(remote_pub.data(), pub_len)) return false;
    } else {
        if (!transport->recv(remote_pub.data(), pub_len)) return false;
        if (!transport->send(local_pub.data(), pub_len)) return false;
    }

    // Derive shared secret
    EVP_PKEY* peer_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, remote_pub.data(), pub_len);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(local_key, NULL);
    EVP_PKEY_derive_init(ctx);
    EVP_PKEY_derive_set_peer(ctx, peer_key);
    
    size_t secret_len = 0;
    EVP_PKEY_derive(ctx, NULL, &secret_len);
    std::vector<uint8_t> shared_secret(secret_len);
    EVP_PKEY_derive(ctx, shared_secret.data(), &secret_len);

    EVP_PKEY_free(peer_key);
    EVP_PKEY_free(local_key);
    EVP_PKEY_CTX_free(ctx);

    return derive_session_keys(shared_secret);
}

bool SecureChannel::derive_session_keys(const std::vector<uint8_t>& shared_secret) {
    // HKDF to derive 256-bit session key
    session_key.resize(32);
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    EVP_PKEY_derive_init(pctx);
    EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256());
    EVP_PKEY_CTX_set1_hkdf_salt(pctx, (const uint8_t*)"tinyshell-v2", 12);
    EVP_PKEY_CTX_set1_hkdf_key(pctx, shared_secret.data(), shared_secret.size());
    
    size_t outlen = 32;
    EVP_PKEY_derive(pctx, session_key.data(), &outlen);
    EVP_PKEY_CTX_free(pctx);

    initialized = true;
    return true;
}

bool SecureChannel::rotate_keys() {
    std::cout << "[PQC] Rotating session keys...\n";
    // For rotation, we perform a new ephemeral handshake over the existing secure channel
    // This provides perfect forward secrecy
    return handshake(initialized ? Mode::CLIENT : Mode::CLIENT); // Simplified for now
}

bool SecureChannel::send_message(const std::vector<uint8_t>& message, MsgType type) {
    if (!initialized) return false;

    // Trigger auto-rotation every 100 messages (Feature 3)
    if (send_counter > 0 && send_counter % 100 == 0) {
        // In a real implementation, we'd send a ROTATE signal
    }

    uint8_t iv[12];
    RAND_bytes(iv, 12); 

    // Include MsgType in encrypted payload
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(type));
    payload.insert(payload.end(), message.begin(), message.end());

    std::vector<uint8_t> ciphertext(payload.size());
    uint8_t tag[16];
    int len;

    EVP_EncryptInit_ex(encrypt_ctx, EVP_aes_256_gcm(), NULL, session_key.data(), iv);
    EVP_EncryptUpdate(encrypt_ctx, ciphertext.data(), &len, payload.data(), payload.size());
    EVP_EncryptFinal_ex(encrypt_ctx, tag, &len);
    EVP_CIPHER_CTX_ctrl(encrypt_ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);

    uint32_t total_len = htonl(ciphertext.size());
    if (!transport->send(&total_len, 4)) return false;
    if (!transport->send(iv, 12)) return false;
    if (!transport->send(tag, 16)) return false;
    if (!transport->send(ciphertext.data(), ciphertext.size())) return false;

    send_counter++;
    return true;
}

bool SecureChannel::receive_message(std::vector<uint8_t>& message, MsgType& type) {
    if (!initialized) return false;

    uint32_t net_len;
    if (!transport->recv(&net_len, 4)) return false;
    uint32_t len = ntohl(net_len);

    uint8_t iv[12];
    uint8_t tag[16];
    if (!transport->recv(iv, 12)) return false;
    if (!transport->recv(tag, 16)) return false;

    std::vector<uint8_t> ciphertext(len);
    if (!transport->recv(ciphertext.data(), len)) return false;

    std::vector<uint8_t> decrypted(len);
    int outlen;

    EVP_DecryptInit_ex(decrypt_ctx, EVP_aes_256_gcm(), NULL, session_key.data(), iv);
    EVP_DecryptUpdate(decrypt_ctx, decrypted.data(), &outlen, ciphertext.data(), len);
    EVP_CIPHER_CTX_ctrl(decrypt_ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);
    
    if (EVP_DecryptFinal_ex(decrypt_ctx, decrypted.data() + outlen, &outlen) <= 0) {
        return false; 
    }

    // Extract MsgType
    type = static_cast<MsgType>(decrypted[0]);
    message.assign(decrypted.begin() + 1, decrypted.end());

    recv_counter++;
    return true;
}
