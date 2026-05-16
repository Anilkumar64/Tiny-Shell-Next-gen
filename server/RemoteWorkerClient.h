#pragma once

#include "WorkerTypes.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tsh {

class RemoteWorkerClient {
public:
  static std::string execute_node_stream(
      const std::vector<uint8_t> &serialized_ast, const RemoteNode &node,
      const std::function<void(const std::string &, bool)> &on_chunk);
};

} // namespace tsh
