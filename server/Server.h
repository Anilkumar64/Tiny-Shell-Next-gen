#pragma once

#include <atomic>
#include <mutex>

namespace tsh {
class CapabilityManager;
class ExecutionEventBus; // BUG FIX: added so handle_client can publish events
class IntentDrift;
class Metrics;
class PipelineValidator;
class StructuredAuditLogger;
class TuiCore;
class ZkAuditTrail;
} // namespace tsh

class Server {
public:
  Server(int port = 4444);
  void run();
  static void
  handle_client(int client_fd, tsh::TuiCore &tui,
                const tsh::CapabilityManager &cap_manager,
                tsh::PipelineValidator &ast_validator,
                tsh::StructuredAuditLogger &audit_logger,
                tsh::ZkAuditTrail &zk_ledger, std::atomic<int> &global_risk,
                std::atomic<bool> &global_drift,
                tsh::IntentDrift &drift_detector, std::mutex &drift_mutex,
                tsh::Metrics &metrics,
                tsh::ExecutionEventBus &event_bus); // BUG FIX: was missing

private:
  int port;
};