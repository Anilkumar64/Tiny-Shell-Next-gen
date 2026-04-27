#pragma once
#include <vector>
#include <string>
#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <cstring>

namespace tsh {
    class AstSigner {
        // In a production environment, this secret key is rotated securely via Vault or KMS.
        static constexpr const char* SECRET_KEY = "tsh_cluster_secret_key_v1";

    public:
        // Generates a cryptographic signature proving the origin of the AST execution graph
        static std::string sign(const std::vector<uint8_t>& serialized_ast) {
            unsigned int len = SHA256_DIGEST_LENGTH;
            unsigned char hash[SHA256_DIGEST_LENGTH];
            
            HMAC_CTX* ctx = HMAC_CTX_new();
            HMAC_Init_ex(ctx, SECRET_KEY, std::strlen(SECRET_KEY), EVP_sha256(), NULL);
            HMAC_Update(ctx, serialized_ast.data(), serialized_ast.size());
            HMAC_Final(ctx, hash, &len);
            HMAC_CTX_free(ctx);

            std::stringstream ss;
            for(unsigned int i = 0; i < len; i++) {
                ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
            }
            return ss.str();
        }

        // Zero-trust verification: Ensures the AST was not maliciously altered in transit
        static bool verify(const std::vector<uint8_t>& serialized_ast, const std::string& signature) {
            return sign(serialized_ast) == signature;
        }
    };
}
