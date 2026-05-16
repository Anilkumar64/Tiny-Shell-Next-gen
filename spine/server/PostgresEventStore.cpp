#include "EventStore.h"
#include "JobLifecycle.h"

#include <google/protobuf/util/json_util.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace tsh::spine {
namespace {

constexpr const char *kWalName = "events.wal";
constexpr const char *kWalMagic = "TSH_EVENT_WAL_V1\n";
constexpr char kJobRecord = 'J';
constexpr char kEventRecord = 'E';
constexpr std::size_t kMaxRetainedEventsPerJob = 4096;
constexpr std::size_t kMaxRetainedAuditRows = 100000;
constexpr std::size_t kDefaultMaxRetainedJobs = 100000;
constexpr std::uint64_t kDefaultCompactBytes = 64ULL * 1024ULL * 1024ULL;

std::string env_string(const char *name, const std::string &fallback = {}) {
  if (const char *value = std::getenv(name)) {
    return value;
  }
  return fallback;
}

std::uint64_t env_u64(const char *name, std::uint64_t fallback) {
  const auto value = env_string(name);
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stoull(value);
  } catch (...) {
    return fallback;
  }
}

std::filesystem::path default_store_dir() {
  if (const auto configured = env_string("TSH_EVENT_STORE_DIR");
      !configured.empty()) {
    return configured;
  }
  return std::filesystem::path(env_string("HOME", "/tmp")) /
         ".local/state/tinyshell/spine";
}

std::string sha256_hex(const std::string &data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_len = 0;
  // BUG: SHA256() is deprecated and unavailable in some hardened providers.
  // FIX: use EVP_Digest with EVP_sha256() through OpenSSL providers.
  if (EVP_Digest(data.data(), data.size(), digest.data(), &digest_len,
                 EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("failed to compute SHA-256 digest");
  }
  std::ostringstream out;
  for (unsigned int i = 0; i < digest_len; ++i) {
    const unsigned char b = digest[i];
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  return out.str();
}

void append_u32(std::string &out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((value >> (i * 8)) & 0xffU));
  }
}

std::uint32_t read_u32(const char *data) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i]))
             << (i * 8);
  }
  return value;
}

void write_all(int fd, const char *data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const ssize_t n = write(fd, data + written, size - written);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      throw std::runtime_error("failed to write event WAL: " +
                               std::string(std::strerror(errno)));
    }
    written += static_cast<std::size_t>(n);
  }
}

void fsync_fd(int fd, const char *label) {
  while (fsync(fd) != 0) {
    if (errno == EINTR) {
      continue;
    }
    throw std::runtime_error(std::string("failed to fsync ") + label + ": " +
                             std::strerror(errno));
  }
}

void fsync_directory(const std::filesystem::path &path) {
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("failed to open event WAL directory: " +
                             std::string(std::strerror(errno)));
  }
  try {
    fsync_fd(fd, "event WAL directory");
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
}

std::string event_type_name(tinyshell::v1::JobEventType t) {
  switch (t) {
  case tinyshell::v1::JOB_CREATED:
    return "job.created";
  case tinyshell::v1::JOB_VALIDATED:
    return "job.validated";
  case tinyshell::v1::JOB_SIGNED:
    return "job.signed";
  case tinyshell::v1::JOB_SCHEDULED:
    return "job.scheduled";
  case tinyshell::v1::JOB_ASSIGNED:
    return "job.assigned";
  case tinyshell::v1::JOB_DELIVERED:
    return "job.delivered";
  case tinyshell::v1::JOB_AGENT_ACCEPTED:
    return "job.agent_accepted";
  case tinyshell::v1::JOB_STARTED:
    return "job.started";
  case tinyshell::v1::JOB_STDOUT:
    return "job.stdout";
  case tinyshell::v1::JOB_STDERR:
    return "job.stderr";
  case tinyshell::v1::JOB_EXITED:
    return "job.exited";
  case tinyshell::v1::JOB_FAILED:
    return "job.failed";
  case tinyshell::v1::JOB_TIMED_OUT:
    return "job.timed_out";
  case tinyshell::v1::JOB_KILLED:
    return "job.killed";
  case tinyshell::v1::JOB_LOST:
    return "job.lost";
  case tinyshell::v1::JOB_REJECTED:
    return "job.rejected";
  case tinyshell::v1::AGENT_HEARTBEAT:
    return "agent.heartbeat";
  case tinyshell::v1::AUDIT_RECORDED:
    return "audit.recorded";
  default:
    return "unknown";
  }
}

bool needs_audit(tinyshell::v1::JobEventType t) {
  switch (t) {
  case tinyshell::v1::AUDIT_RECORDED:
  case tinyshell::v1::JOB_REJECTED:
  case tinyshell::v1::JOB_EXITED:
  case tinyshell::v1::JOB_FAILED:
  case tinyshell::v1::JOB_TIMED_OUT:
  case tinyshell::v1::JOB_KILLED:
  case tinyshell::v1::JOB_LOST:
    return true;
  default:
    return false;
  }
}

struct JobRow {
  std::string state;
  tinyshell::v1::SignedJobSpec signed_spec;
  std::uint64_t next_sequence{1};
  std::deque<tinyshell::v1::JobEvent> events;
  std::int64_t last_event_unix_ms{0};
};

struct AuditRow {
  std::string prev_hash;
  std::string event_hash;
};

class WalEventStore final : public EventStore {
public:
  explicit WalEventStore(std::filesystem::path dir)
      : dir_(std::move(dir)), wal_path_(dir_ / kWalName),
        max_retained_jobs_(static_cast<std::size_t>(
            env_u64("TSH_EVENT_STORE_MAX_JOBS", kDefaultMaxRetainedJobs))),
        compact_bytes_(
            env_u64("TSH_EVENT_STORE_COMPACT_BYTES", kDefaultCompactBytes)) {}

  void initialize() override {
    std::lock_guard<std::mutex> lk(mu_);
#if defined(SIGXFSZ)
    std::signal(SIGXFSZ, SIG_IGN);
#endif
    std::filesystem::create_directories(dir_);
    if (!std::filesystem::exists(wal_path_) ||
        std::filesystem::file_size(wal_path_) == 0) {
      const int fd =
          open(wal_path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
      if (fd < 0) {
        throw std::runtime_error("failed to create event WAL: " +
                                 std::string(std::strerror(errno)));
      }
      try {
        write_all(fd, kWalMagic, std::strlen(kWalMagic));
        fsync_fd(fd, "event WAL header");
      } catch (...) {
        close(fd);
        throw;
      }
      close(fd);
      return;
    }
    load_wal_locked();
  }

  void insert_job(const tinyshell::v1::SignedJobSpec &signed_spec,
                  const std::string &state) override {
    std::lock_guard<std::mutex> lk(mu_);
    const auto &id = signed_spec.spec().job_id();
    if (id.empty() || jobs_.count(id)) {
      return;
    }
    std::string serialized;
    if (!signed_spec.SerializeToString(&serialized)) {
      throw std::runtime_error("failed to serialize job for event WAL");
    }
    std::string payload;
    append_u32(payload, static_cast<std::uint32_t>(state.size()));
    payload.append(state);
    payload.append(serialized);
    append_record_locked(kJobRecord, payload);
    auto &row = jobs_[id];
    row.state = state;
    row.signed_spec = signed_spec;
    row.last_event_unix_ms = signed_spec.spec().created_at_unix_ms();
    prune_retention_locked();
  }

  void update_job_state(const std::string &job_id,
                        const std::string &state) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
      throw std::runtime_error("update_job_state: unknown job: " + job_id);
    }
    it->second.state = state;
  }

  void append_event(tinyshell::v1::JobEvent &event) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(event.job_id());
    if (it == jobs_.end()) {
      throw std::runtime_error("append_event: unknown job: " + event.job_id());
    }
    JobRow &row = it->second;
    const auto assigned_sequence = row.next_sequence;
    event.set_sequence(assigned_sequence);

    std::string serialized;
    if (!event.SerializeToString(&serialized)) {
      throw std::runtime_error("failed to serialize event for event WAL");
    }
    append_record_locked(kEventRecord, serialized);
    row.next_sequence = assigned_sequence + 1;
    if (event.state() != tinyshell::v1::JOB_STATE_UNSPECIFIED) {
      row.state = job_state_name(event.state());
    }
    row.last_event_unix_ms = event.timestamp_unix_ms();
    row.events.push_back(event);
    trim_events_locked(row);
    append_audit_locked(event);
    prune_retention_locked();
    maybe_compact_locked();
  }

  std::vector<tinyshell::v1::JobEvent>
  load_events_after(const std::string &job_id, std::uint64_t after,
                    std::size_t limit) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
      return {};
    }
    std::vector<tinyshell::v1::JobEvent> out;
    for (const auto &ev : it->second.events) {
      if (ev.sequence() <= after) {
        continue;
      }
      out.push_back(ev);
      if (out.size() >= limit) {
        break;
      }
    }
    return out;
  }

  std::string job_state(const std::string &job_id) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(job_id);
    return it == jobs_.end() ? "" : it->second.state;
  }

  void upsert_agent(const tinyshell::v1::AgentHello &) override {}
  void mark_agent_heartbeat(const std::string &) override {}

private:
  void load_wal_locked() {
    std::ifstream in(wal_path_, std::ios::binary);
    if (!in) {
      throw std::runtime_error("failed to open event WAL");
    }
    std::string magic(std::strlen(kWalMagic), '\0');
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (magic != kWalMagic) {
      throw std::runtime_error("event WAL has invalid header");
    }

    while (in) {
      char kind = 0;
      char len_bytes[4]{};
      in.read(&kind, 1);
      if (!in) {
        break;
      }
      in.read(len_bytes, 4);
      if (!in) {
        break;
      }
      const std::uint32_t len = read_u32(len_bytes);
      std::string payload(len, '\0');
      in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
      if (!in) {
        break;
      }
      apply_record_locked(kind, payload);
    }
  }

  void apply_record_locked(char kind, const std::string &payload) {
    if (kind == kJobRecord) {
      if (payload.size() < 4) {
        return;
      }
      const std::uint32_t state_len = read_u32(payload.data());
      if (payload.size() < 4U + state_len) {
        return;
      }
      const std::string state = payload.substr(4, state_len);
      const std::string spec_bytes = payload.substr(4 + state_len);
      tinyshell::v1::SignedJobSpec signed_spec;
      if (!signed_spec.ParseFromString(spec_bytes) ||
          signed_spec.spec().job_id().empty()) {
        return;
      }
      auto &row = jobs_[signed_spec.spec().job_id()];
      row.state = state;
      row.signed_spec = signed_spec;
      row.last_event_unix_ms = signed_spec.spec().created_at_unix_ms();
      return;
    }

    if (kind == kEventRecord) {
      tinyshell::v1::JobEvent event;
      if (!event.ParseFromString(payload) || event.job_id().empty()) {
        return;
      }
      auto &row = jobs_[event.job_id()];
      if (event.state() != tinyshell::v1::JOB_STATE_UNSPECIFIED) {
        row.state = job_state_name(event.state());
      }
      row.next_sequence = std::max(row.next_sequence, event.sequence() + 1);
      row.last_event_unix_ms = event.timestamp_unix_ms();
      row.events.push_back(event);
      trim_events_locked(row);
      append_audit_locked(event);
    }
  }

  void append_record_locked(char kind, const std::string &payload) {
    if (payload.size() > UINT32_MAX) {
      throw std::runtime_error("event WAL record too large");
    }
    const int fd = open(wal_path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0) {
      throw std::runtime_error("failed to open event WAL: " +
                               std::string(std::strerror(errno)));
    }
    try {
      write_all(fd, &kind, 1);
      std::string len;
      append_u32(len, static_cast<std::uint32_t>(payload.size()));
      write_all(fd, len.data(), len.size());
      write_all(fd, payload.data(), payload.size());
      fsync_fd(fd, "event WAL");
    } catch (...) {
      close(fd);
      throw;
    }
    close(fd);
  }

  void append_audit_locked(const tinyshell::v1::JobEvent &event) {
    if (!needs_audit(event.type())) {
      return;
    }
    std::string payload_json;
    google::protobuf::util::MessageToJsonString(event, &payload_json);
    const std::string prev =
        audit_log_.empty() ? "" : audit_log_.back().event_hash;
    AuditRow ar;
    ar.prev_hash = prev;
    ar.event_hash = sha256_hex(prev + event.event_id() + event.job_id() +
                               event_type_name(event.type()) + event.actor() +
                               event.agent_id() + payload_json);
    audit_log_.push_back(std::move(ar));
    while (audit_log_.size() > kMaxRetainedAuditRows) {
      audit_log_.pop_front();
    }
  }

  static bool terminal_state_name(const std::string &state) {
    return is_terminal_state(job_state_from_name(state));
  }

  void trim_events_locked(JobRow &row) {
    while (row.events.size() > kMaxRetainedEventsPerJob) {
      row.events.pop_front();
    }
  }

  void prune_retention_locked() {
    if (jobs_.size() <= max_retained_jobs_) {
      return;
    }
    std::vector<std::string> terminal_ids;
    terminal_ids.reserve(jobs_.size());
    for (const auto &[id, row] : jobs_) {
      if (terminal_state_name(row.state)) {
        terminal_ids.push_back(id);
      }
    }
    std::sort(terminal_ids.begin(), terminal_ids.end(),
              [this](const std::string &a, const std::string &b) {
                return jobs_[a].last_event_unix_ms <
                       jobs_[b].last_event_unix_ms;
              });
    for (const auto &id : terminal_ids) {
      if (jobs_.size() <= max_retained_jobs_) {
        break;
      }
      jobs_.erase(id);
      compact_needed_ = true;
    }
  }

  void maybe_compact_locked() {
    if (!compact_needed_ && compact_bytes_ > 0) {
      std::error_code ec;
      compact_needed_ =
          std::filesystem::exists(wal_path_, ec) &&
          std::filesystem::file_size(wal_path_, ec) > compact_bytes_;
    }
    if (compact_needed_) {
      compact_wal_locked();
      compact_needed_ = false;
    }
  }

  void append_job_record_to_fd_locked(int fd, const JobRow &row) {
    std::string serialized;
    if (!row.signed_spec.SerializeToString(&serialized)) {
      throw std::runtime_error("failed to serialize job during WAL compaction");
    }
    std::string payload;
    append_u32(payload, static_cast<std::uint32_t>(row.state.size()));
    payload.append(row.state);
    payload.append(serialized);
    write_all(fd, &kJobRecord, 1);
    std::string len;
    append_u32(len, static_cast<std::uint32_t>(payload.size()));
    write_all(fd, len.data(), len.size());
    write_all(fd, payload.data(), payload.size());
  }

  void append_event_record_to_fd_locked(int fd,
                                        const tinyshell::v1::JobEvent &event) {
    std::string serialized;
    if (!event.SerializeToString(&serialized)) {
      throw std::runtime_error(
          "failed to serialize event during WAL compaction");
    }
    write_all(fd, &kEventRecord, 1);
    std::string len;
    append_u32(len, static_cast<std::uint32_t>(serialized.size()));
    write_all(fd, len.data(), len.size());
    write_all(fd, serialized.data(), serialized.size());
  }

  void compact_wal_locked() {
    const auto tmp = wal_path_.string() + ".compact";
    const int fd =
        open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
      throw std::runtime_error("failed to create compacted event WAL: " +
                               std::string(std::strerror(errno)));
    }
    try {
      write_all(fd, kWalMagic, std::strlen(kWalMagic));
      for (const auto &[_, row] : jobs_) {
        if (row.signed_spec.spec().job_id().empty()) {
          continue;
        }
        append_job_record_to_fd_locked(fd, row);
        for (const auto &event : row.events) {
          append_event_record_to_fd_locked(fd, event);
        }
      }
      fsync_fd(fd, "compacted event WAL");
    } catch (...) {
      close(fd);
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      throw;
    }
    close(fd);
    std::filesystem::rename(tmp, wal_path_);
    fsync_directory(dir_);
  }

  std::filesystem::path dir_;
  std::filesystem::path wal_path_;
  std::mutex mu_;
  std::unordered_map<std::string, JobRow> jobs_;
  std::deque<AuditRow> audit_log_;
  std::size_t max_retained_jobs_;
  std::uint64_t compact_bytes_;
  bool compact_needed_ = false;
};

} // namespace

std::unique_ptr<EventStore> make_postgres_event_store(const std::string &dsn) {
  std::filesystem::path dir;
  if (dsn.rfind("file://", 0) == 0) {
    dir = dsn.substr(7);
  } else if (!dsn.empty()) {
    dir = dsn;
  } else {
    dir = default_store_dir();
  }
  return std::make_unique<WalEventStore>(std::move(dir));
}

} // namespace tsh::spine
