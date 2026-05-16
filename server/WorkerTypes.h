#pragma once

#include <string>

namespace tsh {

struct RemoteNode {
  std::string host;
  int port = 0;
  std::string id;
};

inline std::string worker_node_id(const RemoteNode &node) {
  if (!node.id.empty())
    return node.id;
  return node.host + ":" + std::to_string(node.port);
}

} // namespace tsh
