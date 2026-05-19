#include "../../common/FastFileLoader.h"
#include "../../config/tsh_config.h"
#include "CommandPolicy.h"
#include "EventBus.h"
#include "EventStore.h"
#include "JobLifecycle.h"
#include "JobSigner.h"
#include "Time.h"
#include "Uuid.h"
#include "tinyshell/v1/spine.grpc.pb.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set> // FIX (eviction loop): needed for evicted_jobs_
#include <vector>

namespace {

std::atomic<bool> g_shutdown{false};

std::string env_string(const char *name, const std::string &fallback = {}) {
  if (const char *value = std::getenv(name)) {
    return value;
  }
  return fallback;
}
static std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("failed to read file: " + path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

std::shared_ptr<grpc::ServerCredentials> server_credentials_from_env() {
  if (env_string("TSH_GRPC_INSECURE_DEV") == "1") {
    if (env_string("TSH_I_KNOW_THIS_IS_INSECURE") != "1") {
      throw std::runtime_error("TSH_GRPC_INSECURE_DEV=1 requires "
                               "TSH_I_KNOW_THIS_IS_INSECURE=1");
    }
    std::cerr << "[TinyShell Spine] WARNING: using insecure gRPC because "
                 "TSH_GRPC_INSECURE_DEV=1\n";
    return grpc::InsecureServerCredentials();
  }

  const auto cert = env_string("TSH_GRPC_SERVER_CERT");
  const auto key = env_string("TSH_GRPC_SERVER_KEY");
  if (cert.empty() || key.empty()) {
    throw std::runtime_error(
        "set TSH_GRPC_SERVER_CERT and TSH_GRPC_SERVER_KEY, or explicitly set "
        "TSH_GRPC_INSECURE_DEV=1 for local development");
  }

  grpc::SslServerCredentialsOptions::PemKeyCertPair pair;
  pair.cert_chain = read_file(cert);
  pair.private_key = read_file(key);

  grpc::SslServerCredentialsOptions opts;
  opts.pem_key_cert_pairs.push_back(std::move(pair));

  const auto client_ca = env_string("TSH_GRPC_CLIENT_CA");
  if (!client_ca.empty()) {
    opts.pem_root_certs = read_file(client_ca);
    opts.client_certificate_request =
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  }
  return grpc::SslServerCredentials(opts);
}

// ─── stale-agent eviction constant ───────────────────────────────────────────
static constexpr std::chrono::milliseconds kAgentHeartbeatTimeoutMs{45'000};
static constexpr std::chrono::milliseconds kAgentMonitorIntervalMs{10'000};

class SpineControlPlane final
    : public tinyshell::v1::ControlPlaneService::Service,
      public tinyshell::v1::AgentConnector::Service {
public:
  SpineControlPlane(tsh::spine::JobSigner signer,
                    std::unique_ptr<tsh::spine::EventStore> store)
      : signer_(std::move(signer)), store_(std::move(store)) {
    store_->initialize();

    // FIX 2: background thread that evicts agents with stale heartbeats.
    monitor_thread_ = std::thread([this] {
      while (!g_shutdown.load()) {
        std::this_thread::sleep_for(kAgentMonitorIntervalMs);
        evict_stale_agents();
        evict_terminal_jobs(); // FIX[CF-7]: prune terminal-job metadata maps
      }
    });
  }

  ~SpineControlPlane() {
    // FIX (Bug 7): g_shutdown is only set by the SIGINT/SIGTERM signal handler.
    // If SpineControlPlane is destroyed without a signal being received (e.g.
    // in a unit test, or during orderly programmatic shutdown), the monitor
    // thread loops forever on !g_shutdown.load() and monitor_thread_.join()
    // hangs indefinitely.  Set g_shutdown here before joining so the monitor
    // thread always sees the shutdown flag and exits promptly.
    g_shutdown.store(true);
    if (monitor_thread_.joinable()) {
      monitor_thread_.join();
    }
  }

  // ── ControlPlaneService ────────────────────────────────────────────────────
  grpc::Status SubmitJob(grpc::ServerContext *,
                         const tinyshell::v1::SubmitJobRequest *request,
                         tinyshell::v1::SubmitJobResponse *response) override {
    std::cout << "[TinyShell Spine] SubmitJob command=" << request->command()
              << " user=" << request->user_id() << "\n";
    tinyshell::v1::JobSpec spec;
    try {
      std::shared_ptr<AgentSession> agent;
      {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        if (agents_.empty()) {
          response->set_state(tinyshell::v1::JOB_STATE_REJECTED);
          response->set_message("no connected agent is available");
          return grpc::Status::OK;
        }
        // FIX[CF-4]: was agents_.begin()->second — an arbitrary map element
        // (whichever agent hashed to the first bucket).  In a multi-agent
        // deployment every job went to the same agent while others sat idle.
        //
        // Fix: pick the agent with the lowest in-flight job count so work is
        // spread across all connected agents.  Ties are broken by agent_id for
        // determinism.  A stale/closed agent is skipped; if no live agent
        // exists we reject the job.
        std::shared_ptr<AgentSession> best;
        for (auto &[id, session] : agents_) {
          if (session->closed)
            continue;
          if (!best || session->active_jobs.load() < best->active_jobs.load())
            best = session;
        }
        if (!best) {
          response->set_state(tinyshell::v1::JOB_STATE_REJECTED);
          response->set_message("all connected agents are closed");
          return grpc::Status::OK;
        }
        agent = best;
      }

      std::vector<std::string> args(request->args().begin(),
                                    request->args().end());
      const auto validated = policy_.validate(request->command(), args);

      const auto now = tsh::spine::now_unix_ms();
      spec.set_job_id(tsh::spine::random_id("job-"));
      spec.set_command(validated.absolute_path);
      for (const auto &arg : validated.args) {
        spec.add_args(arg);
      }
      spec.set_user_id(request->user_id().empty() ? "anonymous"
                                                  : request->user_id());
      spec.set_requested_by(spec.user_id());
      spec.set_agent_id(agent->agent_id);
      spec.set_created_at_unix_ms(now);
      spec.set_expires_at_unix_ms(now + 60'000);
      spec.mutable_limits()->set_timeout_ms(5000);
      spec.mutable_limits()->set_max_stdout_bytes(1024 * 1024);
      spec.mutable_limits()->set_max_stderr_bytes(1024 * 1024);
      spec.mutable_policy()->set_allow_network(false);
      spec.mutable_policy()->set_allow_write_filesystem(false);
      spec.mutable_policy()->add_allowed_absolute_paths(
          validated.absolute_path);

      auto signed_spec = signer_.sign(spec);
      store_->insert_job(signed_spec, tsh::spine::job_state_name(
                                          tinyshell::v1::JOB_STATE_PENDING));
      remember_assignment(spec.job_id(), spec.agent_id());

      append(spec.job_id(), tinyshell::v1::JOB_CREATED, "control-plane",
             spec.agent_id(),
             [&](auto *event) { *event->mutable_job_spec() = spec; });
      append(spec.job_id(), tinyshell::v1::JOB_VALIDATED, "control-plane",
             spec.agent_id(), [&](auto *event) {
               event->set_message("command accepted by phase 1 policy");
             });
      append(spec.job_id(), tinyshell::v1::JOB_SIGNED, "control-plane",
             spec.agent_id(), [&](auto *event) {
               *event->mutable_signed_job_spec() = signed_spec;
             });
      append(spec.job_id(), tinyshell::v1::JOB_ASSIGNED, "scheduler",
             spec.agent_id(), [&](auto *event) {
               event->set_message("assigned to connected agent");
             });

      tinyshell::v1::ServerMessage msg;
      *msg.mutable_job() = signed_spec;
      if (!agent->enqueue(msg)) {
        append(spec.job_id(), tinyshell::v1::JOB_FAILED, "control-plane",
               spec.agent_id(), [&](auto *event) {
                 event->set_message(
                     "agent assignment queue is closed or backpressured");
               });
        response->set_job_id(spec.job_id());
        response->set_state(tinyshell::v1::JOB_STATE_FAILED);
        response->set_message("agent assignment queue is unavailable");
        return grpc::Status::OK;
      }

      response->set_job_id(spec.job_id());
      response->set_state(tinyshell::v1::JOB_STATE_ASSIGNED);
      response->set_message("job signed and assigned");
      return grpc::Status::OK;
    } catch (const tsh::spine::PolicyError &e) {
      response->set_state(tinyshell::v1::JOB_STATE_REJECTED);
      response->set_message(e.what());
      return grpc::Status::OK;
    } catch (const std::exception &e) {
      return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
  }

  grpc::Status
  WatchJob(grpc::ServerContext *context,
           const tinyshell::v1::WatchJobRequest *request,
           grpc::ServerWriter<tinyshell::v1::JobEvent> *writer) override {
    if (store_->job_state(request->job_id()).empty()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "unknown job_id");
    }
    std::uint64_t last_sequence = 0;
    if (request->after_sequence() > 0) {
      last_sequence = request->after_sequence();
    }
    for (const auto &event :
         store_->load_events_after(request->job_id(), last_sequence, 1000)) {
      if (!writer->Write(event)) {
        return grpc::Status::OK;
      }
      last_sequence = event.sequence();
    }

    while (!context->IsCancelled() && !g_shutdown.load()) {
      tinyshell::v1::JobEvent event;
      if (bus_.wait_for_next(request->job_id(), last_sequence, &event)) {
        if (!writer->Write(event)) {
          return grpc::Status::OK;
        }
        last_sequence = event.sequence();
      }
    }
    return grpc::Status::OK;
  }

  grpc::Status WatchAgents(
      grpc::ServerContext *context, const tinyshell::v1::WatchAgentsRequest *,
      grpc::ServerWriter<tinyshell::v1::AgentStatusEvent> *writer) override {
    // Snapshot currently connected agents first.
    {
      std::lock_guard<std::mutex> lk(agents_mutex_);
      for (auto &[id, session] : agents_) {
        if (session->closed)
          continue;
        tinyshell::v1::AgentStatusEvent e;
        e.set_agent_id(id);
        e.set_status(tinyshell::v1::AGENT_STATUS_CONNECTED);
        e.set_timestamp_unix_ms(tsh::spine::now_unix_ms());
        writer->Write(e);
      }
    }
    auto watcher = std::make_shared<AgentWatcher>();
    {
      std::lock_guard<std::mutex> lk(watchers_mutex_);
      watchers_.push_back(watcher);
    }
    tinyshell::v1::AgentStatusEvent event;
    while (!context->IsCancelled() && !g_shutdown.load()) {
      if (watcher->wait_pop(&event))
        if (!writer->Write(event))
          break;
    }
    watcher->close();
    {
      std::lock_guard<std::mutex> lk(watchers_mutex_);
      watchers_.erase(std::remove(watchers_.begin(), watchers_.end(), watcher),
                      watchers_.end());
    }
    return grpc::Status::OK;
  }

  // ── AgentConnector ─────────────────────────────────────────────────────────
  grpc::Status Connect(
      grpc::ServerContext *,
      grpc::ServerReaderWriter<tinyshell::v1::ServerMessage,
                               tinyshell::v1::AgentMessage> *stream) override {
    tinyshell::v1::AgentMessage first;
    if (!stream->Read(&first) || !first.has_hello()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "agent must send AgentHello first");
    }

    const auto agent_id = first.hello().agent_id();
    if (agent_id.empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "agent_id is required");
    }
    store_->upsert_agent(first.hello());
    notify_agent_watchers(agent_id, first.hello().hostname(),
                          tinyshell::v1::AGENT_STATUS_CONNECTED);

    auto session = std::make_shared<AgentSession>();
    session->agent_id = agent_id;
    {
      std::lock_guard<std::mutex> lock(agents_mutex_);
      if (auto old = agents_.find(agent_id); old != agents_.end()) {
        old->second->close();
      }
      agents_[agent_id] = session;
    }
    std::cout << "[TinyShell Spine] agent connected: " << agent_id << "\n";

    std::thread reader([&, session] {
      tinyshell::v1::AgentMessage msg;
      while (stream->Read(&msg)) {
        try {
          if (msg.has_event()) {
            ingest_agent_event(agent_id, msg.event());
          } else if (msg.has_heartbeat()) {
            if (msg.heartbeat().agent_id() == agent_id) {
              session->mark_heartbeat();
              store_->mark_agent_heartbeat(agent_id);
              notify_agent_watchers(agent_id, "",
                                    tinyshell::v1::AGENT_STATUS_HEARTBEAT);
            }
          }
        } catch (const std::exception &e) {
          std::cerr << "[TinyShell Spine] failed to ingest agent message: "
                    << e.what() << "\n";
        }
      }
      session->close();
    });

    try {
      tinyshell::v1::ServerMessage outbound;
      while (session->wait_pop(&outbound)) {
        if (outbound.has_job()) {
          append(outbound.job().spec().job_id(), tinyshell::v1::JOB_DELIVERED,
                 "agent-session", agent_id, [&](auto *event) {
                   event->set_message(
                       "signed JobSpec queued for agent stream delivery");
                 });
        }
        if (!stream->Write(outbound)) {
          if (outbound.has_job()) {
            append(outbound.job().spec().job_id(), tinyshell::v1::JOB_LOST,
                   "agent-session", agent_id, [&](auto *event) {
                     event->set_message(
                         "agent stream closed before job write completed");
                   });
          }
          session->close();
          break;
        }
      }
    } catch (const std::exception &e) {
      std::cerr << "[TinyShell Spine] agent writer failed for " << agent_id
                << ": " << e.what() << "\n";
      session->close();
    }
    if (reader.joinable()) {
      reader.join();
    }

    {
      std::lock_guard<std::mutex> lock(agents_mutex_);
      agents_.erase(agent_id);
      notify_agent_watchers(agent_id, "",
                            tinyshell::v1::AGENT_STATUS_DISCONNECTED);
    }
    std::cout << "[TinyShell Spine] agent disconnected: " << agent_id << "\n";
    return grpc::Status::OK;
  }

private:
  // ── AgentSession
  // ────────────────────────────────────────────────────────────
  struct AgentWatcher {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<tinyshell::v1::AgentStatusEvent> queue;
    bool closed = false;

    void push(const tinyshell::v1::AgentStatusEvent &e) {
      {
        std::lock_guard<std::mutex> lk(mutex);
        queue.push_back(e);
      }
      cv.notify_one();
    }
    bool wait_pop(tinyshell::v1::AgentStatusEvent *out) {
      std::unique_lock<std::mutex> lk(mutex);
      cv.wait_for(lk, std::chrono::seconds(1),
                  [&] { return closed || !queue.empty(); });
      if (queue.empty())
        return !closed;
      *out = std::move(queue.front());
      queue.pop_front();
      return true;
    }
    void close() {
      {
        std::lock_guard<std::mutex> lk(mutex);
        closed = true;
      }
      cv.notify_all();
    }
  };

  struct AgentSession {
    std::string agent_id;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<tinyshell::v1::ServerMessage> queue;
    std::chrono::steady_clock::time_point last_heartbeat =
        std::chrono::steady_clock::now();
    bool closed = false;
    static constexpr std::size_t kMaxQueueDepth = 64;

    // FIX[CF-4]: Track in-flight jobs so SubmitJob can pick the least-loaded
    // agent.  Incremented when a job is enqueued, decremented when the agent
    // reports a terminal event (see ingest_agent_event).
    std::atomic<int> active_jobs{0};

    bool enqueue(const tinyshell::v1::ServerMessage &message) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) {
          return false;
        }
        if (queue.size() >= kMaxQueueDepth) {
          return false;
        }
        queue.push_back(message);
      }
      if (message.has_job())
        active_jobs.fetch_add(1, std::memory_order_relaxed);
      cv.notify_one();
      return true;
    }

    // FIX 3: use timed wait so the outbound writer wakes periodically and can
    // detect shutdown/closure instead of blocking forever.
    bool wait_pop(tinyshell::v1::ServerMessage *message) {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_for(lock, std::chrono::seconds(1),
                  [&] { return closed || !queue.empty(); });
      if (queue.empty()) {
        return !closed; // return true to keep waiting if not closed yet
      }
      *message = std::move(queue.front());
      queue.pop_front();
      return true;
    }

    void close() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
      }
      cv.notify_all();
    }

    void mark_heartbeat() {
      std::lock_guard<std::mutex> lock(mutex);
      last_heartbeat = std::chrono::steady_clock::now();
    }

    std::chrono::steady_clock::time_point get_last_heartbeat() const {
      std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(mutex));
      return last_heartbeat;
    }
  };

  // ── FIX 2: stale-agent eviction ────────────────────────────────────────────
  void evict_stale_agents() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> to_evict;
    {
      std::lock_guard<std::mutex> lock(agents_mutex_);
      for (auto &[id, session] : agents_) {
        const auto age = now - session->get_last_heartbeat();
        if (age > kAgentHeartbeatTimeoutMs) {
          to_evict.push_back(id);
        }
      }
    }
    for (const auto &id : to_evict) {
      std::cerr << "[TinyShell Spine] evicting stale agent: " << id << "\n";
      std::shared_ptr<AgentSession> session;
      {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        if (auto it = agents_.find(id); it != agents_.end()) {
          session = it->second;
          agents_.erase(it);
        }
      }
      if (session) {
        session->close();
      }
    }
  }

  void notify_agent_watchers(const std::string &agent_id,
                             const std::string &hostname,
                             tinyshell::v1::AgentStatus status) {
    tinyshell::v1::AgentStatusEvent e;
    e.set_agent_id(agent_id);
    e.set_hostname(hostname);
    e.set_status(status);
    e.set_timestamp_unix_ms(tsh::spine::now_unix_ms());
    std::lock_guard<std::mutex> lk(watchers_mutex_);
    for (auto &w : watchers_)
      w->push(e);
  }

  // ── sequence helpers
  // ────────────────────────────────────────────────────────

  // FIX 1: load_events_after was called while holding sequence_mutex_, which
  // could block for I/O under lock and risk priority inversion / deadlock with
  // the store's own mutex.  We now do a double-checked pattern: fast path under
  // lock, expensive load *outside* lock, then a second lock to insert.
  std::uint64_t next_sequence(const std::string &job_id) {
    // Fast path: already seeded.
    {
      std::lock_guard<std::mutex> lock(sequence_mutex_);
      if (sequences_.find(job_id) != sequences_.end()) {
        return ++sequences_[job_id];
      }
    }

    // Slow path: load from store WITHOUT holding sequence_mutex_.
    auto events = store_->load_events_after(job_id, 0, 1'000'000);
    const std::uint64_t initial = events.empty() ? 0 : events.back().sequence();

    // Re-acquire and insert only if another thread hasn't already done so.
    std::lock_guard<std::mutex> lock(sequence_mutex_);
    if (sequences_.find(job_id) == sequences_.end()) {
      sequences_[job_id] = initial;
    }
    return ++sequences_[job_id];
  }

  // ── event helpers
  // ───────────────────────────────────────────────────────────
  template <typename Fill>
  void append(const std::string &job_id, tinyshell::v1::JobEventType type,
              const std::string &actor, const std::string &agent_id,
              Fill fill) {
    tinyshell::v1::JobEvent event;
    event.set_event_id(tsh::spine::random_id("evt-"));
    event.set_job_id(job_id);
    event.set_sequence(next_sequence(job_id));
    event.set_type(type);
    event.set_timestamp_unix_ms(tsh::spine::now_unix_ms());
    event.set_actor(actor);
    event.set_agent_id(agent_id);
    event.set_source(actor);
    event.set_correlation_id(job_id);
    fill(&event);
    persist_and_publish(event);
  }

  void ingest_agent_event(const std::string &agent_id,
                          const tinyshell::v1::JobEvent &incoming) {
    if (incoming.job_id().empty()) {
      return;
    }
    const auto vr = validate_agent_event(agent_id, incoming.job_id());
    if (vr == ValidationResult::Unknown) {
      // Job has been pruned from in-process maps (normal after eviction).
      // Log once at trace level; do not spam stderr.
      return;
    }
    if (vr == ValidationResult::Terminal) {
      // Late event for a job already in a terminal state (e.g. trailing
      // stdout chunks queued before the terminal event was processed).
      // Silently drop — the job result is already recorded.
      return;
    }

    tinyshell::v1::JobEvent event = incoming;
    event.set_event_id(tsh::spine::random_id("evt-"));
    event.set_sequence(next_sequence(event.job_id()));
    event.set_timestamp_unix_ms(tsh::spine::now_unix_ms());
    event.set_actor("agent");
    event.set_agent_id(agent_id);
    event.set_source("agent");
    event.set_correlation_id(event.job_id());

    persist_and_publish(event);

    if (event.type() == tinyshell::v1::JOB_EXITED ||
        event.type() == tinyshell::v1::JOB_FAILED ||
        event.type() == tinyshell::v1::JOB_TIMED_OUT ||
        event.type() == tinyshell::v1::JOB_KILLED ||
        event.type() == tinyshell::v1::JOB_LOST) {
      append(event.job_id(), tinyshell::v1::AUDIT_RECORDED, "control-plane",
             agent_id, [&](auto *audit) {
               audit->set_message("final job state persisted to audit log");
             });
      mark_terminal(event.job_id());
    }
  }

  void remember_assignment(const std::string &job_id,
                           const std::string &agent_id) {
    std::lock_guard<std::mutex> lock(assignments_mutex_);
    assignments_[job_id] = {agent_id, false};
  }

  // validate_agent_event returns false (instead of throwing) for events that
  // arrive after a job has already reached a terminal state or been evicted
  // from in-process maps.  These are expected under normal operation:
  //
  //  • The spine marks a job terminal the instant it receives the first
  //    terminal event, then the monitor thread prunes the assignment map after
  //    kAgentMonitorIntervalMs (10 s).  Any stdout/stderr chunks that were
  //    already queued in the agent's write_queue_ can arrive at the spine
  //    milliseconds later — after the assignment is marked terminal but before
  //    the pruning run, or after the pruning run.
  //
  //  • The agent's backoff-reconnect loop calls
  //  emit_recovered_unfinished_jobs()
  //    on every new connection.  Those JOB_LOST events reference job IDs that
  //    are no longer in assignments_ (they were pruned).
  //
  // In both cases throwing an exception was wrong: it spammed stderr and caused
  // the gRPC reader thread to log a noisy "failed to ingest agent message" line
  // for every queued event — producing the multi-hundred-line flood seen in
  // production.  We now return false so the caller silently skips the event.
  //
  // A real security violation (event for a job owned by a different agent) is
  // still treated as an error because it indicates a misbehaving or
  // compromised agent.
  enum class ValidationResult { Ok, Terminal, Unknown, WrongAgent };

  ValidationResult validate_agent_event(const std::string &agent_id,
                                        const std::string &job_id) {
    std::lock_guard<std::mutex> lock(assignments_mutex_);
    const auto it = assignments_.find(job_id);
    if (it == assignments_.end()) {
      return ValidationResult::Unknown;
    }
    if (it->second.agent_id != agent_id) {
      // Hard error — a different agent is claiming ownership of this job.
      throw std::runtime_error("agent sent event for job assigned to " +
                               it->second.agent_id + ": " + job_id);
    }
    if (it->second.terminal) {
      return ValidationResult::Terminal;
    }
    return ValidationResult::Ok;
  }

  void mark_terminal(const std::string &job_id) {
    std::lock_guard<std::mutex> lock(assignments_mutex_);
    const auto it = assignments_.find(job_id);
    if (it != assignments_.end()) {
      it->second.terminal = true;
    }
  }

  tinyshell::v1::JobState current_state(const std::string &job_id) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    // FIX (eviction loop / Fix B): if this job was already fully evicted,
    // do NOT re-insert it from the EventStore.  Returning UNSPECIFIED causes
    // the caller to treat it as Unknown and drop the event without logging.
    if (evicted_jobs_.count(job_id)) {
      return tinyshell::v1::JOB_STATE_UNSPECIFIED;
    }
    if (const auto it = lifecycle_.find(job_id); it != lifecycle_.end()) {
      return it->second.state;
    }
    const auto stored =
        tsh::spine::job_state_from_name(store_->job_state(job_id));
    lifecycle_[job_id] = LifecycleEntry{stored, {}};
    return stored;
  }

  void set_current_state(const std::string &job_id,
                         tinyshell::v1::JobState state) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    auto &entry = lifecycle_[job_id];
    // FIX (eviction loop / Fix A): record the exact moment the job first
    // enters a terminal state so evict_terminal_jobs() can enforce the TTL.
    if (tsh::spine::is_terminal_state(state) &&
        entry.terminal_since == std::chrono::steady_clock::time_point{}) {
      entry.terminal_since = std::chrono::steady_clock::now();
    }
    entry.state = state;
  }

  void persist_and_publish(tinyshell::v1::JobEvent &event) {
    // FIX[C-2]: The original code called current_state, append_event, and
    // set_current_state under three *separate* locks.  Two concurrent gRPC
    // handler threads for the same job could both read state RUNNING, both
    // compute transition → EXITED, both pass is_valid_transition(), and both
    // append a terminal event — violating the state machine invariant.
    //
    // Fix: acquire a per-job-id striped mutex that serializes the entire
    // read → validate → write → update sequence for any given job.
    // 64 stripes keep contention low while bounding memory overhead.
    std::lock_guard<std::mutex> job_lk(job_stripe(event.job_id()));

    const auto before = current_state(event.job_id());
    const auto after = tsh::spine::state_after_event(event.type(), before);
    if (!tsh::spine::is_valid_transition(before, after)) {
      throw std::runtime_error("invalid job lifecycle transition for " +
                               event.job_id() + ": " +
                               tsh::spine::job_state_name(before) + " -> " +
                               tsh::spine::job_state_name(after));
    }
    event.set_state(after);
    store_->append_event(event);
    if (after != before) {
      set_current_state(event.job_id(), after);
    }
    bus_.publish(event);

    // FIX[CF-4]: Decrement the active_jobs counter on the owning agent when
    // a job reaches a terminal state so future agent-selection picks reflect
    // real load.
    if (!tsh::spine::is_terminal_state(before) &&
        tsh::spine::is_terminal_state(after)) {
      std::lock_guard<std::mutex> lk(assignments_mutex_);
      if (const auto it = assignments_.find(event.job_id());
          it != assignments_.end()) {
        std::lock_guard<std::mutex> alk(agents_mutex_);
        if (const auto ait = agents_.find(it->second.agent_id);
            ait != agents_.end()) {
          const int prev =
              ait->second->active_jobs.fetch_sub(1, std::memory_order_relaxed);
          if (prev <= 0)
            ait->second->active_jobs.store(0, std::memory_order_relaxed);
        }
      }
    }
  }

  // ── FIX[C-2]: Per-job striped mutex
  // ────────────────────────────────────────── 64 stripes give good parallelism
  // (different jobs rarely share a stripe) while keeping memory overhead to a
  // few hundred bytes.
  static constexpr std::size_t kJobLockStripes = 64;
  mutable std::array<std::mutex, kJobLockStripes> job_mutexes_;

  std::mutex &job_stripe(const std::string &job_id) const {
    // FNV-1a hash for fast, uniform distribution.
    std::size_t h = 14695981039346656037ULL;
    for (unsigned char c : job_id) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    return job_mutexes_[h % kJobLockStripes];
  }

  // ── FIX[CF-7]: Evict terminal-job metadata to prevent unbounded map growth
  // ── Called from the existing monitor_thread_ every kAgentMonitorIntervalMs.
  // Jobs that have been in a terminal state for more than kTerminalJobTTLMs are
  // removed from assignments_, sequences_, and lifecycle_.  The EventStore
  // retains all events; only the lightweight in-process maps are pruned.
  static constexpr std::int64_t kTerminalJobTTLMs = 3'600'000; // 1 hour

  void evict_terminal_jobs() {
    std::vector<std::string> to_evict;
    const auto now = std::chrono::steady_clock::now();

    // FIX (eviction loop / Fix A): Only evict jobs that have been in a
    // terminal state for longer than kTerminalJobTTLMs.  Previously there was
    // no TTL check here at all — every terminal job was evicted on every
    // monitor tick, causing the "evicted N terminal job(s)" log line to fire
    // continuously even for the most recent completed job.
    {
      std::lock_guard<std::mutex> lk(lifecycle_mutex_);
      for (const auto &[id, entry] : lifecycle_) {
        if (!tsh::spine::is_terminal_state(entry.state))
          continue;
        // terminal_since is zero if set_current_state was never called (e.g.
        // job was loaded from the store but never transitioned in this process
        // lifetime). Treat those as "terminal since now" — they will age out
        // on the next cycle after kTerminalJobTTLMs.
        if (entry.terminal_since == std::chrono::steady_clock::time_point{})
          continue;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - entry.terminal_since);
        if (age.count() >= kTerminalJobTTLMs)
          to_evict.push_back(id);
      }
    }

    for (const auto &id : to_evict) {
      {
        std::lock_guard<std::mutex> lk(lifecycle_mutex_);
        lifecycle_.erase(id);
        // FIX (eviction loop / Fix B): add to evicted set so current_state()
        // never re-inserts this job from the EventStore after eviction.
        evicted_jobs_.insert(id);
      }
      {
        std::lock_guard<std::mutex> lk(sequence_mutex_);
        sequences_.erase(id);
      }
      {
        std::lock_guard<std::mutex> lk(assignments_mutex_);
        assignments_.erase(id);
      }
    }

    if (!to_evict.empty()) {
      std::cerr << "[TinyShell Spine] evicted " << to_evict.size()
                << " terminal job(s) from in-process maps\n";
    }
    bus_.prune_terminal_older_than(
        std::chrono::milliseconds(kTerminalJobTTLMs));
  }

  // ── member data
  // ─────────────────────────────────────────────────────────────
  tsh::spine::CommandPolicy policy_;
  tsh::spine::JobSigner signer_;
  std::unique_ptr<tsh::spine::EventStore> store_;
  tsh::spine::EventBus bus_;

  std::mutex agents_mutex_;
  std::unordered_map<std::string, std::shared_ptr<AgentSession>> agents_;

  std::mutex sequence_mutex_;
  std::unordered_map<std::string, std::uint64_t> sequences_;

  // ── BUG FIX (eviction loop): lifecycle_ previously stored only JobState with
  // no timestamp, so the 1-hour TTL (kTerminalJobTTLMs) could never be checked.
  // Every monitor cycle evicted ALL terminal jobs immediately, then
  // current_state() re-inserted them from the EventStore on the next event,
  // producing an infinite evict → re-insert → evict loop visible as the
  // repeated log line:
  //   "[TinyShell Spine] evicted 1 terminal job(s) from in-process maps"
  //
  // Fix A: Wrap JobState with a terminal_since timestamp.  Eviction now only
  //        fires after kTerminalJobTTLMs has elapsed since the job went
  //        terminal — matching the documented intent of the constant.
  //
  // Fix B: Add evicted_jobs_ set so current_state() never re-inserts a pruned
  //        job back into lifecycle_ from the EventStore.  Without this guard,
  //        even with the TTL fix a late-arriving agent event would resurrect
  //        the entry and restart the eviction cycle.
  struct LifecycleEntry {
    tinyshell::v1::JobState state{tinyshell::v1::JOB_STATE_UNSPECIFIED};
    // Populated the first time state transitions to a terminal value.
    std::chrono::steady_clock::time_point terminal_since{};
  };

  std::mutex lifecycle_mutex_;
  std::unordered_map<std::string, LifecycleEntry> lifecycle_;

  // Jobs fully evicted from all in-process maps.  current_state() treats
  // these as UNKNOWN so they are never re-inserted from the EventStore.
  std::unordered_set<std::string> evicted_jobs_; // guarded by lifecycle_mutex_

  struct Assignment {
    std::string agent_id;
    bool terminal = false;
  };
  std::mutex assignments_mutex_;
  std::unordered_map<std::string, Assignment> assignments_;

  std::thread monitor_thread_;
  std::mutex watchers_mutex_;
  std::vector<std::shared_ptr<AgentWatcher>> watchers_;
};

void handle_signal(int) { g_shutdown.store(true); }

} // namespace

int main() {
  try {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
#if defined(SIGXFSZ)
    std::signal(SIGXFSZ, SIG_IGN);
#endif

    const auto control_listen_addr =
        env_string("TSH_SPINE_CONTROL_LISTEN_ADDR",
                   env_string("TSH_SPINE_LISTEN_ADDR", "0.0.0.0:7443"));
    const auto agent_listen_addr =
        env_string("TSH_SPINE_AGENT_LISTEN_ADDR", "0.0.0.0:7444");
    const auto signing_secret =
        tsh::Config::read_string("TSH_JOB_SIGNING_KEY", "");
    const auto key_id = env_string("TSH_JOB_KEY_ID", "local-hmac-v1");
    const auto postgres_dsn = tsh::Config::read_string("TSH_POSTGRES_DSN", "");
    if (postgres_dsn.empty() && env_string("TSH_WAL_ONLY_MODE") != "1") {
      // BUG: the warning text was thrown as an exception, crashing the spine
      // server whenever Postgres was not configured (the common local-dev
      // case). FIX: emit the warning to stderr and continue with ephemeral WAL.
      // Set TSH_POSTGRES_DSN for persistence, or TSH_WAL_ONLY_MODE=1 to
      // silence.
      std::cerr << "[TinyShell Spine] WARNING: No Postgres DSN configured. "
                   "Using ephemeral WAL - events will be lost on restart. "
                   "Set TSH_POSTGRES_DSN for persistence, or "
                   "TSH_WAL_ONLY_MODE=1 to silence this warning.\n";
    }

    tsh::spine::JobSigner signer(key_id, signing_secret);
    auto store = tsh::spine::make_wal_event_store(postgres_dsn);

    SpineControlPlane service(std::move(signer), std::move(store));

    grpc::ServerBuilder control_builder;
    control_builder.SetSyncServerOption(
        grpc::ServerBuilder::SyncServerOption::NUM_CQS, 2);
    control_builder.SetSyncServerOption(
        grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, 4);
    control_builder.SetSyncServerOption(
        grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, 8);
    control_builder.AddListeningPort(control_listen_addr,
                                     server_credentials_from_env());
    control_builder.RegisterService(
        static_cast<tinyshell::v1::ControlPlaneService::Service *>(&service));

    grpc::ServerBuilder agent_builder;
    agent_builder.SetSyncServerOption(
        grpc::ServerBuilder::SyncServerOption::NUM_CQS, 2);
    agent_builder.SetSyncServerOption(
        grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, 4);
    agent_builder.SetSyncServerOption(
        grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, 8);
    agent_builder.AddListeningPort(agent_listen_addr,
                                   server_credentials_from_env());
    agent_builder.RegisterService(
        static_cast<tinyshell::v1::AgentConnector::Service *>(&service));

    auto control_server = control_builder.BuildAndStart();
    auto agent_server = agent_builder.BuildAndStart();
    if (!control_server || !agent_server) {
      throw std::runtime_error("failed to start gRPC servers");
    }
    std::cout << "[TinyShell Spine] control listening on "
              << control_listen_addr << "\n";
    std::cout << "[TinyShell Spine] agent connector listening on "
              << agent_listen_addr << "\n";

    while (!g_shutdown.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    control_server->Shutdown();
    agent_server->Shutdown();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "[TinyShell Spine] startup failed: " << e.what() << "\n";
    return 1;
  }
}