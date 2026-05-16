// SecureChannel.cpp
// Purpose: Mutual TLS channel with authorized-key pinning, Ed25519 nonce
//          signing, and replay protection.
// Component: common

// ── Bug fix in this revision
// ──────────────────────────────────────────────────
//
//  FIX — C++17 compatibility: replaced unordered_set::contains() (C++20) with
//    find() != end() (C++17).
//
//    CMakeLists.txt declares CMAKE_CXX_STANDARD 17 (fixed separately).
//    The spec (Section 7) mandates C++17 — no C++20 features.
//    unordered_set::contains() was the only C++20 call in this file; replacing
//    it makes the file clean under -std=c++17 -Wall -Wextra -Werror.
//
//  All other logic is preserved exactly:
//    - SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT on server side
//    - client_public_key_verify_callback checks authorized_keys DER store
//    - nonce: UUID-v4 + "." + unix_timestamp_ms, signed with Ed25519
//    - 30-second skew window, 60-second replay cache
//    - OPENSSL_cleanse() on all key material after use
//    - TLS 1.2 minimum, TLS 1.3 preferred
//    - Rejection logged with ISO8601 timestamp + source IP
// ─────────────────────────────────────────────────────────────────────────────

#include "SecureChannel.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

namespace {

constexpr uint32_t kMaxFrameLen = 1024 * 1024;
constexpr std::chrono::milliseconds kNonceSkew{30'000};
constexpr std::chrono::milliseconds kNonceTtl{60'000};
constexpr std::size_t kMaxSeenNonces = 16'384;

constexpr std::string_view kNonceMetadataName = "x-tsh-nonce-sig";
constexpr std::string_view kAuthorizedKeysPath = "/etc/tsh/authorized_keys";
constexpr std::string_view kDefaultServerCertPath = "/etc/tsh/server.crt";
constexpr std::string_view kDefaultServerKeyPath = "/etc/tsh/server.key";

struct AuthorizedKeyStore {
  std::string path;
  std::vector<std::vector<unsigned char>> public_keys_der;
};

std::mutex g_authorized_keys_mutex;
std::shared_ptr<const AuthorizedKeyStore> g_authorized_keys;
std::string g_authorized_keys_path;

class NonceReplayCache {
public:
  // Precondition:  nonce is non-empty and timestamp-validated by caller.
  // Postcondition: returns true and records nonce if unseen; false if replayed.
  bool remember(const std::string &nonce, std::chrono::milliseconds now) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_locked(now);
    // FIX: replaced seen_.contains(nonce) [C++20] with C++17 equivalent.
    if (seen_.find(nonce) != seen_.end()) {
      return false;
    }
    seen_.insert(nonce);
    entries_.push_back({nonce, now + kNonceTtl});
    while (entries_.size() > kMaxSeenNonces) {
      seen_.erase(entries_.front().nonce);
      entries_.pop_front();
    }
    return true;
  }

private:
  struct Entry {
    std::string nonce;
    std::chrono::milliseconds expires_at;
  };

  void prune_locked(std::chrono::milliseconds now) {
    while (!entries_.empty() && entries_.front().expires_at <= now) {
      seen_.erase(entries_.front().nonce);
      entries_.pop_front();
    }
  }

  std::mutex mutex_;
  std::deque<Entry> entries_;
  std::unordered_set<std::string> seen_;
};

NonceReplayCache g_seen_nonces;

std::once_flag g_openssl_once;

void initialize_openssl() {
  std::call_once(g_openssl_once, [] {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
  });
}

std::string env_string(const char *name, std::string fallback = {}) {
  if (const char *value = std::getenv(name)) {
    return value;
  }
  return fallback;
}

std::filesystem::path tsh_home() {
  const char *home = std::getenv("HOME");
  if (!home || std::string(home).empty()) {
    throw std::runtime_error("HOME is required for TinyShell identity storage");
  }
  auto dir = std::filesystem::path(home) / ".tsh";
  std::filesystem::create_directories(dir);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  return dir;
}

std::string iso8601_utc_now() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds)
          .count();
  const std::time_t raw = std::chrono::system_clock::to_time_t(now);

  std::tm tm{};
  gmtime_r(&raw, &tm);

  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << "." << std::setw(3)
      << std::setfill('0') << millis << "Z";
  return out.str();
}

std::string peer_ip_from_fd(int fd) {
  if (fd < 0) {
    return "unknown";
  }

  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  if (getpeername(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    return "unknown";
  }

  char host[INET6_ADDRSTRLEN]{};
  if (addr.ss_family == AF_INET) {
    const auto *in = reinterpret_cast<const sockaddr_in *>(&addr);
    if (inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host))) {
      return host;
    }
  } else if (addr.ss_family == AF_INET6) {
    const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(&addr);
    if (inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host))) {
      return host;
    }
  } else if (addr.ss_family == AF_UNIX) {
    return "local";
  }

  return "unknown";
}

std::string peer_ip_from_ssl(const SSL *ssl) {
  return ssl ? peer_ip_from_fd(SSL_get_fd(ssl)) : "unknown";
}

std::string peer_ip_from_store_ctx(X509_STORE_CTX *store_ctx) {
  if (!store_ctx) {
    return "unknown";
  }
  auto *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(
      store_ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
  return peer_ip_from_ssl(ssl);
}

void log_rejection(const std::string &source_ip, const std::string &reason) {
  std::cerr << iso8601_utc_now() << " source_ip=" << source_ip
            << " rejection=\"" << reason << "\"\n";
}

std::string openssl_error_string() {
  std::ostringstream out;
  bool first = true;
  while (const unsigned long err = ERR_get_error()) {
    if (!first) {
      out << "; ";
    }
    first = false;
    out << ERR_error_string(err, nullptr);
  }
  return first ? "unknown OpenSSL error" : out.str();
}

std::string trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

std::string strip_inline_comment(std::string_view line) {
  const auto pos = line.find('#');
  if (pos == std::string_view::npos) {
    return trim(line);
  }
  return trim(line.substr(0, pos));
}

std::vector<unsigned char> from_hex(std::string_view hex) {
  std::string compact;
  compact.reserve(hex.size());
  for (unsigned char ch : hex) {
    if (std::isxdigit(ch)) {
      compact.push_back(static_cast<char>(std::tolower(ch)));
    } else if (ch != ':' && !std::isspace(ch)) {
      return {};
    }
  }

  if (compact.empty() || compact.size() % 2 != 0) {
    return {};
  }

  std::vector<unsigned char> out;
  out.reserve(compact.size() / 2);
  for (std::size_t i = 0; i < compact.size(); i += 2) {
    const auto byte = compact.substr(i, 2);
    char *end = nullptr;
    const auto value = std::strtoul(byte.c_str(), &end, 16);
    if (!end || *end != '\0' || value > 0xff) {
      return {};
    }
    out.push_back(static_cast<unsigned char>(value));
  }
  return out;
}

std::string to_hex(const unsigned char *data, std::size_t size) {
  std::ostringstream out;
  for (std::size_t i = 0; i < size; ++i) {
    out << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(data[i]);
  }
  return out.str();
}

std::string to_hex(const std::vector<unsigned char> &bytes) {
  return to_hex(bytes.data(), bytes.size());
}

std::vector<unsigned char> sha256(const std::vector<unsigned char> &bytes) {
  std::vector<unsigned char> digest(EVP_MD_size(EVP_sha256()));
  unsigned int digest_len = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digest_len,
                 EVP_sha256(), nullptr) != 1) {
    return {};
  }
  digest.resize(digest_len);
  return digest;
}

std::vector<unsigned char> base64_decode(std::string_view input) {
  std::string compact;
  compact.reserve(input.size());
  for (unsigned char ch : input) {
    if (!std::isspace(ch)) {
      compact.push_back(static_cast<char>(ch));
    }
  }

  if (compact.empty() || compact.size() % 4 != 0) {
    return {};
  }

  std::vector<unsigned char> decoded((compact.size() / 4) * 3 + 3);
  const int len = EVP_DecodeBlock(
      decoded.data(), reinterpret_cast<const unsigned char *>(compact.data()),
      static_cast<int>(compact.size()));
  if (len < 0) {
    return {};
  }

  std::size_t real_len = static_cast<std::size_t>(len);
  if (!compact.empty() && compact.back() == '=') {
    --real_len;
    if (compact.size() >= 2 && compact[compact.size() - 2] == '=') {
      --real_len;
    }
  }
  decoded.resize(real_len);
  return decoded;
}

std::optional<std::string_view> read_ssh_string(std::string_view &wire) {
  if (wire.size() < 4) {
    return std::nullopt;
  }
  const auto len =
      (static_cast<uint32_t>(static_cast<unsigned char>(wire[0])) << 24) |
      (static_cast<uint32_t>(static_cast<unsigned char>(wire[1])) << 16) |
      (static_cast<uint32_t>(static_cast<unsigned char>(wire[2])) << 8) |
      static_cast<uint32_t>(static_cast<unsigned char>(wire[3]));
  wire.remove_prefix(4);
  if (wire.size() < len) {
    return std::nullopt;
  }
  const auto field = wire.substr(0, len);
  wire.remove_prefix(len);
  return field;
}

std::vector<unsigned char> public_key_der(EVP_PKEY *key) {
  if (!key) {
    return {};
  }
  const int len = i2d_PUBKEY(key, nullptr);
  if (len <= 0) {
    return {};
  }

  std::vector<unsigned char> der(static_cast<std::size_t>(len));
  unsigned char *cursor = der.data();
  if (i2d_PUBKEY(key, &cursor) != len) {
    return {};
  }
  return der;
}

std::vector<unsigned char> certificate_public_key_der(X509 *cert) {
  if (!cert) {
    return {};
  }

  EVP_PKEY *key = X509_get_pubkey(cert);
  if (!key) {
    return {};
  }
  auto der = public_key_der(key);
  EVP_PKEY_free(key);
  return der;
}

std::vector<unsigned char> parse_openssh_ed25519_key(std::string_view line) {
  std::istringstream in{std::string(line)};
  std::string type;
  std::string b64;
  in >> type >> b64;
  if (type != "ssh-ed25519" || b64.empty()) {
    return {};
  }

  auto wire_bytes = base64_decode(b64);
  if (wire_bytes.empty()) {
    return {};
  }
  std::string_view wire(reinterpret_cast<const char *>(wire_bytes.data()),
                        wire_bytes.size());
  const auto wire_type = read_ssh_string(wire);
  const auto raw_key = read_ssh_string(wire);
  if (!wire_type || !raw_key || *wire_type != "ssh-ed25519" ||
      raw_key->size() != 32) {
    OPENSSL_cleanse(wire_bytes.data(), wire_bytes.size());
    return {};
  }

  EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char *>(raw_key->data()),
      raw_key->size());
  auto der = public_key_der(key);
  if (key) {
    EVP_PKEY_free(key);
  }
  OPENSSL_cleanse(wire_bytes.data(), wire_bytes.size());
  return der;
}

void append_unique_key(std::vector<std::vector<unsigned char>> *keys,
                       std::vector<unsigned char> key) {
  if (key.empty()) {
    return;
  }
  const auto exists =
      std::any_of(keys->begin(), keys->end(),
                  [&](const auto &candidate) { return candidate == key; });
  if (!exists) {
    keys->push_back(std::move(key));
  }
}

void parse_pem_blocks(std::string_view text,
                      std::vector<std::vector<unsigned char>> *keys) {
  std::size_t pos = 0;
  while (true) {
    const auto begin = text.find("-----BEGIN ", pos);
    if (begin == std::string_view::npos) {
      return;
    }
    const auto end = text.find("-----END ", begin);
    if (end == std::string_view::npos) {
      return;
    }
    const auto close = text.find("-----", end + 9);
    if (close == std::string_view::npos) {
      return;
    }
    const auto block_end = close + 5;
    const auto block = text.substr(begin, block_end - begin);

    BIO *bio = BIO_new_mem_buf(block.data(), static_cast<int>(block.size()));
    if (bio) {
      if (EVP_PKEY *pkey =
              PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr)) {
        append_unique_key(keys, public_key_der(pkey));
        EVP_PKEY_free(pkey);
      } else {
        ERR_clear_error();
      }
      BIO_free(bio);
    }

    bio = BIO_new_mem_buf(block.data(), static_cast<int>(block.size()));
    if (bio) {
      if (X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        append_unique_key(keys, certificate_public_key_der(cert));
        X509_free(cert);
      } else {
        ERR_clear_error();
      }
      BIO_free(bio);
    }

    pos = block_end;
  }
}

std::vector<std::vector<unsigned char>>
parse_authorized_keys_text(const std::string &text) {
  std::vector<std::vector<unsigned char>> keys;
  parse_pem_blocks(text, &keys);

  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const auto clean = strip_inline_comment(line);
    if (clean.empty() || clean.rfind("-----", 0) == 0) {
      continue;
    }

    append_unique_key(&keys, parse_openssh_ed25519_key(clean));
    append_unique_key(&keys, from_hex(clean));
  }
  return keys;
}

std::shared_ptr<const AuthorizedKeyStore> load_authorized_keys_store() {
  const auto path =
      env_string("TSH_AUTHORIZED_KEYS", std::string(kAuthorizedKeysPath));

  std::lock_guard<std::mutex> lock(g_authorized_keys_mutex);
  if (g_authorized_keys && g_authorized_keys_path == path) {
    return g_authorized_keys;
  }

  std::ifstream in(path, std::ios::binary);
  auto store = std::make_shared<AuthorizedKeyStore>();
  store->path = path;
  if (!in) {
    g_authorized_keys = store;
    g_authorized_keys_path = path;
    return g_authorized_keys;
  }

  const std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  store->public_keys_der = parse_authorized_keys_text(text);
  g_authorized_keys = store;
  g_authorized_keys_path = path;
  return g_authorized_keys;
}

bool public_key_is_authorized(const std::vector<unsigned char> &der) {
  const auto store = load_authorized_keys_store();
  for (const auto &authorized : store->public_keys_der) {
    if (authorized.size() == der.size() &&
        CRYPTO_memcmp(authorized.data(), der.data(), der.size()) == 0) {
      return true;
    }
  }
  return false;
}

// ── mTLS: server-side client certificate verification ────────────────────────
// Called by OpenSSL during TLS handshake for every certificate in the chain.
// At depth 0 (the leaf / client cert): extract the public key and check it
// against the authorized_keys store loaded from /etc/tsh/authorized_keys.
// Any unknown client is rejected before any gRPC handler runs.
int client_public_key_verify_callback(int preverify_ok,
                                      X509_STORE_CTX *store_ctx) {
  const int depth = X509_STORE_CTX_get_error_depth(store_ctx);
  if (depth != 0) {
    return 1; // trust chain intermediates without checking authorized_keys
  }

  const auto source_ip = peer_ip_from_store_ctx(store_ctx);
  X509 *cert = X509_STORE_CTX_get_current_cert(store_ctx);
  if (!cert) {
    log_rejection(source_ip, "missing client certificate");
    return 0;
  }

  const auto der = certificate_public_key_der(cert);
  if (der.empty()) {
    log_rejection(source_ip,
                  "client certificate public key could not be extracted");
    X509_STORE_CTX_set_error(store_ctx, X509_V_ERR_CERT_REJECTED);
    return 0;
  }

  const auto store = load_authorized_keys_store();
  if (store->public_keys_der.empty()) {
    log_rejection(source_ip,
                  "authorized_keys is missing or contains no usable keys");
    X509_STORE_CTX_set_error(store_ctx, X509_V_ERR_CERT_REJECTED);
    return 0;
  }

  if (!public_key_is_authorized(der)) {
    std::string reason = "client certificate public key is not authorized";
    if (!preverify_ok) {
      reason += ": ";
      reason +=
          X509_verify_cert_error_string(X509_STORE_CTX_get_error(store_ctx));
    }
    log_rejection(source_ip, reason);
    X509_STORE_CTX_set_error(store_ctx, X509_V_ERR_CERT_REJECTED);
    return 0;
  }

  return 1;
}

int pinned_server_verify_callback(int, X509_STORE_CTX *) {
  // Server certificate is pinned via verify_server_pin() after handshake.
  // We accept it here and check the pin explicitly.
  return 1;
}

bool configure_common_tls_context(SSL_CTX *ctx) {
  if (!ctx) {
    return false;
  }
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
  SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_ciphersuites(
      ctx, "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:"
           "TLS_AES_128_GCM_SHA256");
  SSL_CTX_set_cipher_list(
      ctx, "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
           "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
           "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256");
  return true;
}

// ── Server TLS context
// ──────────────────────────────────────────────────────── Precondition:
// authorized_keys must contain at least one usable key. Postcondition:
// SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT are set;
//               client_public_key_verify_callback is installed.
SSL_CTX *create_server_context() {
  auto *ctx = SSL_CTX_new(TLS_server_method());
  if (!configure_common_tls_context(ctx)) {
    if (ctx) {
      SSL_CTX_free(ctx);
    }
    return nullptr;
  }

  const auto cert_path =
      env_string("TSH_TLS_CERT", std::string(kDefaultServerCertPath));
  const auto key_path =
      env_string("TSH_TLS_KEY", std::string(kDefaultServerKeyPath));

  if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) !=
          1 ||
      SSL_CTX_check_private_key(ctx) != 1) {
    std::cerr << "[TinyShell TLS] failed to load server certificate/key: "
              << openssl_error_string() << "\n";
    SSL_CTX_free(ctx);
    return nullptr;
  }

  const auto store = load_authorized_keys_store();
  if (store->public_keys_der.empty()) {
    std::cerr << "[TinyShell TLS] no authorized client keys loaded from "
              << store->path << "\n";
    SSL_CTX_free(ctx);
    return nullptr;
  }

  // Enforce mTLS: require and verify client certificate at handshake time.
  // Connections without a valid certificate in authorized_keys are dropped
  // before any gRPC handler is invoked.
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     client_public_key_verify_callback);
  SSL_CTX_set_verify_depth(ctx, 8);
  return ctx;
}

// ── Client TLS context
// ──────────────────────────────────────────────────────── Loads
// ~/.tsh/client.crt and ~/.tsh/client.key for presentation to the server during
// the TLS handshake.
SSL_CTX *create_client_context() {
  auto *ctx = SSL_CTX_new(TLS_client_method());
  if (!configure_common_tls_context(ctx)) {
    if (ctx) {
      SSL_CTX_free(ctx);
    }
    return nullptr;
  }

  const auto home = tsh_home();
  const auto cert_path = home / "client.crt";
  const auto key_path = home / "client.key";

  if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) !=
          1 ||
      SSL_CTX_check_private_key(ctx) != 1) {
    std::cerr << "[TinyShell TLS] failed to load client certificate/key from "
              << cert_path << " and " << key_path << ": "
              << openssl_error_string() << "\n";
    SSL_CTX_free(ctx);
    return nullptr;
  }

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, pinned_server_verify_callback);
  SSL_CTX_set_verify_depth(ctx, 8);
  return ctx;
}

struct ServerPins {
  std::vector<std::vector<unsigned char>> pubkey_der;
  std::vector<std::vector<unsigned char>> pubkey_sha256;
};

void append_pin_value(ServerPins *pins, std::string value, bool force_hash) {
  value = trim(value);
  if (value.empty()) {
    return;
  }

  constexpr std::string_view kSha256Prefix = "sha256:";
  if (value.size() > kSha256Prefix.size() &&
      std::equal(kSha256Prefix.begin(), kSha256Prefix.end(), value.begin(),
                 [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
                 })) {
    force_hash = true;
    value = value.substr(kSha256Prefix.size());
  }

  if (value.rfind("ssh-ed25519 ", 0) == 0) {
    append_unique_key(&pins->pubkey_der, parse_openssh_ed25519_key(value));
    return;
  }

  const auto decoded = from_hex(value);
  if (decoded.empty()) {
    return;
  }

  if (force_hash ||
      decoded.size() == static_cast<std::size_t>(EVP_MD_size(EVP_sha256()))) {
    append_unique_key(&pins->pubkey_sha256, decoded);
  } else {
    append_unique_key(&pins->pubkey_der, decoded);
  }
}

ServerPins load_server_pins(const std::string &peer_identity) {
  ServerPins pins;
  const auto config_path = tsh_home() / "config";
  std::ifstream in(config_path, std::ios::binary);
  if (!in) {
    return pins;
  }

  const std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  parse_pem_blocks(text, &pins.pubkey_der);

  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const auto clean = strip_inline_comment(line);
    if (clean.empty() || clean.rfind("-----", 0) == 0) {
      continue;
    }

    const auto eq = clean.find('=');
    std::string key;
    std::string value;
    if (eq == std::string::npos) {
      std::istringstream parts(clean);
      parts >> key;
      std::getline(parts, value);
      value = trim(value);
    } else {
      key = trim(std::string_view(clean).substr(0, eq));
      value = trim(std::string_view(clean).substr(eq + 1));
    }

    const bool host_scoped =
        key == "server_pubkey." + peer_identity ||
        key == "server_public_key." + peer_identity ||
        key == "server_pubkey_sha256." + peer_identity ||
        key == "server_fingerprint_sha256." + peer_identity;
    const bool global_key =
        key == "server_pubkey" || key == "server_public_key" ||
        key == "server_pubkey_sha256" || key == "server_fingerprint_sha256";
    if (!host_scoped && !global_key) {
      continue;
    }

    const bool is_hash = key.find("sha256") != std::string::npos ||
                         key.find("fingerprint") != std::string::npos;
    append_pin_value(&pins, value, is_hash);
  }

  return pins;
}

bool verify_server_pin(SSL *ssl, const std::string &peer_identity) {
  X509 *cert = SSL_get_peer_certificate(ssl);
  if (!cert) {
    std::cerr << "[TinyShell TLS] server did not present a certificate\n";
    return false;
  }

  const auto der = certificate_public_key_der(cert);
  X509_free(cert);
  if (der.empty()) {
    std::cerr << "[TinyShell TLS] could not extract server public key\n";
    return false;
  }

  const auto pins = load_server_pins(peer_identity);
  if (pins.pubkey_der.empty() && pins.pubkey_sha256.empty()) {
    std::cerr << "[TinyShell TLS] no server public key pin found in "
              << (tsh_home() / "config") << "\n";
    return false;
  }

  for (const auto &pin : pins.pubkey_der) {
    if (pin.size() == der.size() &&
        CRYPTO_memcmp(pin.data(), der.data(), der.size()) == 0) {
      return true;
    }
  }

  const auto fingerprint = sha256(der);
  for (const auto &pin : pins.pubkey_sha256) {
    if (pin.size() == fingerprint.size() &&
        CRYPTO_memcmp(pin.data(), fingerprint.data(), fingerprint.size()) ==
            0) {
      return true;
    }
  }

  std::cerr << "[TinyShell TLS] server public key pin mismatch for "
            << peer_identity << "\n";
  return false;
}

std::chrono::milliseconds unix_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
}

std::string uuid_v4() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw std::runtime_error("RAND_bytes failed while generating nonce");
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

  std::ostringstream out;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out << "-";
    }
    out << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(bytes[i]);
  }
  OPENSSL_cleanse(bytes.data(), bytes.size());
  return out.str();
}

bool valid_uuid_v4(std::string_view uuid) {
  if (uuid.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (uuid[i] != '-') {
        return false;
      }
      continue;
    }
    if (!std::isxdigit(static_cast<unsigned char>(uuid[i]))) {
      return false;
    }
  }
  const char version = static_cast<char>(std::tolower(uuid[14]));
  const char variant = static_cast<char>(std::tolower(uuid[19]));
  return version == '4' &&
         (variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

std::optional<std::chrono::milliseconds>
parse_timestamp_ms(std::string_view s) {
  if (s.empty() || s.size() > 19) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (unsigned char ch : s) {
    if (!std::isdigit(ch)) {
      return std::nullopt;
    }
    const auto digit = static_cast<unsigned>(ch - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return std::chrono::milliseconds(value);
}

bool nonce_timestamp_is_fresh(const std::string &nonce, std::string *reason) {
  const auto dot = nonce.rfind('.');
  if (dot == std::string::npos) {
    *reason = "nonce is missing timestamp separator";
    return false;
  }

  const auto uuid = std::string_view(nonce).substr(0, dot);
  const auto ts_text = std::string_view(nonce).substr(dot + 1);
  if (!valid_uuid_v4(uuid)) {
    *reason = "nonce UUID is not UUID-v4";
    return false;
  }

  const auto ts = parse_timestamp_ms(ts_text);
  if (!ts) {
    *reason = "nonce timestamp is invalid";
    return false;
  }

  const auto now = unix_time_ms();
  const auto skew = now > *ts ? now - *ts : *ts - now;
  if (skew > kNonceSkew) {
    *reason = "nonce timestamp is outside the 30 second window";
    return false;
  }

  if (!g_seen_nonces.remember(nonce, now)) {
    *reason = "nonce replay detected";
    return false;
  }
  return true;
}

EVP_PKEY *load_private_key_file(const std::filesystem::path &path) {
  BIO *bio = BIO_new_file(path.c_str(), "rb");
  if (!bio) {
    return nullptr;
  }
  EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return key;
}

std::vector<unsigned char> sign_ed25519(EVP_PKEY *key,
                                        std::string_view payload) {
  if (!key || EVP_PKEY_id(key) != EVP_PKEY_ED25519) {
    return {};
  }

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    return {};
  }

  std::vector<unsigned char> signature(64);
  std::size_t signature_len = signature.size();
  const bool ok =
      EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
      EVP_DigestSign(ctx, signature.data(), &signature_len,
                     reinterpret_cast<const unsigned char *>(payload.data()),
                     payload.size()) == 1;
  EVP_MD_CTX_free(ctx);

  if (!ok) {
    OPENSSL_cleanse(signature.data(), signature.size());
    return {};
  }
  signature.resize(signature_len);
  return signature;
}

bool verify_ed25519(EVP_PKEY *key, const std::vector<unsigned char> &signature,
                    std::string_view payload) {
  if (!key || EVP_PKEY_id(key) != EVP_PKEY_ED25519 || signature.empty()) {
    return false;
  }

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    return false;
  }

  const bool ok =
      EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
      EVP_DigestVerify(ctx, signature.data(), signature.size(),
                       reinterpret_cast<const unsigned char *>(payload.data()),
                       payload.size()) == 1;
  EVP_MD_CTX_free(ctx);
  return ok;
}

std::optional<std::pair<std::string, std::vector<unsigned char>>>
parse_nonce_metadata_value(std::string_view value) {
  const auto sig_dot = value.rfind('.');
  if (sig_dot == std::string_view::npos) {
    return std::nullopt;
  }
  const auto nonce = value.substr(0, sig_dot);
  const auto sig_hex = value.substr(sig_dot + 1);
  auto signature = from_hex(sig_hex);
  if (nonce.empty() || signature.empty()) {
    return std::nullopt;
  }
  return std::make_pair(std::string(nonce), std::move(signature));
}

struct TlsState {
  ~TlsState() {
    if (ssl) {
      SSL_free(ssl);
    }
    if (ctx) {
      SSL_CTX_free(ctx);
    }
  }

  SSL_CTX *ctx = nullptr;
  SSL *ssl = nullptr;
};

bool ssl_write_all(SSL *ssl, const void *buffer, std::size_t length) {
  const auto *ptr = static_cast<const unsigned char *>(buffer);
  std::size_t written_total = 0;
  while (written_total < length) {
    std::size_t written = 0;
    const int rc = SSL_write_ex(ssl, ptr + written_total,
                                length - written_total, &written);
    if (rc != 1) {
      const int err = SSL_get_error(ssl, rc);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    written_total += written;
  }
  return true;
}

bool ssl_read_all(SSL *ssl, void *buffer, std::size_t length) {
  auto *ptr = static_cast<unsigned char *>(buffer);
  std::size_t read_total = 0;
  while (read_total < length) {
    std::size_t n = 0;
    const int rc = SSL_read_ex(ssl, ptr + read_total, length - read_total, &n);
    if (rc != 1) {
      const int err = SSL_get_error(ssl, rc);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    read_total += n;
  }
  return true;
}

bool write_tls_frame(SSL *ssl, const std::vector<unsigned char> &payload) {
  if (payload.size() > kMaxFrameLen) {
    return false;
  }
  const uint32_t net_len = htonl(static_cast<uint32_t>(payload.size()));
  return ssl_write_all(ssl, &net_len, sizeof(net_len)) &&
         (payload.empty() ||
          ssl_write_all(ssl, payload.data(), payload.size()));
}

bool read_tls_frame(SSL *ssl, std::vector<unsigned char> *payload) {
  uint32_t net_len = 0;
  if (!ssl_read_all(ssl, &net_len, sizeof(net_len))) {
    return false;
  }
  const uint32_t len = ntohl(net_len);
  if (len == 0 || len > kMaxFrameLen) {
    return false;
  }
  payload->assign(len, 0);
  return ssl_read_all(ssl, payload->data(), payload->size());
}

bool send_signed_nonce(SSL *ssl) {
  const auto nonce = uuid_v4() + "." + std::to_string(unix_time_ms().count());
  const auto key_path = tsh_home() / "client.key";

  EVP_PKEY *key = load_private_key_file(key_path);
  if (!key) {
    std::cerr << "[TinyShell TLS] failed to load Ed25519 client key for nonce: "
              << openssl_error_string() << "\n";
    return false;
  }

  auto signature = sign_ed25519(key, nonce);
  EVP_PKEY_free(key);
  if (signature.empty()) {
    std::cerr << "[TinyShell TLS] failed to sign nonce with Ed25519 key\n";
    return false;
  }

  const auto value = nonce + "." + to_hex(signature);
  std::string header;
  header.reserve(kNonceMetadataName.size() + value.size() + 2);
  header.append(kNonceMetadataName);
  header.append(": ");
  header.append(value);

  std::vector<unsigned char> payload(header.begin(), header.end());
  const bool ok = write_tls_frame(ssl, payload);
  OPENSSL_cleanse(signature.data(), signature.size());
  OPENSSL_cleanse(payload.data(), payload.size());
  OPENSSL_cleanse(header.data(), header.size());
  return ok;
}

bool verify_nonce_from_client(SSL *ssl) {
  std::vector<unsigned char> frame;
  if (!read_tls_frame(ssl, &frame)) {
    log_rejection(peer_ip_from_ssl(ssl), "missing x-tsh-nonce-sig metadata");
    return false;
  }

  const std::string metadata(frame.begin(), frame.end());
  OPENSSL_cleanse(frame.data(), frame.size());
  const auto colon = metadata.find(':');
  if (colon == std::string::npos) {
    log_rejection(peer_ip_from_ssl(ssl), "malformed x-tsh-nonce-sig metadata");
    return false;
  }

  const auto name = trim(std::string_view(metadata).substr(0, colon));
  const auto value = trim(std::string_view(metadata).substr(colon + 1));
  if (name != kNonceMetadataName) {
    log_rejection(peer_ip_from_ssl(ssl), "missing x-tsh-nonce-sig metadata");
    return false;
  }

  auto parsed = parse_nonce_metadata_value(value);
  if (!parsed) {
    log_rejection(peer_ip_from_ssl(ssl), "malformed x-tsh-nonce-sig value");
    return false;
  }

  auto &[nonce, signature] = *parsed;
  std::string reason;
  if (!nonce_timestamp_is_fresh(nonce, &reason)) {
    OPENSSL_cleanse(signature.data(), signature.size());
    log_rejection(peer_ip_from_ssl(ssl), reason);
    return false;
  }

  X509 *cert = SSL_get_peer_certificate(ssl);
  if (!cert) {
    OPENSSL_cleanse(signature.data(), signature.size());
    log_rejection(peer_ip_from_ssl(ssl),
                  "cannot verify nonce without client certificate");
    return false;
  }
  EVP_PKEY *pubkey = X509_get_pubkey(cert);
  X509_free(cert);

  const bool ok = verify_ed25519(pubkey, signature, nonce);
  if (pubkey) {
    EVP_PKEY_free(pubkey);
  }
  OPENSSL_cleanse(signature.data(), signature.size());

  if (!ok) {
    log_rejection(peer_ip_from_ssl(ssl), "invalid x-tsh-nonce-sig signature");
    return false;
  }
  return true;
}

} // namespace

struct SecureChannel::TlsState : ::TlsState {};

SecureChannel::SecureChannel(std::unique_ptr<ITransport> t)
    : transport(std::move(t)) {}

SecureChannel::~SecureChannel() {
  tls_.reset();
  if (!session_key.empty()) {
    OPENSSL_cleanse(session_key.data(), session_key.size());
  }
  if (encrypt_ctx) {
    EVP_CIPHER_CTX_free(encrypt_ctx);
  }
  if (decrypt_ctx) {
    EVP_CIPHER_CTX_free(decrypt_ctx);
  }
}

void SecureChannel::set_peer_identity(std::string peer_identity_key) {
  peer_identity = std::move(peer_identity_key);
}

// Precondition:  raw_fd() returns a valid connected socket fd.
// Postcondition: initialized == true on success; tls_ is reset on failure.
//   Server path: client cert verified in verify callback before SSL_accept
//                returns; nonce verified after handshake.
//   Client path: server cert pin verified after SSL_connect; signed nonce sent.
bool SecureChannel::handshake(Mode mode) {
  mode_ = mode;
  initialized = false;
  set_error(0);
  initialize_openssl();

  const int fd = raw_fd();
  if (fd < 0) {
    set_error(80);
    return false;
  }

  tls_ = std::make_unique<TlsState>();
  tls_->ctx =
      mode == Mode::SERVER ? create_server_context() : create_client_context();
  if (!tls_->ctx) {
    set_error(81);
    tls_.reset();
    return false;
  }

  tls_->ssl = SSL_new(tls_->ctx);
  if (!tls_->ssl || SSL_set_fd(tls_->ssl, fd) != 1) {
    set_error(82);
    tls_.reset();
    return false;
  }

  const int rc =
      mode == Mode::SERVER ? SSL_accept(tls_->ssl) : SSL_connect(tls_->ssl);
  if (rc != 1) {
    X509 *peer_cert =
        mode == Mode::SERVER ? SSL_get_peer_certificate(tls_->ssl) : nullptr;
    const bool missing_peer_cert = peer_cert == nullptr;
    if (peer_cert) {
      X509_free(peer_cert);
    }
    if (mode == Mode::SERVER && missing_peer_cert) {
      log_rejection(peer_ip_from_ssl(tls_->ssl), "missing client certificate");
    } else if (mode == Mode::SERVER) {
      log_rejection(peer_ip_from_ssl(tls_->ssl),
                    "TLS handshake failed: " + openssl_error_string());
    } else {
      std::cerr << "[TinyShell TLS] TLS handshake failed: "
                << openssl_error_string() << "\n";
    }
    set_error(83);
    tls_.reset();
    return false;
  }

  if (mode == Mode::CLIENT && !verify_server_pin(tls_->ssl, peer_identity)) {
    set_error(84);
    tls_.reset();
    return false;
  }

  if (mode == Mode::CLIENT) {
    if (!send_signed_nonce(tls_->ssl)) {
      set_error(85);
      tls_.reset();
      return false;
    }
  } else if (!verify_nonce_from_client(tls_->ssl)) {
    set_error(86);
    tls_.reset();
    return false;
  }

  initialized = true;
  return true;
}

bool SecureChannel::derive_session_keys(
    const std::vector<uint8_t> &shared_secret,
    const std::vector<uint8_t> &hkdf_salt) {
  if (!session_key.empty()) {
    OPENSSL_cleanse(session_key.data(), session_key.size());
    session_key.clear();
  }
  if (!shared_secret.empty()) {
    OPENSSL_cleanse(const_cast<uint8_t *>(shared_secret.data()),
                    shared_secret.size());
  }
  if (!hkdf_salt.empty()) {
    OPENSSL_cleanse(const_cast<uint8_t *>(hkdf_salt.data()), hkdf_salt.size());
  }
  set_error(87);
  return false;
}

bool SecureChannel::rotate_keys() {
  if (!initialized || !tls_ || !tls_->ssl) {
    set_error(88);
    return false;
  }

  if (SSL_version(tls_->ssl) != TLS1_3_VERSION) {
    set_error(89);
    return false;
  }

  if (SSL_key_update(tls_->ssl, SSL_KEY_UPDATE_REQUESTED) != 1 ||
      SSL_do_handshake(tls_->ssl) != 1) {
    set_error(90);
    return false;
  }
  return true;
}

bool SecureChannel::send_message(const std::vector<uint8_t> &message,
                                 MsgType type) {
  if (!initialized || !tls_ || !tls_->ssl) {
    return false;
  }
  if (message.size() + 1 > kMaxFrameLen) {
    set_error(72);
    return false;
  }

  std::vector<unsigned char> payload;
  payload.reserve(message.size() + 1);
  payload.push_back(static_cast<unsigned char>(type));
  payload.insert(payload.end(), message.begin(), message.end());

  const bool ok = write_tls_frame(tls_->ssl, payload);
  if (!ok) {
    set_error(91);
  }
  return ok;
}

bool SecureChannel::receive_message(std::vector<uint8_t> &message,
                                    MsgType &type) {
  if (!initialized || !tls_ || !tls_->ssl) {
    return false;
  }

  std::vector<unsigned char> payload;
  if (!read_tls_frame(tls_->ssl, &payload)) {
    set_error(92);
    return false;
  }
  if (payload.empty()) {
    set_error(93);
    return false;
  }

  type = static_cast<MsgType>(payload[0]);
  message.assign(payload.begin() + 1, payload.end());
  return true;
}

bool SecureChannel::send_all(const void *buffer, size_t length) {
  if (tls_ && tls_->ssl) {
    return ssl_write_all(tls_->ssl, buffer, length);
  }
  return transport && transport->send(buffer, length);
}

bool SecureChannel::recv_all(void *buffer, size_t length) {
  if (tls_ && tls_->ssl) {
    return ssl_read_all(tls_->ssl, buffer, length);
  }
  return transport && transport->recv(buffer, length);
}