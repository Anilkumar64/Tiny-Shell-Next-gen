#pragma once

#include <array>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace tsh::spine {

inline std::string random_id(const char *prefix) {
  std::array<unsigned char, 16> bytes{};
  std::random_device rd;
  for (auto &byte : bytes) {
    byte = static_cast<unsigned char>(rd());
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

  std::ostringstream out;
  out << prefix;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out << '-';
    }
    out << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(bytes[i]);
  }
  return out.str();
}

} // namespace tsh::spine
