#pragma once
#include "ExecutionEventBus.h"
#include "JobScheduler.h"
#include "Metrics.h"
#include "tinyshell/v1/spine.grpc.pb.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tsh {

class SpineEventBridge {
public:
  SpineEventBridge(ExecutionEventBus *bus, JobScheduler *scheduler,
                   Metrics *metrics,
                   const std::string &spine_addr)
      : bus_(bus), scheduler_(scheduler), metrics_(metrics) {
    if (spine_addr.empty())
      return;
    auto channel = grpc::CreateChannel(spine_addr, credentials());
    stub_ = tinyshell::v1::ControlPlaneService::NewStub(channel);

    if (stub_) {
      worker_sync_thread_ = std::thread([this]() { sync_worker_registry(); });
    }
  }

  // Called from HttpApi whenever a job is submitted through the Spine path.
  void watch_job(const std::string &job_id) {
    if (!stub_)
      return;
    std::lock_guard<std::mutex> lk(threads_mutex_);
    threads_.emplace_back([this, job_id]() { stream_job(job_id); });
  }

  void stop() {
    running_.store(false);
    cancel_contexts();
    if (worker_sync_thread_.joinable())
      worker_sync_thread_.join();
    std::lock_guard<std::mutex> lk(threads_mutex_);
    for (auto &t : threads_)
      if (t.joinable())
        t.join();
  }

  ~SpineEventBridge() { stop(); }

private:
  void stream_job(const std::string &job_id) {
    tinyshell::v1::WatchJobRequest req;
    req.set_job_id(job_id);
    grpc::ClientContext ctx;
    register_context(&ctx);
    auto reader = stub_->WatchJob(&ctx, req);
    tinyshell::v1::JobEvent spine_event;
    const auto started = std::chrono::steady_clock::now();
    bool recorded_metric = false;
    std::string command;
    while (running_.load() && reader->Read(&spine_event)) {
      if (spine_event.has_job_spec()) {
        command = spine_event.job_spec().command();
      }

      auto event = translate(spine_event);
      if (event.command.empty())
        event.command = command;
      if (bus_)
        bus_->publish(std::move(event));

      if (!recorded_metric && is_terminal(spine_event.type())) {
        record_terminal_metric(spine_event, started);
        recorded_metric = true;
      }
    }
    reader->Finish();
    unregister_context(&ctx);
  }

  void sync_worker_registry() {
    tinyshell::v1::WatchAgentsRequest req;
    grpc::ClientContext ctx;
    register_context(&ctx);
    auto reader = stub_->WatchAgents(&ctx, req);
    tinyshell::v1::AgentStatusEvent event;
    while (running_.load() && reader->Read(&event)) {
      if (!scheduler_)
        continue;
      const auto &id = event.agent_id();
      switch (event.status()) {
      case tinyshell::v1::AGENT_STATUS_CONNECTED:
      case tinyshell::v1::AGENT_STATUS_HEARTBEAT:
        scheduler_->mark_node_healthy(id);
        break;
      case tinyshell::v1::AGENT_STATUS_DISCONNECTED:
        scheduler_->mark_node_unreachable(id);
        break;
      default:
        break;
      }
    }
    reader->Finish();
    unregister_context(&ctx);
  }

  void register_context(grpc::ClientContext *ctx) {
    std::lock_guard<std::mutex> lock(contexts_mutex_);
    active_contexts_.push_back(ctx);
  }

  void unregister_context(grpc::ClientContext *ctx) {
    std::lock_guard<std::mutex> lock(contexts_mutex_);
    active_contexts_.erase(
        std::remove(active_contexts_.begin(), active_contexts_.end(), ctx),
        active_contexts_.end());
  }

  void cancel_contexts() {
    std::vector<grpc::ClientContext *> contexts;
    {
      std::lock_guard<std::mutex> lock(contexts_mutex_);
      contexts = active_contexts_;
    }
    for (auto *ctx : contexts) {
      if (ctx)
        ctx->TryCancel();
    }
  }

  void record_terminal_metric(
      const tinyshell::v1::JobEvent &event,
      std::chrono::steady_clock::time_point started) const {
    if (!metrics_)
      return;

    const bool ok = event.type() == tinyshell::v1::JOB_EXITED &&
                    (!event.has_exit() || event.exit().exit_code() == 0);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (event.has_exit() && event.exit().runtime_ms() > 0) {
      duration = std::chrono::milliseconds(event.exit().runtime_ms());
    }
    metrics_->record_command(ok, duration);
  }

  static bool is_terminal(tinyshell::v1::JobEventType type) {
    using T = tinyshell::v1::JobEventType;
    return type == T::JOB_EXITED || type == T::JOB_FAILED ||
           type == T::JOB_TIMED_OUT || type == T::JOB_KILLED ||
           type == T::JOB_LOST || type == T::JOB_REJECTED;
  }

  static std::string state_name(tinyshell::v1::JobState state,
                                tinyshell::v1::JobEventType type) {
    using S = tinyshell::v1::JobState;
    switch (state) {
    case S::JOB_STATE_PENDING:
      return "pending";
    case S::JOB_STATE_ASSIGNED:
      return "assigned";
    case S::JOB_STATE_DELIVERED:
      return "delivered";
    case S::JOB_STATE_STARTING:
      return "starting";
    case S::JOB_STATE_RUNNING:
      return "running";
    case S::JOB_STATE_STREAMING:
      return "streaming";
    case S::JOB_STATE_EXITED:
      return "completed";
    case S::JOB_STATE_FAILED:
      return "failed";
    case S::JOB_STATE_TIMED_OUT:
      return "timeout";
    case S::JOB_STATE_KILLED:
      return "killed";
    case S::JOB_STATE_LOST:
      return "lost";
    case S::JOB_STATE_REJECTED:
      return "rejected";
    default:
      break;
    }

    using T = tinyshell::v1::JobEventType;
    if (type == T::JOB_EXITED)
      return "completed";
    if (is_terminal(type))
      return "failed";
    if (type == T::JOB_STDOUT)
      return "stdout";
    if (type == T::JOB_STDERR)
      return "stderr";
    return "running";
  }

  static ExecutionEvent translate(const tinyshell::v1::JobEvent &e) {
    ExecutionEvent out;
    out.request_id = e.job_id();
    out.user = e.actor();
    out.worker = e.agent_id();
    out.command = e.has_job_spec() ? e.job_spec().command() : "";
    out.timestamp = std::to_string(e.timestamp_unix_ms());
    out.detail = e.message();
    out.state = state_name(e.state(), e.type());

    using T = tinyshell::v1::JobEventType;
    switch (e.type()) {
    case T::JOB_CREATED:
    case T::JOB_ASSIGNED:
      out.type = ExecutionEventType::ExecutionStarted;
      break;
    case T::JOB_STDOUT:
      out.type = ExecutionEventType::StdoutChunk;
      if (e.has_output())
        out.detail = e.output().data();
      break;
    case T::JOB_STDERR:
      out.type = ExecutionEventType::StderrChunk;
      if (e.has_output())
        out.detail = e.output().data();
      break;
    case T::JOB_EXITED:
      out.type = ExecutionEventType::ExecutionCompleted;
      if (e.has_exit()) {
        out.duration_ms = e.exit().runtime_ms();
        out.detail = e.exit().reason();
      }
      break;
    case T::JOB_FAILED:
    case T::JOB_TIMED_OUT:
    case T::JOB_KILLED:
    case T::JOB_LOST:
    case T::JOB_REJECTED:
      out.type = ExecutionEventType::ExecutionFailed;
      break;
    default:
      out.type = ExecutionEventType::ExecutionStarted;
      break;
    }
    out.result =
        (out.type == ExecutionEventType::ExecutionCompleted) ? "ok" : "error";
    return out;
  }

  static std::string env(const char *name) {
    if (const char *value = std::getenv(name)) {
      return value;
    }
    return {};
  }

  static std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      throw std::runtime_error("failed to read file: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }

  static std::shared_ptr<grpc::ChannelCredentials> credentials() {
    if (env("TSH_GRPC_INSECURE_DEV") == "1") {
      return grpc::InsecureChannelCredentials();
    }

    grpc::SslCredentialsOptions opts;
    const auto root = env("TSH_GRPC_ROOT_CA");
    if (!root.empty()) {
      opts.pem_root_certs = read_file(root);
    }
    const auto cert = env("TSH_GRPC_CLIENT_CERT");
    const auto key = env("TSH_GRPC_CLIENT_KEY");
    if (!cert.empty() || !key.empty()) {
      if (cert.empty() || key.empty()) {
        throw std::runtime_error(
            "both TSH_GRPC_CLIENT_CERT and TSH_GRPC_CLIENT_KEY are required");
      }
      opts.pem_cert_chain = read_file(cert);
      opts.pem_private_key = read_file(key);
    }
    return grpc::SslCredentials(opts);
  }

  ExecutionEventBus *bus_;
  std::unique_ptr<tinyshell::v1::ControlPlaneService::Stub> stub_;
  std::atomic<bool> running_{true};
  JobScheduler *scheduler_ = nullptr;
  Metrics *metrics_ = nullptr;
  std::thread worker_sync_thread_;
  std::mutex threads_mutex_;
  std::vector<std::thread> threads_;
  std::mutex contexts_mutex_;
  std::vector<grpc::ClientContext *> active_contexts_;
};

} // namespace tsh
