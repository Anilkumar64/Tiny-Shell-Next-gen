#pragma once
#include "../common/Ast.h"
#include <functional>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tsh {
class SemanticDedup {
  std::mutex mtx;
  static constexpr size_t MAX_CACHE_SIZE = 1000;
  // BUG-9 FIX: Commands like ps/uptime/df return live data that changes every
  // second.  Without a TTL the first result is served indefinitely until LRU
  // eviction (only at 1000 entries).  We now expire entries after 5 seconds.
  static constexpr std::chrono::seconds CACHE_TTL{5};
  std::list<std::size_t> lru_order;
  struct CacheEntry {
    std::string response;
    std::list<std::size_t>::iterator iterator;
    std::chrono::steady_clock::time_point stored_at;
  };
  std::unordered_map<std::size_t, CacheEntry> result_cache;

  // A-1: Merkle-DAG Equivalence Hashing for AST graphs
  std::size_t hash_ast(std::shared_ptr<AstNode> head) {
    std::size_t h = 0;
    auto current = head;
    std::hash<std::string> str_hasher;
    std::hash<int> int_hasher;

    while (current) {
      // BUG-25 FIX: Include the node's OpType in the hash.  Previously only
      // the name was hashed, so filter(cpu > 80) and map(cpu > 80) produced
      // identical hashes and a cached FILTER result would be incorrectly
      // returned for a MAP query (or vice-versa).
      h ^= int_hasher(static_cast<int>(current->type)) + 0x9e3779b9 + (h << 6) +
           (h >> 2);
      h ^= str_hasher(current->name) + 0x9e3779b9 + (h << 6) + (h >> 2);
      for (const auto &arg : current->args) {
        h ^= str_hasher(arg) + 0x9e3779b9 + (h << 6) + (h >> 2);
      }
      current = current->next;
    }
    return h;
  }

public:
  // Returns true if the AST was recently executed, returning the cached data
  // instantly.
  bool try_dedup(std::shared_ptr<AstNode> head, std::string &response) {
    std::lock_guard<std::mutex> lock(mtx);
    std::size_t h = hash_ast(head);
    auto it = result_cache.find(h);
    if (it != result_cache.end()) {
      // BUG-9 FIX: Honour TTL — treat expired entries as cache misses.
      const auto age = std::chrono::steady_clock::now() - it->second.stored_at;
      if (age > CACHE_TTL) {
        lru_order.erase(it->second.iterator);
        result_cache.erase(it);
        return false;
      }
      lru_order.splice(lru_order.begin(), lru_order, it->second.iterator);
      response = it->second.response;
      std::cout << "[SemanticDedup] C-1 Cache Hit: Reusing previous AST stream "
                   "results! (Hash: "
                << h << ")\n";
      return true;
    }
    return false;
  }

  void store_result(std::shared_ptr<AstNode> head,
                    const std::string &response) {
    std::lock_guard<std::mutex> lock(mtx);
    std::size_t h = hash_ast(head);
    // FIX[MEM-1]: Bound cache with LRU eviction to prevent production OOM.
    auto existing = result_cache.find(h);
    if (existing != result_cache.end()) {
      existing->second.response = response;
      lru_order.splice(lru_order.begin(), lru_order, existing->second.iterator);
      return;
    }
    if (result_cache.size() >= MAX_CACHE_SIZE && !lru_order.empty()) {
      const auto evict = lru_order.back();
      lru_order.pop_back();
      result_cache.erase(evict);
    }
    lru_order.push_front(h);
    result_cache.emplace(h, CacheEntry{response, lru_order.begin(),
                                       std::chrono::steady_clock::now()});
  }

  static SemanticDedup &instance() {
    static SemanticDedup cache;
    return cache;
  }
};
} // namespace tsh