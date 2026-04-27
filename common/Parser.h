#pragma once
#include "Ast.h"
#include <string>
#include <vector>
#include <sstream>
#include <memory>

namespace tsh {

    class Parser {
    public:
        // Basic parser: splits by '|', identifies commands and filters.
        // Syntax example: "ps | filter(cpu > 80)"
        static std::shared_ptr<AstNode> parse_pipeline(const std::string& input) {
            std::vector<std::string> tokens;
            std::stringstream ss(input);
            std::string item;
            
            while (std::getline(ss, item, '|')) {
                // Trim whitespace
                size_t start = item.find_first_not_of(" \t");
                size_t end = item.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    tokens.push_back(item.substr(start, end - start + 1));
                }
            }

            if (tokens.empty()) return nullptr;

            std::shared_ptr<AstNode> head = nullptr;
            std::shared_ptr<AstNode> current = nullptr;

            for (const auto& token : tokens) {
                std::shared_ptr<AstNode> node;
                
                if (token.find("filter(") == 0) {
                    // Extract expression inside filter()
                    size_t start = token.find('(') + 1;
                    size_t end = token.find(')');
                    std::string expr = token.substr(start, end - start);
                    node = std::make_shared<AstNode>(OpType::FILTER, expr);
                } else if (token.find("map(") == 0) {
                    size_t start = token.find('(') + 1;
                    size_t end = token.find(')');
                    std::string expr = token.substr(start, end - start);
                    node = std::make_shared<AstNode>(OpType::MAP, expr);
                } else if (token.find("count()") == 0) {
                    node = std::make_shared<AstNode>(OpType::REDUCE, "count");
                } else {
                    // Base command
                    std::stringstream cmd_ss(token);
                    std::string cmd_name;
                    cmd_ss >> cmd_name;
                    
                    node = std::make_shared<AstNode>(OpType::COMMAND, cmd_name);
                    std::string arg;
                    while (cmd_ss >> arg) {
                        node->args.push_back(arg);
                    }
                }

                if (!head) {
                    head = node;
                    current = head;
                } else {
                    current->next = node;
                    current = node;
                }
            }

            return head;
        }
    };

}
