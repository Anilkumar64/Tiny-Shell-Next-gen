#include "JobSigner.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdexcept>
#include <vector>

namespace tsh::spine {
namespace {

std::string hmac_sha256(const std::string &secret, const std::string &payload) {
  unsigned int len = EVP_MAX_MD_SIZE;
  std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
  if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char *>(payload.data()),
            payload.size(), digest.data(), &len)) {
    throw std::runtime_error("HMAC-SHA256 signing failed");
  }
  return std::string(reinterpret_cast<const char *>(digest.data()), len);
}

bool constant_time_equal(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

} // namespace

JobSigner::JobSigner(std::string key_id, std::string secret)
    : key_id_(std::move(key_id)), secret_(std::move(secret)) {
  if (key_id_.empty()) {
    throw std::invalid_argument("job signing key id is required");
  }
  if (secret_.size() < 32) {
    throw std::invalid_argument("job signing secret must be at least 32 bytes");
  }
}

tinyshell::v1::SignedJobSpec JobSigner::sign(
    const tinyshell::v1::JobSpec &spec) const {
  std::string payload;
  if (!spec.SerializeToString(&payload)) {
    throw std::runtime_error("failed to serialize JobSpec for signing");
  }

  tinyshell::v1::SignedJobSpec signed_spec;
  *signed_spec.mutable_spec() = spec;
  signed_spec.set_key_id(key_id_);
  signed_spec.set_signature(hmac_sha256(secret_, payload));
  return signed_spec;
}

bool JobSigner::verify(const tinyshell::v1::SignedJobSpec &signed_spec) const {
  if (signed_spec.key_id() != key_id_ || !signed_spec.has_spec()) {
    return false;
  }
  std::string payload;
  if (!signed_spec.spec().SerializeToString(&payload)) {
    return false;
  }
  return constant_time_equal(signed_spec.signature(),
                             hmac_sha256(secret_, payload));
}

} // namespace tsh::spine
