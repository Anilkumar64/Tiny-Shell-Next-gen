#pragma once
#include "../common/Ast.h"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tsh {

// AstSerializer converts a linked list of AstNode stages to/from a compact
// byte representation suitable for transmission over a SecureChannel.
//
// Wire format (little-endian throughout)
// =======================================
//   [4 bytes] magic  = 0x54534841  ("TSHA")
//   [4 bytes] node_count
//   For each node:
//     [1 byte]  OpType  (cast to uint8_t)
//     [2 bytes] name_len
//     [name_len bytes] name (UTF-8, no null terminator)
//     [2 bytes] arg_count
//     For each arg:
//       [2 bytes] arg_len
//       [arg_len bytes] arg (UTF-8)

class AstSerializer {
public:
  static constexpr uint32_t kMagic = 0x54534841u; // "TSHA"

  // Serialize a pipeline (linked list) to bytes.
  static std::vector<uint8_t> serialize(std::shared_ptr<AstNode> head) {
    std::vector<uint8_t> out;

    // Collect nodes
    std::vector<const AstNode *> nodes;
    for (auto *n = head.get(); n; n = n->next.get())
      nodes.push_back(n);

    write_u32(out, kMagic);
    write_u32(out, static_cast<uint32_t>(nodes.size()));

    for (const auto *n : nodes) {
      out.push_back(static_cast<uint8_t>(n->type));
      write_str16(out, n->name);
      write_u16(out, static_cast<uint16_t>(n->args.size()));
      for (const auto &a : n->args)
        write_str16(out, a);
    }
    return out;
  }

  // Deserialize bytes back into a pipeline.  Returns nullptr on error.
  static std::shared_ptr<AstNode>
  deserialize(const std::vector<uint8_t> &data) {
    std::size_t pos = 0;

    const auto read_u32 = [&]() -> uint32_t {
      if (pos + 4 > data.size())
        throw std::runtime_error("AstSerializer: truncated u32");
      uint32_t v = (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) |
                   ((uint32_t)data[pos + 2] << 16) |
                   ((uint32_t)data[pos + 3] << 24);
      pos += 4;
      return v;
    };
    const auto read_u16 = [&]() -> uint16_t {
      if (pos + 2 > data.size())
        throw std::runtime_error("AstSerializer: truncated u16");
      uint16_t v = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
      pos += 2;
      return v;
    };
    const auto read_str = [&]() -> std::string {
      const uint16_t len = read_u16();
      if (pos + len > data.size())
        throw std::runtime_error("AstSerializer: truncated string");
      std::string s(data.begin() + pos, data.begin() + pos + len);
      pos += len;
      return s;
    };

    if (read_u32() != kMagic)
      return nullptr;
    const uint32_t count = read_u32();
    if (count == 0)
      return nullptr;

    std::shared_ptr<AstNode> head, tail;
    for (uint32_t i = 0; i < count; ++i) {
      if (pos >= data.size())
        return nullptr;
      const auto type = static_cast<OpType>(data[pos++]);
      const std::string name = read_str();
      const uint16_t arg_count = read_u16();
      std::vector<std::string> args;
      args.reserve(arg_count);
      for (uint16_t j = 0; j < arg_count; ++j)
        args.push_back(read_str());

      auto node = std::make_shared<AstNode>(type, name, std::move(args));
      if (!head) {
        head = tail = node;
      } else {
        tail->next = node;
        tail = node;
      }
    }
    return head;
  }

private:
  static void write_u32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
  }
  static void write_u16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
  }
  static void write_str16(std::vector<uint8_t> &out, const std::string &s) {
    write_u16(out, static_cast<uint16_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  }
};

} // namespace tsh
