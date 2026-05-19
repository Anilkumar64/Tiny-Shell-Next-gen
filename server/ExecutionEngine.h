#pragma once

#include "../common/Parser.h"
#include "AstSerializer.h"
#include "ClusterHealthManager.h"
#include "ExecutionEventBus.h"
#include "ExecutionTimeout.h"
#include "IntentDrift.h"
#include "JobScheduler.h"
#include "Pipeline.h"
#include "PipelineValidator.h"
#include "RbacManager.h"
#include "RemoteWorkerClient.h"
#include "RiskScorer.h"
#include "TaintTracker.h"
#include "UnitValidator.h"
#include "WorkerTypes.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tsh {

struct ExecutionRequest {
  std::string request_id;
  std::string user = "api-client";
  std::string tenant = "default";
  std::string client_ip = "127.0.0.1";
  std::string command;
};

struct ExecutionResult {
  bool ok = false;
  std::string output;
  std::string error;
  std::string worker;
  long long duration_ms = 0;
};

class ExecutionEngine {
public:
  ExecutionEngine(ExecutionEventBus &events, std::atomic<int> &risk,
                  std::atomic<bool> &drift, JobScheduler *scheduler = nullptr)
      : events_(events), global_risk_(risk), global_drift_(drift),
        scheduler_(scheduler) {}

  void set_workers(std::vector<RemoteNode> workers) {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    workers_ = std::move(workers);
    worker_health_.clear();
    for (const auto &worker : workers_) {
      worker_health_[worker_node_id(worker)] = WorkerHealthStatus::HEALTHY;
    }
  }

  void set_worker_health(const std::string &worker_id,
                         WorkerHealthStatus status) {
    // BUG: execution routing used a stale worker list and ignored health.
    // FIX: health probes update routing state before each worker selection.
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_health_[worker_id] = status;
  }

  ExecutionResult execute(const ExecutionRequest &request) {
    const auto started = std::chrono::steady_clock::now();
    ExecutionResult result;

    emit(request, ExecutionEventType::RequestReceived, "received",
         "Client requested: " + request.command);

    try {
      auto ast = Parser::parse_pipeline(request.command);
      if (!ast) {
        throw ValidationException(
            "Command is empty or outside parser allowlist");
      }
      emit(request, ExecutionEventType::AstParsed, "parsed",
           "Command parsed into AST");

      if (!rbac_.verify_permission("operator", Permission::EXECUTE_COMMAND)) {
        throw std::runtime_error("RBAC denied EXECUTE_COMMAND for operator");
      }
      emit(request, ExecutionEventType::RbacPassed, "rbac_passed",
           "Operator role may execute commands");

      validator_.validate(ast);
      emit(request, ExecutionEventType::AllowlistPassed, "allowlist_passed",
           "AST allowlist accepted root command");

      TaintTracker::enforce_data_flow(ast);
      UnitValidator::validate_units(ast);
      emit(request, ExecutionEventType::TaintPassed, "taint_passed",
           "Taint and unit checks passed");

      const int risk = RiskScorer::calculate_score(ast);
      global_risk_.store(risk);
      global_drift_.store(drift_detector_.detect_drift(risk));

      const auto worker = select_worker();
      result.worker = worker_node_id(worker);
      emit(request, ExecutionEventType::WorkerAssigned, "worker_assigned",
           "Assigned to " + result.worker, result.worker);
      emit(request, ExecutionEventType::ExecutionStarted, "running",
           "Worker execution started", result.worker);

      if (is_local_worker(worker)) {
        result.output = ExecutionGuard::execute_with_timeout(
            std::chrono::milliseconds(3000),
            [&]() { return Executor::execute(ast); });
      } else {
        const auto serialized = AstSerializer::serialize(ast);
        std::string streamed_output;
        const auto error = RemoteWorkerClient::execute_node_stream(
            serialized, worker, [&](const std::string &chunk, bool is_stderr) {
              streamed_output += chunk;
              emit(request,
                   is_stderr ? ExecutionEventType::StderrChunk
                             : ExecutionEventType::StdoutChunk,
                   is_stderr ? "stderr" : "stdout", chunk, result.worker,
                   chunk);
            });
        if (!error.empty()) {
          throw std::runtime_error(error);
        }
        result.output = streamed_output;
      }
      if (result.output.empty()) {
        result.output = "Command completed with no output.\n";
      } else if (result.output.back() != '\n') {
        result.output.push_back('\n');
      }

      result.ok = true;
      result.duration_ms = elapsed_ms(started);
      if (is_local_worker(worker)) {
        emit(request, ExecutionEventType::StdoutChunk, "stdout", result.output,
             result.worker, result.output);
      }
      emit(request, ExecutionEventType::ExecutionCompleted, "completed",
           "Execution completed", result.worker, result.output,
           result.duration_ms);
    } catch (const TaintException &e) {
      result =
          fail(request, started, "taint_violation", e.what(), result.worker);
    } catch (const ValidationException &e) {
      result =
          fail(request, started, "validation_failed", e.what(), result.worker);
    } catch (const TimeoutException &e) {
      result = fail(request, started, "timeout_kill", e.what(), result.worker);
    } catch (const std::exception &e) {
      result =
          fail(request, started, "execution_failed", e.what(), result.worker);
    }

    return result;
  }

private:
  RemoteNode select_worker() {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (workers_.empty()) {
      return RemoteNode{.host = "127.0.0.1", .port = 0, .id = "local-worker"};
    }
    for (std::size_t attempts = 0; attempts < workers_.size(); ++attempts) {
      const auto selected = workers_[next_worker_ % workers_.size()];
      ++next_worker_;
      const auto id = worker_node_id(selected);
      if (scheduler_) {
        if (scheduler_->is_node_healthy(id))
          return selected;
      } else {
        const auto health = worker_health_.find(id);
        if (health == worker_health_.end() ||
            health->second == WorkerHealthStatus::HEALTHY ||
            health->second == WorkerHealthStatus::DEGRADED) {
          return selected;
        }
      }
    }
    throw std::runtime_error("No healthy workers available");
  }

  static bool is_local_worker(const RemoteNode &worker) {
    return worker.id == "local-worker" || worker.port == 0;
  }

  static long long elapsed_ms(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
  }

  void emit(const ExecutionRequest &request, ExecutionEventType type,
            std::string state, std::string detail, std::string worker = {},
            std::string result = {}, long long duration_ms = 0) {
    ExecutionEvent event;
    event.type = type;
    event.request_id = request.request_id;
    event.user = request.user;
    event.tenant = request.tenant;
    event.client_ip = request.client_ip;
    event.command = request.command;
    event.worker = std::move(worker);
    event.state = std::move(state);
    event.detail = std::move(detail);
    event.result = std::move(result);
    event.duration_ms = duration_ms;
    events_.publish(std::move(event));
  }

  ExecutionResult fail(const ExecutionRequest &request,
                       std::chrono::steady_clock::time_point started,
                       const std::string &state, const std::string &message,
                       const std::string &worker) {
    ExecutionResult result;
    result.ok = false;
    result.error = message;
    result.worker = worker;
    result.duration_ms = elapsed_ms(started);
    const auto event_type =
        state == "taint_violation" || state == "validation_failed"
            ? ExecutionEventType::SecurityViolation
            : ExecutionEventType::ExecutionFailed;
    emit(request, event_type, state, message, worker, message,
         result.duration_ms);
    return result;
  }

  ExecutionEventBus &events_;
  std::atomic<int> &global_risk_;
  std::atomic<bool> &global_drift_;
  PipelineValidator validator_;
  RbacManager rbac_;
  IntentDrift drift_detector_;
  std::vector<RemoteNode> workers_;
  std::unordered_map<std::string, WorkerHealthStatus> worker_health_;
  std::size_t next_worker_ = 0;
  std::mutex worker_mutex_;
  JobScheduler *scheduler_ = nullptr;
};

} // namespace tsh
