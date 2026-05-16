#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace tsh::spine {

struct ValidatedCommand {
  std::string absolute_path;
  std::vector<std::string> args;
};

class PolicyError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class CommandPolicy {
public:
  ValidatedCommand validate(const std::string &command,
                            const std::vector<std::string> &args) const;
};

} // namespace tsh::spine
