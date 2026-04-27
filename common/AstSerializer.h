#pragma once
#include "Ast.h"
#include <vector>
#include <string>
#include <memory>
#include <cstring>

namespace tsh {
    class AstSerializer {
    public:
        // Serializes the AST into a compact binary format
        static std::vector<uint8_t> serialize(std::shared_ptr<AstNode> head) {
            std::vector<uint8_t> buffer;
            auto current = head;
            while (current) {
                // Format: [Type (1b)] [NameLen (4b)] [Name (Nb)] [ArgsCount (4b)] [Args...]
                uint8_t type = static_cast<uint8_t>(current->type);
                buffer.push_back(type);

                uint32_t name_len = current->name.length();
                append_bytes(buffer, &name_len, sizeof(name_len));
                append_bytes(buffer, current->name.data(), name_len);

                uint32_t args_count = current->args.size();
                append_bytes(buffer, &args_count, sizeof(args_count));
                for (const auto& arg : current->args) {
                    uint32_t arg_len = arg.length();
                    append_bytes(buffer, &arg_len, sizeof(arg_len));
                    append_bytes(buffer, arg.data(), arg_len);
                }
                current = current->next;
            }
            return buffer;
        }

        static std::shared_ptr<AstNode> deserialize(const std::vector<uint8_t>& buffer) {
            if (buffer.empty()) return nullptr;
            
            size_t offset = 0;
            std::shared_ptr<AstNode> head = nullptr;
            std::shared_ptr<AstNode> tail = nullptr;

            while (offset < buffer.size()) {
                uint8_t type_val = buffer[offset++];
                OpType type = static_cast<OpType>(type_val);

                uint32_t name_len;
                std::memcpy(&name_len, &buffer[offset], sizeof(name_len));
                offset += sizeof(name_len);
                std::string name(reinterpret_cast<const char*>(&buffer[offset]), name_len);
                offset += name_len;

                auto node = std::make_shared<AstNode>(type, name);

                uint32_t args_count;
                std::memcpy(&args_count, &buffer[offset], sizeof(args_count));
                offset += sizeof(args_count);
                for (uint32_t i = 0; i < args_count; ++i) {
                    uint32_t arg_len;
                    std::memcpy(&arg_len, &buffer[offset], sizeof(arg_len));
                    offset += sizeof(arg_len);
                    std::string arg(reinterpret_cast<const char*>(&buffer[offset]), arg_len);
                    offset += arg_len;
                    node->args.push_back(arg);
                }

                if (!head) head = node;
                if (tail) tail->next = node;
                tail = node;
            }
            return head;
        }

    private:
        static void append_bytes(std::vector<uint8_t>& buffer, const void* data, size_t len) {
            const uint8_t* ptr = static_cast<const uint8_t*>(data);
            buffer.insert(buffer.end(), ptr, ptr + len);
        }
    };
}
