#pragma once
#include "../common/Ast.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <iostream>

namespace tsh {
    class SemanticDedup {
        std::mutex mtx;
        std::unordered_map<std::size_t, std::string> result_cache;

        // A-1: Merkle-DAG Equivalence Hashing for AST graphs
        std::size_t hash_ast(std::shared_ptr<AstNode> head) {
            std::size_t h = 0;
            auto current = head;
            std::hash<std::string> str_hasher;
            
            while (current) {
                h ^= str_hasher(current->name) + 0x9e3779b9 + (h << 6) + (h >> 2);
                for (const auto& arg : current->args) {
                    h ^= str_hasher(arg) + 0x9e3779b9 + (h << 6) + (h >> 2);
                }
                current = current->next;
            }
            return h;
        }

    public:
        // Returns true if the AST was recently executed, returning the cached data instantly.
        bool try_dedup(std::shared_ptr<AstNode> head, std::string& response) {
            std::lock_guard<std::mutex> lock(mtx);
            std::size_t h = hash_ast(head);
            if (result_cache.find(h) != result_cache.end()) {
                response = result_cache[h];
                std::cout << "[SemanticDedup] C-1 Cache Hit: Reusing previous AST stream results! (Hash: " << h << ")\n";
                return true;
            }
            return false;
        }

        void store_result(std::shared_ptr<AstNode> head, const std::string& response) {
            std::lock_guard<std::mutex> lock(mtx);
            std::size_t h = hash_ast(head);
            result_cache[h] = response;
        }
        
        static SemanticDedup& instance() {
            static SemanticDedup cache;
            return cache;
        }
    };
}
