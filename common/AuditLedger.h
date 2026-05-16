#pragma once
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/sha.h>
#include <sstream> // BUG-26 FIX: was missing; required for std::stringstream used in commit()
#include <string>
#include <vector>

class AuditLedger {
public:
  static void commit(const std::string &command, const std::string &output) {
    std::cout << "[Audit] Appending legacy local audit digest...\n";

    std::string record = command + "|" + output;
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t *>(record.data()), record.size(),
           hash);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
      ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }

    // Legacy compatibility helper only. Production execution-spine audit must
    // use structured events, durable storage, signed specs, and append-only
    // integrity controls.
    std::ofstream ledger("tsh_audit.log", std::ios::app);
    ledger << "CMD: " << command << " | HASH: " << ss.str() << "\n";
    ledger.close();
  }
};
