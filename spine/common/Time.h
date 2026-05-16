#pragma once

#include <chrono>
#include <cstdint>

namespace tsh::spine {

inline std::int64_t now_unix_ms() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

} // namespace tsh::spine
