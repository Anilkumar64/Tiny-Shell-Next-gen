#pragma once
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <fstream>
#include <iostream>

namespace tsh {
    struct ZkLogEntry {
        std::string timestamp;
        std::string user_id;
        std::string commitment; // HMAC(Secret, Command)
        std::string proof;      // SHA256(Commitment + PrevHash)
    };

    class ZkAuditTrail {
        std::string prev_hash = "00000000000000000000000000000000";
        std::string cluster_secret = "tsh_zkp_master_v3_2026";
        std::string log_path;

    public:
        explicit ZkAuditTrail(std::string path = "tsh_zk_audit.ledger") : log_path(std::move(path)) {}

        // F-2: Generates a ZK-style commitment and sequence proof
        void log_secure_event(const std::string& user, const std::string& command) {
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%dT%H:%M:%S");

            // 1. Generate Commitment: HMAC-SHA256(ClusterSecret, Command)
            unsigned char c_hash[SHA256_DIGEST_LENGTH];
            unsigned int c_len = SHA256_DIGEST_LENGTH;
            HMAC(EVP_sha256(), cluster_secret.c_str(), cluster_secret.length(),
                 reinterpret_cast<const unsigned char*>(command.c_str()), command.length(),
                 c_hash, &c_len);
            
            std::string commitment = to_hex(c_hash, c_len);

            // 2. Generate Sequence Proof: SHA256(Commitment + PrevHash)
            std::string combined = commitment + prev_hash;
            unsigned char p_hash[SHA256_DIGEST_LENGTH];
            SHA256(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.length(), p_hash);
            
            std::string proof = to_hex(p_hash, SHA256_DIGEST_LENGTH);
            prev_hash = proof;

            // 3. Persist to immutable-style ledger
            std::ofstream ledger(log_path, std::ios::app);
            ledger << ss.str() << " | USER:" << user << " | COMMITMENT:" << commitment << " | PROOF:" << proof << "\n";
            std::cout << "[ZK-Audit] F-2 Proof Generated: " << proof.substr(0, 12) << "...\n";
        }

    private:
        static std::string to_hex(unsigned char* hash, size_t len) {
            std::stringstream hex_ss;
            for(size_t i = 0; i < len; i++) {
                hex_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
            }
            return hex_ss.str();
        }
    };
}
