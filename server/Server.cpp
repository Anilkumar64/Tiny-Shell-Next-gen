#include "Server.h"
#include "../common/SecureChannel.h"
#include "../common/transport/TcpTransport.h"
#include "CapabilityManager.h"
#include "StateSnapshot.h"
#include "WasmEngine.h"
// Removed: anti-debugging and emergency wipe have no place in a legitimate
// production server.
#include "../common/Parser.h"
#include "../config/tsh_config.h"
#include "AdvancedSandbox.h"
#include "AgentIdentity.h"
#include "AstMinifier.h"
#include "BpfFilterCompiler.h"
#include "ControllerTrust.h"
#include "ExecutionEventBus.h" // BUG FIX: was missing — handle_client must publish events
#include "ExecutionTimeout.h"
#include "HttpApi.h"
#include "IntentDrift.h"
#include "JobScheduler.h"
#include "Metrics.h"
#include "Pipeline.h"
#include "PipelineValidator.h"
#include "PtySession.h"
#include "QueryPlanner.h"
#include "RiskScorer.h"
#include "SemanticDedup.h"
#include "StructuredAuditLogger.h"
#include "TaintTracker.h"
#include "ThreadPool.h"
#include "TuiCore.h"
#include "UnitValidator.h"
#include "ZkAuditTrail.h"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> g_running{true};
std::atomic<int> g_server_fd{-1};
constexpr const char *kControllerHelloPrefix = "controller-hello ";
constexpr const char *kRegisterPrefix = "register ";

void handle_signal(int) {
  // FIX[4.7]: Signal handlers request graceful shutdown and unblock accept().
  g_running.store(false);
  const int fd = g_server_fd.exchange(-1);
  if (fd >= 0)
    close(fd);
}

std::vector<tsh::RemoteNode> parse_worker_nodes_from_env() {
  std::vector<tsh::RemoteNode> workers;
  const char *raw = std::getenv("TSH_WORKERS");
  if (!raw || std::string(raw).empty()) {
    return workers;
  }

  std::stringstream stream(raw);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (item.empty())
      continue;

    std::string id;
    std::string endpoint = item;
    const auto at = item.find('@');
    if (at != std::string::npos) {
      id = item.substr(0, at);
      endpoint = item.substr(at + 1);
    }

    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
      std::cerr << "[TinyShell] Ignoring invalid TSH_WORKERS entry: " << item
                << "\n";
      continue;
    }

    tsh::RemoteNode worker;
    worker.host = endpoint.substr(0, colon);
    try {
      size_t consumed = 0;
      const auto port_text = endpoint.substr(colon + 1);
      worker.port = std::stoi(port_text, &consumed, 10);
      if (consumed != port_text.size() || worker.port <= 0 ||
          worker.port > 65535) {
        throw std::out_of_range("invalid port");
      }
    } catch (const std::exception &e) {
      std::cerr << "[TinyShell] Ignoring invalid TSH_WORKERS port in " << item
                << ": " << e.what() << "\n";
      continue;
    }
    worker.id =
        id.empty() ? worker.host + ":" + std::to_string(worker.port) : id;
    workers.push_back(std::move(worker));
  }
  return workers;
}
} // namespace

Server::Server(int p) : port(p) {}

// BUG FIX: Added `tsh::ExecutionEventBus &event_bus` parameter.
// Previously this parameter was missing entirely. handle_client had no
// reference to the event bus and therefore never published any events:
// no ClientConnected, no Authenticated, no ExecutionStarted, no
// ExecutionCompleted. The GUI dashboard (ClientConnectionPanel,
// ExecutionEventStreamWidget) polls /control/events which reads from that
// same event_bus — so the dashboard showed nothing, and the server page
// showed no connected clients, because zero events were ever published
// by the TCP path.
void Server::handle_client(
    int client_fd, tsh::TuiCore &tui, const tsh::CapabilityManager &cap_manager,
    tsh::PipelineValidator &ast_validator,
    tsh::StructuredAuditLogger &audit_logger, tsh::ZkAuditTrail &zk_ledger,
    std::atomic<int> &global_risk, std::atomic<bool> &global_drift,
    tsh::IntentDrift &drift_detector, std::mutex &drift_mutex,
    tsh::Metrics &metrics,
    tsh::ExecutionEventBus &event_bus) { // BUG FIX: added
  metrics.connection_opened();
  const auto connection_guard =
      std::unique_ptr<int, void (*)(int *)>(new int(client_fd), [](int *fd) {
        if (fd && *fd >= 0)
          close(*fd);
        delete fd;
      });
  try {
    auto transport = std::make_unique<TcpTransport>(client_fd);
    *connection_guard = -1;
    SecureChannel sc(std::move(transport));

    if (!sc.handshake(SecureChannel::Mode::SERVER)) {
      tui.log_message("[Security] Handshake failed.");
      metrics.connection_closed();
      return;
    }
    tui.log_message("[Crypto] Handshake successful (X25519/TOFU).");
    std::string controller_id;
    bool controller_trusted = false;

    // BUG FIX: Capture the client IP at connection time for use in events.
    // Previously no IP was available inside handle_client at all; now it is
    // recovered from the fd via getpeername so events carry the correct IP.
    std::string client_ip_str;
    {
      sockaddr_in peer{};
      socklen_t peer_len = sizeof(peer);
      if (getpeername(client_fd, reinterpret_cast<sockaddr *>(&peer),
                      &peer_len) == 0) {
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, buf, INET_ADDRSTRLEN);
        client_ip_str = buf;
      }
    }

    // BUG FIX: Publish ClientConnected so the server GUI shows the connection.
    // This event is what ClientConnectionPanel reads from /control/events to
    // populate the "Live Client Connections" table. Without it the table stayed
    // empty even with active TCP clients.
    {
      tsh::ExecutionEvent conn_event;
      conn_event.type = tsh::ExecutionEventType::ClientConnected;
      conn_event.client_ip = client_ip_str;
      conn_event.state = "connected";
      conn_event.detail = "X25519+AES256-GCM handshake complete";
      event_bus.publish(std::move(conn_event));
    }

    while (g_running.load()) {
      std::vector<uint8_t> msg;
      SecureChannel::MsgType type;
      if (!sc.receive_message(msg, type)) {
        break;
      }

      const auto started = std::chrono::steady_clock::now();
      bool ok = false;
      std::string response;
      try {
        if (type == SecureChannel::MsgType::COMMAND) {
          const std::string cmd(msg.begin(), msg.end());
          if (cmd.rfind(kControllerHelloPrefix, 0) == 0) {
            controller_id = cmd.substr(std::strlen(kControllerHelloPrefix));
            controller_trusted =
                tsh::ControllerTrust::is_trusted(controller_id);
            response = "Controller: " + controller_id + "\n";
            response += controller_trusted ? "Trust: registered\n"
                                           : "Trust: unregistered\n";
            ok = true;

            // BUG FIX: Publish Authenticated event so the dashboard can show
            // the client's identity (user/controller_id) and IP. Without this,
            // ClientConnectionPanel.pollClients() never gets an event with a
            // non-empty `user` field, so the client table never populates.
            tsh::ExecutionEvent auth_event;
            auth_event.type = tsh::ExecutionEventType::Authenticated;
            auth_event.user = controller_id;
            auth_event.client_ip = client_ip_str;
            auth_event.state = controller_trusted ? "trusted" : "unregistered";
            auth_event.detail = "controller-hello received";
            event_bus.publish(std::move(auth_event));

          } else if (cmd == "agent-info") {
            cap_manager.require("cap_fs_read");
            response = tsh::AgentIdentity::format_human(
                tsh::AgentIdentity::load_metadata());
            ok = true;
          } else if (cmd == "pairing-code") {
            cap_manager.require("cap_fs_read");
            const auto meta = tsh::AgentIdentity::load_metadata();
            response = "Pairing code: " + meta.pairing_code + "\n";
            response += "Agent ID: " + meta.agent_id + "\n";
            ok = true;
          } else if (cmd.rfind(kRegisterPrefix, 0) == 0) {
            if (controller_id.empty()) {
              response = "Registration denied: controller identity was not "
                         "announced.\n";
            } else {
              const auto meta = tsh::AgentIdentity::load_metadata();
              const auto provided_code =
                  cmd.substr(std::strlen(kRegisterPrefix));
              if (provided_code != meta.pairing_code) {
                response = "Registration denied: pairing code mismatch.\n";
              } else if (tsh::ControllerTrust::is_trusted(controller_id)) {
                controller_trusted = true;
                response = "Controller already registered.\n";
              } else if (tsh::ControllerTrust::approve_interactively(
                             controller_id, meta.agent_id)) {
                tsh::ControllerTrust::trust(controller_id);
                controller_trusted = true;
                audit_logger.log_event("system", "controller_register",
                                       controller_id, "ok");
                response = "Registration approved. Resource access granted.\n";
              } else {
                audit_logger.log_event("system", "controller_register",
                                       controller_id, "denied");
                response = "Registration denied by host user.\n";
              }
            }
            ok = controller_trusted;
          } else if (!controller_trusted) {
            response =
                "Controller not registered. Run: register <pairing-code>\n";
          } else if (cmd == "!rewind") {
            cap_manager.require("cap_state_restore");
            StateSnapshot::restore({});
            response = "State restored.\n";
          } else if (cmd == "shell") {
            cap_manager.require("cap_exec_shell");
            zk_ledger.log_secure_event(controller_id, "shell");
            response = tsh::PtySession::run(sc, audit_logger);
            // BUG-10 FIX: PtySession::run() returns "" on clean exit and
            // an error string on failure (e.g. forkpty returned -1).
            // Previously ok=true was set unconditionally, so forkpty errors
            // were counted as successes and emitted to the client as if the
            // session succeeded.
            ok = response.empty();
            if (response.empty()) {
              const auto elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started);
              metrics.record_command(ok, elapsed);
              continue;
            }
          } else {
            cap_manager.require("cap_exec_shell");
            auto ast = tsh::Parser::parse_pipeline(cmd);
            if (ast) {
              ast_validator.validate(ast);
              zk_ledger.log_secure_event(controller_id, cmd);
              auto current_node = ast;
              while (current_node) {
                if (current_node->type == tsh::OpType::FILTER) {
                  auto bpf = tsh::BpfFilterCompiler::compile(current_node);
                  tsh::BpfFilterCompiler::inject_to_kernel(bpf);
                }
                current_node = current_node->next;
              }

              const int risk = tsh::RiskScorer::calculate_score(ast);
              global_risk.store(risk);
              // FIX (Bug 5): was static locals; now uses the references passed
              // from run() so ownership and sharing are explicit.
              {
                std::lock_guard<std::mutex> lock(drift_mutex);
                global_drift.store(drift_detector.detect_drift(risk));
              }

              tsh::AstMinifier::minify(ast);
              tsh::TaintTracker::enforce_data_flow(ast);
              tsh::UnitValidator::validate_units(ast);

              // BUG FIX: Publish ExecutionStarted so the dashboard's
              // ExecutionEventStreamWidget shows commands as they run, and
              // worker_telemetry() can compute active_jobs / running_command.
              {
                tsh::ExecutionEvent start_event;
                start_event.type = tsh::ExecutionEventType::ExecutionStarted;
                start_event.user = controller_id;
                start_event.client_ip = client_ip_str;
                start_event.command = cmd;
                start_event.worker = "local-worker";
                start_event.state = "running";
                event_bus.publish(std::move(start_event));
              }

              if (!tsh::SemanticDedup::instance().try_dedup(ast, response)) {
                tsh::QueryPlanner::optimize(ast);
                response = tsh::ExecutionGuard::execute_with_timeout(
                    std::chrono::milliseconds(2000),
                    [&]() { return tsh::Executor::execute(ast); });
                tsh::SemanticDedup::instance().store_result(ast, response);
              }
              if (response.empty()) {
                response = "Command completed with no output.\n";
              } else if (response.back() != '\n') {
                response.push_back('\n');
              }
              ok = true;
            } else {
              response = "Empty command.\n";
            }
          }
        } else if (type == SecureChannel::MsgType::WASM_PAYLOAD) {
          cap_manager.require("cap_exec_wasm");
          response = WasmEngine::execute(msg);
          ok = true;
        } else {
          // FIX[2.2][2.4]: Disguised command channels and arbitrary binary
          // execution were removed.
          response = "Unsupported message type.\n";
        }
      } catch (const tsh::CapabilityException &e) {
        response = std::string("Security Alert: ") + e.what();
      } catch (const tsh::TimeoutException &e) {
        response = std::string("Execution Aborted: ") + e.what();
      } catch (const tsh::ValidationException &e) {
        response = std::string("AST Rejected: ") + e.what();
      } catch (const tsh::TaintException &e) {
        response = std::string("Taint Violation: ") + e.what();
      } catch (const tsh::UnitException &e) {
        response = std::string("Unit Mismatch: ") + e.what();
      } catch (const std::exception &e) {
        response = std::string("Command rejected: ") + e.what();
      }
      if (!response.empty() && response.back() != '\n') {
        response.push_back('\n');
      }

      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started);
      metrics.record_command(ok, elapsed);

      // BUG FIX: Publish ExecutionCompleted/ExecutionFailed after every
      // command so the dashboard event stream updates and worker telemetry
      // (active_jobs, running_command) reflects the finished state.
      if (!response.empty() && response.rfind("Command rejected:", 0) != 0 &&
          response.rfind("Security Alert:", 0) != 0 &&
          response.rfind("Execution Aborted:", 0) != 0 &&
          response.rfind("AST Rejected:", 0) != 0) {
        tsh::ExecutionEvent done_event;
        done_event.type = ok ? tsh::ExecutionEventType::ExecutionCompleted
                             : tsh::ExecutionEventType::ExecutionFailed;
        done_event.user = controller_id;
        done_event.client_ip = client_ip_str;
        done_event.worker = "local-worker";
        done_event.state = ok ? "completed" : "failed";
        done_event.duration_ms = elapsed.count();
        event_bus.publish(std::move(done_event));
      }

      std::vector<uint8_t> resp_vec(response.begin(), response.end());
      if (!sc.send_message(resp_vec, SecureChannel::MsgType::COMMAND)) {
        break;
      }
    }
  } catch (const std::exception &e) {
    tui.log_message("[Error] Client handler exception: " +
                    std::string(e.what()));
  }
  metrics.connection_closed();
  tui.log_message("[Network] Client disconnected.");
}

void Server::run() {
  const auto cfg = tsh::Config::load_from_env();
  cfg.require_server_secrets();

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  // BUG: sandbox policy was applied after listeners were already accepting.
  // FIX: apply process sandbox before socket creation, bind, listen, or accept.
  tsh::AdvancedSandbox::apply_policy();

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "[Critical] Failed to create socket: " << std::strerror(errno)
              << "\n";
    return;
  }
  g_server_fd.store(server_fd);

  int opt = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    std::cerr << "[Critical] Failed to set SO_REUSEADDR: "
              << std::strerror(errno) << "\n";
    close(server_fd);
    return;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  // BUG: shell port defaulted to INADDR_ANY and exposed itself on all NICs.
  // FIX: bind localhost by default; TSH_BIND_ADDR=0.0.0.0 is explicit exposure.
  if (inet_pton(AF_INET, cfg.server_bind_addr.c_str(), &address.sin_addr) !=
      1) {
    std::cerr << "[Critical] Invalid TSH_BIND_ADDR: " << cfg.server_bind_addr
              << "\n";
    close(server_fd);
    return;
  }
  address.sin_port = htons(cfg.server_port);

  if (bind(server_fd, reinterpret_cast<struct sockaddr *>(&address),
           sizeof(address)) < 0) {
    std::cerr << "[Critical] Bind failed on port " << cfg.server_port << ": "
              << std::strerror(errno) << "\n";
    close(server_fd);
    return;
  }

  if (listen(server_fd, 64) < 0) {
    std::cerr << "[Critical] Failed to listen on socket: "
              << std::strerror(errno) << "\n";
    close(server_fd);
    return;
  }
  // FIX[4.7]: Nonblocking accept lets SIGTERM/SIGINT shutdown break the accept
  // loop promptly.
  const int flags = fcntl(server_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
  }

  tsh::CapabilityManager cap_manager;
  tsh::StructuredAuditLogger audit_logger(cfg.audit_log_path);
  tsh::ZkAuditTrail zk_ledger(cfg.zk_ledger_path);
  tsh::PipelineValidator ast_validator;
  tsh::JobScheduler scheduler;
  tsh::Metrics metrics;
  tsh::ExecutionEventBus event_bus;
  tsh::SpineEventBridge spine_bridge(
      &event_bus, &scheduler, &metrics,
      tsh::Config::read_string("TSH_SPINE_CONTROL_ADDR", ""));

  tsh::ThreadPool pool(std::max(2u, std::thread::hardware_concurrency()));
  const auto agent_meta = tsh::AgentIdentity::load_metadata();
  const auto workers = parse_worker_nodes_from_env();
  for (const auto &worker : workers) {
    scheduler.register_node(worker.host, worker.port);
  }

  scheduler.start_health_check();

  std::atomic<int> global_risk{0};
  std::atomic<bool> global_drift{false};
  // FIX (Bug 5): IntentDrift and its mutex were function-static locals inside
  // handle_client(), which is called concurrently from the thread pool.
  // Static-local initialisation is thread-safe in C++11, but the objects
  // themselves were shared across all client threads with only a per-call
  // lock_guard protecting detect_drift() — not the construction of the statics
  // themselves during the first concurrent calls.  More importantly, declaring
  // shared mutable state as a hidden static inside a thread-pool worker is a
  // maintenance hazard: the lifetime and ownership are invisible to callers.
  // Fixed by declaring them here at Server::run() scope (same lifetime as
  // global_risk / global_drift) and passing them as references into
  // handle_client, making the sharing explicit and the ownership clear.
  tsh::IntentDrift drift_detector;
  std::mutex drift_mutex;
  auto api = std::make_shared<tsh::HttpApi>(
      cfg.http_port, cfg.api_bind_addr, cfg.api_token, scheduler, global_risk,
      global_drift, metrics, [&pool]() { return pool.queue_depth(); },
      &event_bus, workers, &spine_bridge);

  cap_manager.grant("cap_fs_read");
  cap_manager.grant("cap_exec_shell");
  cap_manager.grant("cap_net_read");
  cap_manager.grant("cap_exec_wasm");
  cap_manager.grant("cap_state_restore");
  api->start();

  audit_logger.log_event("system", "server_start", "startup sequence complete",
                         "ok");

  auto tui = std::make_shared<tsh::TuiCore>(false);
  tui->log_message("[System] Server startup sequence complete.");
  std::cout << "[TinyShell] Agent ID: " << agent_meta.agent_id << "\n"
            << "[TinyShell] Pairing code: " << agent_meta.pairing_code << "\n";
  std::cout << "[TinyShell] Ready. TCP listening on " << cfg.server_bind_addr
            << ":" << cfg.server_port << ", HTTP API on " << cfg.api_bind_addr
            << ":" << cfg.http_port << "\n";
  if (workers.empty()) {
    std::cout << "[TinyShell] No TSH_WORKERS configured; using local-worker "
                 "fallback.\n";
  } else {
    for (const auto &worker : workers) {
      std::cout << "[TinyShell] Registered worker "
                << tsh::worker_node_id(worker) << " at " << worker.host << ':'
                << worker.port << "\n";
    }
  }

  while (g_running.load()) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        accept(server_fd, reinterpret_cast<struct sockaddr *>(&client_addr),
               &client_len);
    if (client_fd < 0) {
      if (!g_running.load())
        break;
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      std::cerr << "[Error] Failed to accept connection: "
                << std::strerror(errno) << "\n";
      continue;
    }

    char client_ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    timeval tv{};
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    // BUG: idle shell clients could hold worker threads forever.
    // FIX: accepted shell sockets now have read and write timeouts.
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    tui->log_message("[Network] New client connection from " +
                     std::string(client_ip));
    // FIX[SCALE-1]: Bounded worker pool prevents one slow client from blocking
    // all others.
    // BUG FIX: Pass event_bus into handle_client so it can publish events.
    // Previously event_bus was captured in this lambda scope but never
    // forwarded as a parameter — handle_client had no reference to it and
    // published nothing.
    if (!pool.submit([client_fd, tui, &cap_manager, &ast_validator,
                      &audit_logger, &zk_ledger, &global_risk, &global_drift,
                      &drift_detector, &drift_mutex, &metrics,
                      &event_bus]() { // BUG FIX: &event_bus added
          Server::handle_client(client_fd, *tui, cap_manager, ast_validator,
                                audit_logger, zk_ledger, global_risk,
                                global_drift, drift_detector, drift_mutex,
                                metrics, event_bus); // BUG FIX: event_bus added
        })) {
      close(client_fd);
      tui->log_message(
          "[Runtime] Rejected client because worker queue is saturated.");
    }
  }
  pool.shutdown();
  api->stop();
  spine_bridge.stop();
  if (g_server_fd.exchange(-1) >= 0)
    close(server_fd);
  tui->log_message("[System] Server shutdown complete.");
}