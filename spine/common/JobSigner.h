#pragma once

#include "tinyshell/v1/spine.pb.h"

#include <string>

namespace tsh::spine {

class JobSigner {
public:
  JobSigner(std::string key_id, std::string secret);

  tinyshell::v1::SignedJobSpec sign(const tinyshell::v1::JobSpec &spec) const;
  bool verify(const tinyshell::v1::SignedJobSpec &signed_spec) const;

  const std::string &key_id() const { return key_id_; }

private:
  std::string key_id_;
  std::string secret_;
};

} // namespace tsh::spine
