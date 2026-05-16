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
                // Check if we have enough bytes for type
                if (offset + sizeof(uint8_t) > buffer.size()) {
                    throw std::runtime_error("Invalid buffer: insufficient data for type field");
                }
                
                uint8_t type_val = buffer[offset++];
                
                // Validate type value
                if (type_val != static_cast<uint8_t>(OpType::COMMAND) &&
                    type_val != static_cast<uint8_t>(OpType::FILTER) &&
                    type_val != static_cast<uint8_t>(OpType::MAP) &&
                    type_val != static_cast<uint8_t>(OpType::REDUCE)) {
                    throw std::runtime_error("Invalid AST node type: " + std::to_string(type_val));
                }
                
                OpType type = static_cast<OpType>(type_val);

                // Check if we have enough bytes for name_len
                if (offset + sizeof(uint32_t) > buffer.size()) {
                    throw std::runtime_error("Invalid buffer: insufficient data for name length");
                }
                
                uint32_t name_len;
                std::memcpy(&name_len, &buffer[offset], sizeof(name_len));
                offset += sizeof(name_len);
                
                // Validate name length bounds (reasonable limits)
                const uint32_t MAX_NAME_LEN = 1024;
                if (name_len > MAX_NAME_LEN) {
                    throw std::runtime_error("Name length exceeds maximum allowed size: " + std::to_string(name_len));
                }
                
                // Check if we have enough bytes for name
                if (offset + name_len > buffer.size()) {
                    throw std::runtime_error("Invalid buffer: insufficient data for name field");
                }
                
                std::string name(reinterpret_cast<const char*>(&buffer[offset]), name_len);
                offset += name_len;

                auto node = std::make_shared<AstNode>(type, name);

                // Check if we have enough bytes for args_count
                if (offset + sizeof(uint32_t) > buffer.size()) {
                    throw std::runtime_error("Invalid buffer: insufficient data for args count");
                }
                
                uint32_t args_count;
                std::memcpy(&args_count, &buffer[offset], sizeof(args_count));
                offset += sizeof(args_count);
                
                // Validate args count bounds
                const uint32_t MAX_ARGS_COUNT = 100;
                if (args_count > MAX_ARGS_COUNT) {
                    throw std::runtime_error("Arguments count exceeds maximum allowed: " + std::to_string(args_count));
                }
                
                for (uint32_t i = 0; i < args_count; ++i) {
                    // Check if we have enough bytes for arg_len
                    if (offset + sizeof(uint32_t) > buffer.size()) {
                        throw std::runtime_error("Invalid buffer: insufficient data for argument " + std::to_string(i) + " length");
                    }
                    
                    uint32_t arg_len;
                    std::memcpy(&arg_len, &buffer[offset], sizeof(arg_len));
                    offset += sizeof(arg_len);
                    
                    // Validate arg length bounds
                    const uint32_t MAX_ARG_LEN = 4096;
                    if (arg_len > MAX_ARG_LEN) {
                        throw std::runtime_error("Argument " + std::to_string(i) + " length exceeds maximum allowed: " + std::to_string(arg_len));
                    }
                    
                    // Check if we have enough bytes for arg
                    if (offset + arg_len > buffer.size()) {
                        throw std::runtime_error("Invalid buffer: insufficient data for argument " + std::to_string(i));
                    }
                    
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
