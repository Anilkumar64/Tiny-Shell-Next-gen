#pragma once

#include "tinyshell/v1/spine.pb.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tsh::spine {

class EventStore {
public:
  virtual ~EventStore() = default;
  virtual void initialize() = 0;
  virtual void insert_job(const tinyshell::v1::SignedJobSpec &signed_spec,
                          const std::string &state) = 0;
  virtual void update_job_state(const std::string &job_id,
                                const std::string &state) = 0;
  virtual void append_event(tinyshell::v1::JobEvent &event) = 0;
  virtual std::vector<tinyshell::v1::JobEvent>
  load_events_after(const std::string &job_id, std::uint64_t after_sequence,
                    std::size_t limit) = 0;
  virtual std::string job_state(const std::string &job_id) = 0;
  virtual void upsert_agent(const tinyshell::v1::AgentHello &hello) = 0;
  virtual void mark_agent_heartbeat(const std::string &agent_id) = 0;
};

std::unique_ptr<EventStore> make_wal_event_store(const std::string &data_dir);

} // namespace tsh::spine
