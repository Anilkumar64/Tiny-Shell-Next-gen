// SpineAgent.cpp — fully fixed
//
// Fixes applied:
//
//  BUG #1  — Detached threads replaced with owned std::vector<std::thread>.
//             Workers are joined after wait_for_jobs() before the gRPC stream
//             is torn down, eliminating use-after-free and shutdown races.
//
//  BUG #2  — Thread lambda wraps execute_job() in catch(...) so
//             decrement_jobs() is unconditionally called even for non-
//             std::exception throwables.
//
//  BUG #3  — Terminal-type selection switched from result.reason string
//             comparison to result.failure (ExecutionFailure enum), using a
//             switch so the compiler warns on unhandled enum values.
//
//  BUG #4  — Both std::filesystem::canonical() calls now use the
//             std::error_code overload.  No uncaught filesystem_error if the
//             binary or an allowlist path disappears between check and exec.
//
//  BUG #5  — Root-execution check moved to main() before AgentRuntime is
//             constructed.  The process now fails at startup rather than
//             per-job after journalling and emitting JOB_STARTED, which
//             previously left a permanently stuck .inflight entry.
//
//  BUG #6  — Dead increment_jobs() method removed.  The read loop was already
//             incrementing job_count_ inline under the lock; having an unused
//             helper with the same name was a maintenance hazard.
//
//  BUG #7  — Thread construction is now inside a try/catch.  If the OS
//             refuses to create the thread (e.g. EAGAIN), the already-
//             incremented job_count_ is decremented before re-throwing so
//             wait_for_jobs() is never permanently blocked.

#include "JobSigner.h"
#include "SandboxExecutor.h"
#include "Time.h"
#include "Uuid.h"
#include "tinyshell/v1/spine.grpc.pb.h"

#include "../../config/tsh_config.h"
#include <grpcpp/grpcpp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

// ── free-function helpers
// ─────────────────────────────────────────────────────

std::string env_string(const char *name, const std::string &fallback = {}) {
  if (const char *v = std::getenv(name))
    return v;
  return fallback;
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("failed to read file: " + path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::shared_ptr<grpc::ChannelCredentials> channel_credentials_from_env() {
  if (env_string("TSH_GRPC_INSECURE_DEV") == "1") {
    if (env_string("TSH_I_KNOW_THIS_IS_INSECURE") != "1")
      throw std::runtime_error("TSH_GRPC_INSECURE_DEV=1 requires "
                               "TSH_I_KNOW_THIS_IS_INSECURE=1");
    std::cerr << "[TinyShell Agent] WARNING: using insecure gRPC "
                 "because TSH_GRPC_INSECURE_DEV=1\n";
    return grpc::InsecureChannelCredentials();
  }

  grpc::SslCredentialsOptions opts;
  const auto root_ca = env_string("TSH_GRPC_ROOT_CA");
  if (!root_ca.empty())
    opts.pem_root_certs = read_file(root_ca);

  const auto cert = env_string("TSH_GRPC_CLIENT_CERT");
  const auto key = env_string("TSH_GRPC_CLIENT_KEY");
  if (!cert.empty() || !key.empty()) {
    if (cert.empty() || key.empty())
      throw std::runtime_error("both TSH_GRPC_CLIENT_CERT and "
                               "TSH_GRPC_CLIENT_KEY are required");
    opts.pem_cert_chain = read_file(cert);
    opts.pem_private_key = read_file(key);
  }
  return grpc::SslCredentials(opts);
}

bool valid_job_id(const std::string &job_id) {
  if (job_id.empty() || job_id.size() > 128)
    return false;
  for (unsigned char ch : job_id)
    if (!(std::isalnum(ch) || ch == '-' || ch == '_'))
      return false;
  return job_id.find("..") == std::string::npos;
}

// ── AgentRuntime
// ──────────────────────────────────────────────────────────────

class AgentRuntime {
public:
  AgentRuntime(std::string agent_id, tsh::spine::JobSigner signer,
               std::filesystem::path state_dir)
      : agent_id_(std::move(agent_id)), signer_(std::move(signer)),
        state_dir_(std::move(state_dir)) {
    std::filesystem::create_directories(state_dir_);
    jobs_dir_ = state_dir_ / "jobs";
    std::filesystem::create_directories(jobs_dir_);
    seen_jobs_path_ = state_dir_ / "seen_jobs";
    load_seen_jobs();
  }

  // Owns threads — must not be copied or moved.
  AgentRuntime(const AgentRuntime &) = delete;
  AgentRuntime &operator=(const AgentRuntime &) = delete;

  void run(const std::string &target) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(4 * 1024 * 1024);
    args.SetMaxSendMessageSize(4 * 1024 * 1024);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 20'000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10'000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

    auto channel =
        grpc::CreateCustomChannel(target, channel_credentials_from_env(), args);
    auto stub = tinyshell::v1::AgentConnector::NewStub(channel);

    grpc::ClientContext context;
    auto stream = stub->Connect(&context);
    stream_ = stream.get();

    std::atomic<bool> running{true};

    // ── writer thread ────────────────────────────────────────────────────
    std::thread writer([&] {
      try {
        writer_loop();
      } catch (const std::exception &e) {
        std::cerr << "[TinyShell Agent] writer failed: " << e.what() << "\n";
        context.TryCancel();
        running.store(false);
      }
    });

    // Announce presence and recover any crash-interrupted jobs.
    {
      tinyshell::v1::AgentMessage hello;
      hello.mutable_hello()->set_agent_id(agent_id_);
      hello.mutable_hello()->set_hostname(hostname());
      hello.mutable_hello()->add_capabilities("execve");
      hello.mutable_hello()->add_capabilities("stdout_stderr_streaming");
      write(hello);
    }
    emit_recovered_unfinished_jobs();

    // ── heartbeat thread ─────────────────────────────────────────────────
    std::thread heartbeat([&] {
      while (running.load()) {
        try {
          tinyshell::v1::AgentMessage msg;
          msg.mutable_heartbeat()->set_agent_id(agent_id_);
          msg.mutable_heartbeat()->set_timestamp_unix_ms(
              tsh::spine::now_unix_ms());
          write(msg);
        } catch (const std::exception &e) {
          std::cerr << "[TinyShell Agent] heartbeat failed: " << e.what()
                    << "\n";
          context.TryCancel();
          running.store(false);
          break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
    });

    // ── stream read loop ─────────────────────────────────────────────────
    // execute_job() is dispatched to a worker thread so stream->Read()
    // is never blocked for the duration of a job.
    tinyshell::v1::ServerMessage server_msg;
    while (stream->Read(&server_msg)) {
      if (!server_msg.has_job())
        continue;

      const auto signed_spec = server_msg.job(); // copy for the lambda

      // ── admission control ────────────────────────────────────────────
      // BUG #7 fix: increment before spawning (inside the lock) so the
      // counter always matches the number of live threads.  If thread
      // construction throws, decrement_jobs() undoes the increment so
      // wait_for_jobs() cannot hang.
      {
        std::lock_guard<std::mutex> lk(jobs_cv_mutex_);
        if (job_count_ >= kMaxConcurrentJobs) {
          const auto &jid = signed_spec.spec().job_id();
          if (!jid.empty()) {
            tinyshell::v1::AgentMessage reject;
            auto *ev = reject.mutable_event();
            ev->set_job_id(jid);
            ev->set_type(tinyshell::v1::JOB_FAILED);
            ev->set_timestamp_unix_ms(tsh::spine::now_unix_ms());
            ev->set_actor("agent");
            ev->set_agent_id(agent_id_);
            ev->set_source("agent");
            ev->set_correlation_id(jid);
            ev->set_message("agent at capacity: too many concurrent jobs");
            write(reject);
          }
          continue;
        }
        ++job_count_; // BUG #7 fix: increment before spawn
      }

      // BUG #1 fix: store thread in workers_ instead of detaching.
      // BUG #2 fix: outer catch(...) guarantees decrement_jobs() runs.
      // BUG #7 fix: if emplace_back throws, decrement undoes the
      //             pre-increment so wait_for_jobs() doesn't hang.
      try {
        std::lock_guard<std::mutex> lk(workers_mutex_);
        workers_.emplace_back([this, signed_spec] {
          try {
            execute_job(signed_spec);
          } catch (...) {
            // execute_job catches std::exception internally.
            // This outer catch is a last-resort guard for anything
            // else (e.g. a protobuf internal, std::bad_alloc).
          }
          decrement_jobs(); // always reached — BUG #2 fix
        });
      } catch (...) {
        // BUG #7 fix: undo the pre-increment on spawn failure.
        decrement_jobs();
        throw;
      }
    }

    // ── shutdown ─────────────────────────────────────────────────────────
    running.store(false);
    heartbeat.join();

    // BUG #1 fix: drain the counter first, then join every stored thread
    // so no worker is still running when the gRPC stream is destroyed.
    wait_for_jobs();
    {
      std::lock_guard<std::mutex> lk(workers_mutex_);
      for (auto &w : workers_)
        if (w.joinable())
          w.join();
      workers_.clear();
    }

    close_writer();
    if (writer.joinable())
      writer.join();

    // Only call WritesDone/Finish when the writer did not already close
    // the stream (avoids gRPC calling std::terminate() on a dead stream).
    if (!write_closed_) {
      try {
        stream->WritesDone();
      } catch (...) {
      }
    }
    grpc::Status status;
    try {
      status = stream->Finish();
    } catch (...) {
    }
    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED) {
      throw std::runtime_error("agent stream failed: " +
                               status.error_message());
    }
  }

private:
  // ── constants ─────────────────────────────────────────────────────────────
  static constexpr int kMaxConcurrentJobs = 16;
  static constexpr std::size_t kMaxOutboundQueueDepth = 4096;
  static constexpr std::size_t kMaxSeenJobs = 100000;

  // ── job-count bookkeeping ─────────────────────────────────────────────────
  // BUG #6 fix: increment_jobs() removed — it was never called (the read
  // loop incremented inline) and its presence was a maintenance hazard.

  void decrement_jobs() {
    {
      std::lock_guard<std::mutex> lk(jobs_cv_mutex_);
      --job_count_;
    }
    jobs_cv_.notify_all();
  }

  void wait_for_jobs() {
    std::unique_lock<std::mutex> lk(jobs_cv_mutex_);
    jobs_cv_.wait(lk, [this] { return job_count_ == 0; });
  }

  // ── misc ──────────────────────────────────────────────────────────────────

  std::string hostname() const {
    char buf[256]{};
    if (gethostname(buf, sizeof(buf) - 1) == 0)
      return buf;
    return "unknown";
  }

  // ── outbound write queue ──────────────────────────────────────────────────

  void write(const tinyshell::v1::AgentMessage &msg) {
    std::unique_lock<std::mutex> lock(write_mutex_);
    write_cv_.wait(lock, [this] {
      return write_closed_ || write_queue_.size() < kMaxOutboundQueueDepth;
    });
    if (write_closed_)
      throw std::runtime_error("agent writer is closed");
    write_queue_.push_back(msg);
    lock.unlock();
    write_cv_.notify_one();
  }

  void writer_loop() {
    while (true) {
      tinyshell::v1::AgentMessage msg;
      {
        std::unique_lock<std::mutex> lock(write_mutex_);
        write_cv_.wait(
            lock, [this] { return write_closed_ || !write_queue_.empty(); });
        if (write_queue_.empty())
          return; // write_closed_ == true, exit cleanly
        msg = std::move(write_queue_.front());
        write_queue_.pop_front();
      }
      write_cv_.notify_all();
      if (!stream_->Write(msg)) {
        close_writer();
        throw std::runtime_error("failed to write agent message");
      }
    }
  }

  void close_writer() {
    {
      std::lock_guard<std::mutex> lk(write_mutex_);
      write_closed_ = true;
    }
    write_cv_.notify_all();
  }

  void emit_event(const std::string &job_id, tinyshell::v1::JobEventType type,
                  std::function<void(tinyshell::v1::JobEvent &)> fill) {
    tinyshell::v1::AgentMessage msg;
    auto *ev = msg.mutable_event();
    ev->set_job_id(job_id);
    ev->set_type(type);
    ev->set_timestamp_unix_ms(tsh::spine::now_unix_ms());
    ev->set_actor("agent");
    ev->set_agent_id(agent_id_);
    ev->set_source("agent");
    ev->set_correlation_id(job_id);
    fill(*ev);
    write(msg);
  }

  // ── core execution ────────────────────────────────────────────────────────

  void execute_job(const tinyshell::v1::SignedJobSpec &signed_spec) {
    const auto &spec = signed_spec.spec();
    try {
      // ── validation ───────────────────────────────────────────────────
      if (!signer_.verify(signed_spec))
        throw std::runtime_error("JobSpec signature verification failed");

      if (!valid_job_id(spec.job_id()))
        throw std::runtime_error("JobSpec job_id has invalid format");

      if (spec.agent_id() != agent_id_)
        throw std::runtime_error("JobSpec is bound to a different agent");

      if (spec.expires_at_unix_ms() < tsh::spine::now_unix_ms())
        throw std::runtime_error("JobSpec has expired");

      if (spec.command().empty() || spec.command().front() != '/')
        throw std::runtime_error("JobSpec command must be an absolute path");

      // BUG #4 fix: error_code overload — never throws filesystem_error.
      std::error_code ec;
      const auto command_path = std::filesystem::canonical(spec.command(), ec);
      if (ec)
        throw std::runtime_error("failed to canonicalize command path: " +
                                 ec.message());

      bool path_allowed = false;
      for (const auto &raw : spec.policy().allowed_absolute_paths()) {
        std::error_code ec2;
        const auto p = std::filesystem::canonical(raw, ec2);
        if (!ec2 && p == command_path) {
          path_allowed = true;
          break;
        }
      }
      if (!path_allowed)
        throw std::runtime_error("JobSpec command is outside policy allowlist");

      remember_job_once(spec.job_id());
      journal_started(signed_spec);

      emit_event(spec.job_id(), tinyshell::v1::JOB_AGENT_ACCEPTED,
                 [](tinyshell::v1::JobEvent &ev) {
                   ev.set_message("signature and policy verified");
                 });
      emit_event(spec.job_id(), tinyshell::v1::JOB_STARTED,
                 [](tinyshell::v1::JobEvent &ev) {
                   ev.set_message("sandboxed execve started");
                 });

      tsh::spine::SandboxExecutor executor;
      const auto result = executor.run(
          spec, [&](tinyshell::v1::StreamName stream, std::uint64_t offset,
                    const std::string &data) {
            emit_event(spec.job_id(),
                       stream == tinyshell::v1::STDOUT
                           ? tinyshell::v1::JOB_STDOUT
                           : tinyshell::v1::JOB_STDERR,
                       [&](tinyshell::v1::JobEvent &ev) {
                         auto *chunk = ev.mutable_output();
                         chunk->set_job_id(spec.job_id());
                         chunk->set_stream(stream);
                         chunk->set_offset(offset);
                         chunk->set_data(data);
                       });
          });

      tinyshell::v1::JobEventType terminal_type = tinyshell::v1::JOB_FAILED;
      switch (result.failure) {
      case tsh::spine::ExecutionFailure::NONE:
        terminal_type = tinyshell::v1::JOB_EXITED;
        break;
      case tsh::spine::ExecutionFailure::TIMEOUT:
        terminal_type = tinyshell::v1::JOB_TIMED_OUT;
        break;
      case tsh::spine::ExecutionFailure::OUTPUT_LIMIT:
        terminal_type = tinyshell::v1::JOB_KILLED;
        break;
      case tsh::spine::ExecutionFailure::SIGNAL_TERMINATED:
      case tsh::spine::ExecutionFailure::SECCOMP_VIOLATION:
      case tsh::spine::ExecutionFailure::EXECVE_FAILED:
      case tsh::spine::ExecutionFailure::INTERNAL_ERROR:
        terminal_type = tinyshell::v1::JOB_FAILED;
        break;
      }

      emit_event(spec.job_id(), terminal_type,
                 [&](tinyshell::v1::JobEvent &ev) {
                   auto *exit = ev.mutable_exit();
                   exit->set_job_id(spec.job_id());
                   exit->set_exit_code(result.exit_code);
                   exit->set_reason(result.reason);
                   exit->set_signal(result.signal);
                   exit->set_timed_out(result.timed_out);
                   exit->set_signaled(result.signaled);
                   exit->set_runtime_ms(result.runtime_ms);
                   exit->set_stdout_bytes(result.stdout_bytes);
                   exit->set_stderr_bytes(result.stderr_bytes);
                   exit->set_stdout_truncated(result.stdout_truncated);
                   exit->set_stderr_truncated(result.stderr_truncated);
                   exit->set_failure_class(
                       tsh::spine::failure_class_string(result.failure));
                 });

      journal_finished(spec.job_id());

    } catch (const std::exception &e) {
      emit_event(
          spec.job_id(), tinyshell::v1::JOB_FAILED,
          [&](tinyshell::v1::JobEvent &ev) { ev.set_message(e.what()); });
      if (!spec.job_id().empty())
        journal_finished(spec.job_id());
    }
  }
  // ── journal ───────────────────────────────────────────────────────────────

  void journal_started(const tinyshell::v1::SignedJobSpec &signed_spec) {
    const auto &job_id = signed_spec.spec().job_id();
    if (!valid_job_id(job_id))
      throw std::runtime_error("refusing to journal invalid job_id");

    std::string serialized;
    if (!signed_spec.SerializeToString(&serialized))
      throw std::runtime_error("failed to serialize job journal entry");

    const auto tmp = jobs_dir_ / (job_id + ".tmp");
    const auto inflight = jobs_dir_ / (job_id + ".inflight");
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out)
        throw std::runtime_error("failed to open job execution journal");
      out.write(serialized.data(),
                static_cast<std::streamsize>(serialized.size()));
      out.flush();
      if (!out)
        throw std::runtime_error("failed to write job execution journal");
    }
    fsync_file(tmp);
    std::filesystem::rename(tmp, inflight);
    fsync_directory(jobs_dir_);
  }

  void journal_finished(const std::string &job_id) {
    if (!valid_job_id(job_id))
      return;
    const auto inflight = jobs_dir_ / (job_id + ".inflight");
    const auto done = jobs_dir_ / (job_id + ".done");
    if (std::filesystem::exists(inflight)) {
      std::filesystem::rename(inflight, done);
      fsync_directory(jobs_dir_);
    }
  }

  void emit_recovered_unfinished_jobs() {
    for (const auto &entry : std::filesystem::directory_iterator(jobs_dir_)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".inflight")
        continue;

      tinyshell::v1::SignedJobSpec signed_spec;
      {
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in || !signed_spec.ParseFromIstream(&in) ||
            signed_spec.spec().job_id().empty())
          continue;
      }
      const auto job_id = signed_spec.spec().job_id();
      // FIX (Bug 4): emit_event's callback is std::function<void(JobEvent &)>
      // — a reference, not a pointer.  The original lambda took auto *ev and
      // used -> which caused a compile error.  Changed to auto &ev and ..
      emit_event(job_id, tinyshell::v1::JOB_LOST,
                 [&](tinyshell::v1::JobEvent &ev) {
                   ev.set_message("agent recovered an unfinished "
                                  "local execution journal entry");
                 });
      journal_finished(job_id);
    }
  }

  // ── replay protection ─────────────────────────────────────────────────────

  void load_seen_jobs() {
    // Single-threaded at construction time — no lock needed.
    std::ifstream in(seen_jobs_path_);
    std::string job_id;
    while (std::getline(in, job_id))
      if (!job_id.empty())
        seen_jobs_order_.push_back(job_id);

    while (seen_jobs_order_.size() > kMaxSeenJobs)
      seen_jobs_order_.pop_front();

    for (const auto &id : seen_jobs_order_)
      seen_jobs_.insert(id);

    if (seen_jobs_order_.size() == kMaxSeenJobs)
      rewrite_seen_jobs_locked();
  }

  void remember_job_once(const std::string &job_id) {
    if (!valid_job_id(job_id))
      throw std::runtime_error("JobSpec job_id has invalid format");

    std::lock_guard<std::mutex> lk(seen_jobs_mutex_);
    if (seen_jobs_.find(job_id) != seen_jobs_.end())
      throw std::runtime_error("JobSpec replay rejected");

    std::ofstream out(seen_jobs_path_, std::ios::app);
    if (!out)
      throw std::runtime_error("failed to open agent replay journal");
    out << job_id << '\n';
    out.flush();
    if (!out)
      throw std::runtime_error("failed to write agent replay journal");
    out.close();
    fsync_file(seen_jobs_path_);

    seen_jobs_.insert(job_id);
    seen_jobs_order_.push_back(job_id);

    if (seen_jobs_order_.size() > kMaxSeenJobs) {
      while (seen_jobs_order_.size() > kMaxSeenJobs) {
        seen_jobs_.erase(seen_jobs_order_.front());
        seen_jobs_order_.pop_front();
      }
      rewrite_seen_jobs_locked();
    }
  }

  void rewrite_seen_jobs_locked() {
    const auto tmp = seen_jobs_path_.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      if (!out)
        throw std::runtime_error("failed to rewrite agent replay journal");
      for (const auto &id : seen_jobs_order_)
        out << id << '\n';
      out.flush();
      if (!out)
        throw std::runtime_error(
            "failed to flush agent replay journal rewrite");
    }
    fsync_file(tmp);
    std::filesystem::rename(tmp, seen_jobs_path_);
    fsync_directory(state_dir_);
  }

  // ── fsync helpers ─────────────────────────────────────────────────────────

  static void fsync_file(const std::filesystem::path &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      throw std::runtime_error("failed to open journal file for fsync");
    if (fsync(fd) != 0) {
      close(fd);
      throw std::runtime_error("failed to fsync journal file");
    }
    close(fd);
  }

  static void fsync_directory(const std::filesystem::path &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
      throw std::runtime_error("failed to open journal directory for fsync");
    if (fsync(fd) != 0) {
      close(fd);
      throw std::runtime_error("failed to fsync journal directory");
    }
    close(fd);
  }

  // ── member data ───────────────────────────────────────────────────────────

  std::string agent_id_;
  tsh::spine::JobSigner signer_;
  std::filesystem::path state_dir_;
  std::filesystem::path jobs_dir_;
  std::filesystem::path seen_jobs_path_;

  // Replay-protection journal — guarded by seen_jobs_mutex_.
  std::mutex seen_jobs_mutex_;
  std::unordered_set<std::string> seen_jobs_;
  std::deque<std::string> seen_jobs_order_;

  // Outbound gRPC write queue — guarded by write_mutex_.
  std::mutex write_mutex_;
  std::condition_variable write_cv_;
  std::deque<tinyshell::v1::AgentMessage> write_queue_;
  bool write_closed_ = false;
  grpc::ClientReaderWriterInterface<tinyshell::v1::AgentMessage,
                                    tinyshell::v1::ServerMessage> *stream_ =
      nullptr;

  // BUG #1 fix — owned worker threads (replaces detach()).
  std::mutex workers_mutex_;
  std::vector<std::thread> workers_;

  // In-flight job counter + condvar — guarded by jobs_cv_mutex_.
  std::mutex jobs_cv_mutex_;
  std::condition_variable jobs_cv_;
  int job_count_{0};
};

} // namespace

// ── main
// ──────────────────────────────────────────────────────────────────────

int main() {
  try {
    // BUG #5 fix — root check before AgentRuntime is constructed.
    // Previously this check ran inside execute_job(), after the job had
    // already been journalled as .inflight and JOB_STARTED emitted, which
    // left a permanently stuck entry that recovered as JOB_LOST on every
    // subsequent restart.
    if (geteuid() == 0 && env_string("TSH_ALLOW_ROOT_AGENT_EXEC") != "1") {
      std::cerr << "[TinyShell Agent] refusing to run as root; "
                   "set TSH_ALLOW_ROOT_AGENT_EXEC=1 to override\n";
      return 1;
    }

    const auto target =
        env_string("TSH_AGENT_CONNECT_TARGET",
                   env_string("TSH_SPINE_TARGET", "127.0.0.1:7444"));
    const auto agent_id = env_string("TSH_AGENT_ID", "agent-local-1");
    const auto signing_secret =
        tsh::Config::read_string("TSH_JOB_SIGNING_KEY", "");
    if (signing_secret.empty())
      throw std::runtime_error("TSH_JOB_SIGNING_KEY is required");
    const auto key_id = env_string("TSH_JOB_KEY_ID", "local-hmac-v1");
    const auto default_state_dir = env_string("HOME", "/tmp") +
                                   "/.local/state/tinyshell/agent-" + agent_id;
    const auto state_dir = env_string("TSH_AGENT_STATE_DIR", default_state_dir);

    AgentRuntime runtime(
        agent_id, tsh::spine::JobSigner(key_id, signing_secret), state_dir);

    int backoff_ms = 500;
    while (true) {
      try {
        runtime.run(target);
        backoff_ms = 500;
      } catch (const std::exception &e) {
        std::cerr << "[TinyShell Agent] stream failed: " << e.what() << "\n";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
      backoff_ms = std::min(backoff_ms * 2, 30'000);
    }
  } catch (const std::exception &e) {
    std::cerr << "[TinyShell Agent] failed: " << e.what() << "\n";
    return 1;
  }
}