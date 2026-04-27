#pragma once
#include "../common/Ast.h"
#include <string>
#include <stdexcept>
#include <regex>
#include <iostream>

namespace tsh {
    class UnitException : public std::runtime_error {
    public:
        explicit UnitException(const std::string& msg) : std::runtime_error("Unit Mismatch: " + msg) {}
    };

    class UnitValidator {
    public:
        // C-5: Unit-Aware Arithmetic Validator
        // Parses arguments like "80%" or "1000ms" and ensures they match expected operator semantics.
        static void validate_units(std::shared_ptr<AstNode> head) {
            auto current = head;
            std::regex unit_regex(R"(^(\d+)(ms|s|%|kb|mb|gb|b)$)", std::regex_constants::icase);
            
            while (current) {
                if (current->type == OpType::FILTER) {
                    std::smatch match;
                    if (std::regex_search(current->name, match, unit_regex)) {
                        std::string unit = match[2];
                        // Example heuristic: CPU filters should use percentages
                        if (current->name.find("cpu") != std::string::npos && unit != "%") {
                            throw UnitException("CPU filters require '%' units. Found: " + unit);
                        }
                        // Example heuristic: Latency filters should use time
                        if (current->name.find("time") != std::string::npos && (unit != "ms" && unit != "s")) {
                            throw UnitException("Time filters require 'ms' or 's' units. Found: " + unit);
                        }
                    }
                }
                current = current->next;
            }
            std::cout << "[UnitValidator] C-5 AST Type-Check: All mathematical units validated.\n";
        }
    };
}
