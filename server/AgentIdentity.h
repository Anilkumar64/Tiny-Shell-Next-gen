#pragma once
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/evp.h> // FIX: for compute_fingerprint (SHA-256)
#include <random>
#include <sstream>
#include <string>
#include <unistd.h> // FIX: gethostname() lives here, not in C++ standard headers

namespace tsh {

// AgentMetadata holds the identity information for this TinyShell agent
// instance.  It is generated once on first start and persisted to
// ~/.tsh/agent_identity so it survives restarts.
struct AgentMetadata {
  std::string agent_id;     // UUIDv4-like identifier, unique per installation
  std::string fingerprint;  // FIX: SHA-256 hex digest of agent_id (was missing)
  std::string pairing_code; // Short human-friendly code used for controller
                            // registration
  std::string hostname;
  std::string created_at; // ISO-8601 timestamp of first start
};

// AgentIdentity manages stable, persistent agent identity.
//
// On first run it generates a random agent_id and pairing_code and writes
// them to ~/.tsh/agent_identity.  On subsequent runs the same values are
// re-loaded from disk so the identity survives server restarts.
class AgentIdentity {
public:
  // Load (or generate) persistent metadata.  Thread-safe within a single
  // process; file I/O is guarded by a check-then-create pattern.
  static AgentMetadata load_metadata() {
    const auto path = identity_path();

    AgentMetadata meta;
    if (std::filesystem::exists(path)) {
      meta = read_from_file(path);
      if (!meta.agent_id.empty() && !meta.pairing_code.empty()) {
        // FIX: recompute fingerprint if the persisted file predates
        // the field (backwards-compatible with older identity files).
        if (meta.fingerprint.empty())
          meta.fingerprint = compute_fingerprint(meta.agent_id);
        return meta;
      }
    }

    // First run — generate fresh identity.
    meta.agent_id = generate_uuid();
    meta.pairing_code = generate_pairing_code();
    meta.hostname = get_hostname();
    meta.created_at = iso_timestamp();
    meta.fingerprint = compute_fingerprint(meta.agent_id); // FIX: populate

    write_to_file(path, meta);
    std::cout << "[AgentIdentity] New agent identity generated.\n";
    return meta;
  }

  // Format metadata as a human-readable string (for the "agent-info" command).
  // FIX: labels now match what the test suite expects:
  //   "Agent ID:"     (was "Agent ID    : ")
  //   "Fingerprint:"  (was absent)
  //   "Hostname:"     (was "Hostname    : ")
  //   "Pairing code:" (was "Pairing Code: " — wrong capitalisation)
  static std::string format_human(const AgentMetadata &m) {
    std::ostringstream ss;
    ss << "Agent Identity\n";
    ss << "==============\n";
    ss << "Agent ID: " << m.agent_id << "\n";
    ss << "Fingerprint: " << m.fingerprint << "\n";
    ss << "Hostname: " << m.hostname << "\n";
    ss << "Pairing code: " << m.pairing_code << "\n";
    ss << "Created At: " << m.created_at << "\n";
    return ss.str();
  }

  static std::string new_uuid() {
    // BUG: callers used libuuid despite an existing portable UUID generator.
    // FIX: expose a narrow public wrapper around the local UUIDv4 generator.
    return generate_uuid();
  }

private:
  static std::filesystem::path identity_path() {
    const char *home = std::getenv("HOME");
    if (!home)
      home = "/tmp";
    const auto dir = std::filesystem::path(home) / ".tsh";
    std::filesystem::create_directories(dir);
    return dir / "agent_identity";
  }

  static AgentMetadata read_from_file(const std::filesystem::path &path) {
    AgentMetadata m;
    std::ifstream f(path);
    if (!f.is_open())
      return m;
    std::string line;
    while (std::getline(f, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      const auto key = line.substr(0, eq);
      const auto val = line.substr(eq + 1);
      if (key == "agent_id")
        m.agent_id = val;
      else if (key == "fingerprint")
        m.fingerprint = val; // FIX: read field
      else if (key == "pairing_code")
        m.pairing_code = val;
      else if (key == "hostname")
        m.hostname = val;
      else if (key == "created_at")
        m.created_at = val;
    }
    return m;
  }

  static void write_to_file(const std::filesystem::path &path,
                            const AgentMetadata &m) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) {
      std::cerr << "[AgentIdentity] Warning: could not persist identity to "
                << path << "\n";
      return;
    }
    f << "agent_id=" << m.agent_id << "\n"
      << "fingerprint=" << m.fingerprint << "\n" // FIX: persist field
      << "pairing_code=" << m.pairing_code << "\n"
      << "hostname=" << m.hostname << "\n"
      << "created_at=" << m.created_at << "\n";
  }

  // FIX: SHA-256 hex digest of the agent_id, used as a stable fingerprint
  // (e.g. for display / TOFU pinning).  Uses OpenSSL EVP which is already
  // a project-wide dependency.
  static std::string compute_fingerprint(const std::string &agent_id) {
    unsigned char digest[32];
    unsigned int dlen = sizeof(digest);
    EVP_Digest(reinterpret_cast<const unsigned char *>(agent_id.data()),
               agent_id.size(), digest, &dlen, EVP_sha256(), nullptr);
    std::ostringstream ss;
    for (unsigned int i = 0; i < dlen; ++i)
      ss << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<int>(digest[i]);
    return ss.str();
  }

  // Generate a random UUIDv4-style identifier.
  static std::string generate_uuid() {
    std::random_device rd;
    std::mt19937_64 eng(rd());
    std::uniform_int_distribution<uint64_t> dist;

    const uint64_t hi = dist(eng);
    const uint64_t lo = dist(eng);

    // Force version 4 and variant bits.
    const uint64_t h = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    const uint64_t l = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(8) << (h >> 32) << '-'
       << std::setw(4) << ((h >> 16) & 0xFFFF) << '-' << std::setw(4)
       << (h & 0xFFFF) << '-' << std::setw(4) << (l >> 48) << '-'
       << std::setw(12) << (l & 0x0000FFFFFFFFFFFFULL);
    return ss.str();
  }

  // Generate a short alphanumeric pairing code (8 characters).
  static std::string generate_pairing_code() {
    static constexpr std::string_view kAlpha =
        "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // omit I, O, 0, 1 (ambiguous)
    std::random_device rd;
    std::mt19937 eng(rd());
    std::uniform_int_distribution<size_t> dist(0, kAlpha.size() - 1);
    std::string code;
    code.reserve(8);
    for (int i = 0; i < 8; ++i)
      code += kAlpha[dist(eng)];
    return code;
  }

  static std::string get_hostname() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0)
      return buf; // FIX: <unistd.h> provides this
    return "unknown";
  }

  static std::string iso_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
  }
};

} // namespace tsh
