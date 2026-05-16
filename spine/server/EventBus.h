#pragma once

#include "tinyshell/v1/spine.pb.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tsh::spine {

class EventBus {
public:
  void publish(const tinyshell::v1::JobEvent &event);
  std::vector<tinyshell::v1::JobEvent> snapshot(const std::string &job_id);
  bool wait_for_next(const std::string &job_id, std::uint64_t after_sequence,
                     tinyshell::v1::JobEvent *event);
  void prune_terminal_older_than(std::chrono::milliseconds age);

private:
  static constexpr std::size_t kMaxLiveEventsPerJob = 4096;
  struct Slot {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<tinyshell::v1::JobEvent> events;
    std::chrono::steady_clock::time_point last_access =
        std::chrono::steady_clock::now();
    bool terminal = false;
    std::chrono::steady_clock::time_point terminal_at{};
  };

  std::shared_ptr<Slot> slot_for(const std::string &job_id);
  std::shared_ptr<Slot> find_slot(const std::string &job_id);

  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Slot>> slots_;
};

} // namespace tsh::spine
