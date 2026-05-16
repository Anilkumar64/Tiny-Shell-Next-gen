#pragma once
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_set>

namespace tsh {

// ControllerTrust maintains a persistent allowlist of trusted controller IDs.
//
// Trust is granted through an interactive registration flow:
//   1. Controller announces its identity with "controller-hello <id>".
//   2. Controller provides the agent's pairing code via "register <code>".
//   3. approve_interactively() prompts the host user for confirmation.
//   4. On approval, trust() persists the controller ID so it survives restarts.
//
// All methods are static for ergonomics; internal state is protected by a
// process-wide mutex so concurrent client handlers are safe.

class ControllerTrust {
public:
  // Returns true if controller_id has previously been trusted.
  static bool is_trusted(const std::string &controller_id) {
    std::lock_guard<std::mutex> lock(mutex());
    return trusted_set().count(controller_id) > 0;
  }

  // Persist trust for controller_id.  Appends to the trust store on disk
  // and updates the in-process cache.
  static void trust(const std::string &controller_id) {
    std::lock_guard<std::mutex> lock(mutex());
    if (trusted_set().insert(controller_id).second) {
      // Newly inserted — write to disk.
      std::ofstream f(trust_path(), std::ios::app);
      if (f.is_open()) {
        f << controller_id << "\n";
      } else {
        std::cerr << "[ControllerTrust] Warning: could not persist trust "
                     "entry for: "
                  << controller_id << "\n";
      }
      std::cout << "[ControllerTrust] Controller trusted: " << controller_id
                << "\n";
    }
  }

  // Revoke trust for controller_id (in-process and on-disk).
  static void revoke(const std::string &controller_id) {
    std::lock_guard<std::mutex> lock(mutex());
    trusted_set().erase(controller_id);
    rewrite_trust_file();
    std::cout << "[ControllerTrust] Trust revoked for: " << controller_id
              << "\n";
  }

  // Prompt the host user interactively and return true only if they confirm.
  // agent_id is shown for context so the user knows which agent is being
  // paired.
  static bool approve_interactively(const std::string &controller_id,
                                    const std::string &agent_id) {
    // In non-interactive deployments (CI, containers) auto-deny to avoid
    // hanging.  Set TSH_AUTO_TRUST=1 only in trusted environments.
    if (const char *env = std::getenv("TSH_AUTO_TRUST");
        env && std::string(env) == "1") {
      std::cout << "[ControllerTrust] TSH_AUTO_TRUST=1 — auto-approving "
                << controller_id << "\n";
      return true;
    }

    std::cout << "\n[TinyShell] Controller registration request\n"
              << "  Agent ID    : " << agent_id << "\n"
              << "  Controller  : " << controller_id << "\n"
              << "Allow this controller? [y/N] " << std::flush;

    std::string answer;
    if (!std::getline(std::cin, answer))
      return false;
    const bool approved =
        (!answer.empty() && (answer[0] == 'y' || answer[0] == 'Y'));
    if (!approved) {
      std::cout << "[ControllerTrust] Registration denied by user.\n";
    }
    return approved;
  }

private:
  static std::mutex &mutex() {
    static std::mutex m;
    return m;
  }

  // In-process cache, loaded lazily from disk on first access.
  static std::unordered_set<std::string> &trusted_set() {
    static std::unordered_set<std::string> s = load_from_disk();
    return s;
  }

  static std::filesystem::path trust_path() {
    const char *home = std::getenv("HOME");
    if (!home)
      home = "/tmp";
    const auto dir = std::filesystem::path(home) / ".tsh";
    std::filesystem::create_directories(dir);
    return dir / "trusted_controllers";
  }

  static std::unordered_set<std::string> load_from_disk() {
    std::unordered_set<std::string> result;
    std::ifstream f(trust_path());
    if (!f.is_open())
      return result;
    std::string line;
    while (std::getline(f, line)) {
      // Strip trailing whitespace / CR
      while (!line.empty() &&
             (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
        line.pop_back();
      if (!line.empty())
        result.insert(line);
    }
    return result;
  }

  // Rewrite the trust file from the in-process set (called after revocation).
  // Must be called with mutex held.
  static void rewrite_trust_file() {
    std::ofstream f(trust_path(), std::ios::trunc);
    if (!f.is_open()) {
      std::cerr << "[ControllerTrust] Warning: could not rewrite trust file.\n";
      return;
    }
    for (const auto &id : trusted_set()) {
      f << id << "\n";
    }
  }
};

} // namespace tsh