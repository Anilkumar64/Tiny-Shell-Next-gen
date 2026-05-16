#include "EventBus.h"
#include "JobLifecycle.h"

#include <chrono>
#include <cstddef>

namespace tsh::spine {

std::shared_ptr<EventBus::Slot> EventBus::slot_for(const std::string &job_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto &slot = slots_[job_id];
  if (!slot) {
    slot = std::make_shared<Slot>();
  }
  return slot;
}

std::shared_ptr<EventBus::Slot> EventBus::find_slot(const std::string &job_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = slots_.find(job_id);
  if (it == slots_.end()) {
    return nullptr;
  }
  return it->second;
}

void EventBus::publish(const tinyshell::v1::JobEvent &event) {
  {
    auto slot = slot_for(event.job_id());
    std::lock_guard<std::mutex> lock(slot->mutex);
    const auto now = std::chrono::steady_clock::now();
    slot->last_access = now;
    slot->events.push_back(event);
    if (slot->events.size() > kMaxLiveEventsPerJob) {
      slot->events.erase(slot->events.begin(),
                         slot->events.begin() +
                             static_cast<std::ptrdiff_t>(slot->events.size() -
                                                         kMaxLiveEventsPerJob));
    }
    if (is_terminal_state(event.state())) {
      slot->terminal = true;
      slot->terminal_at = now;
    }
    slot->cv.notify_all();
  }
}

std::vector<tinyshell::v1::JobEvent>
EventBus::snapshot(const std::string &job_id) {
  std::shared_ptr<Slot> slot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = slots_.find(job_id);
    if (it != slots_.end()) {
      slot = it->second;
    }
  }
  if (!slot) {
    return {};
  }
  std::lock_guard<std::mutex> lock(slot->mutex);
  slot->last_access = std::chrono::steady_clock::now();
  return slot->events;
}

bool EventBus::wait_for_next(const std::string &job_id,
                             std::uint64_t after_sequence,
                             tinyshell::v1::JobEvent *event) {
  auto slot = find_slot(job_id);
  if (!slot) {
    return false;
  }
  std::unique_lock<std::mutex> lock(slot->mutex);
  slot->last_access = std::chrono::steady_clock::now();
  if (!slot->cv.wait_for(lock, std::chrono::seconds(1), [&] {
        return !slot->events.empty() &&
               slot->events.back().sequence() > after_sequence;
      })) {
    return false;
  }

  for (const auto &candidate : slot->events) {
    if (candidate.sequence() > after_sequence) {
      *event = candidate;
      return true;
    }
  }
  return false;
}

void EventBus::prune_terminal_older_than(std::chrono::milliseconds age) {
  const auto cutoff = std::chrono::steady_clock::now() - age;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = slots_.begin(); it != slots_.end();) {
    auto slot = it->second;
    bool erase = false;
    {
      std::lock_guard<std::mutex> slot_lock(slot->mutex);
      erase = slot->terminal && slot->terminal_at < cutoff &&
              slot->last_access < cutoff;
    }
    if (erase) {
      it = slots_.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace tsh::spine
