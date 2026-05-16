#pragma once

#include <atomic>

namespace tsh {
class CapabilityManager;
class Metrics;
class PipelineValidator;
class StructuredAuditLogger;
class TuiCore;
class ZkAuditTrail;
}

class Server {
public:
    Server(int port = 4444);
    void run();
    static void handle_client(int client_fd,
                              tsh::TuiCore& tui,
                              const tsh::CapabilityManager& cap_manager,
                              tsh::PipelineValidator& ast_validator,
                              tsh::StructuredAuditLogger& audit_logger,
                              tsh::ZkAuditTrail& zk_ledger,
                              std::atomic<int>& global_risk,
                              std::atomic<bool>& global_drift,
                              tsh::Metrics& metrics);
private:
    int port;
};
