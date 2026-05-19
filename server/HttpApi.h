#pragma once

#include "../common/Parser.h"
#include "../config/tsh_config.h"
#include "ExecutionEngine.h"
#include "ExecutionEventBus.h"
#include "JobScheduler.h"
#include "Metrics.h"
#include "MultiTenantManager.h"
#include "Pipeline.h"
#include "RbacManager.h"
#include "SpineEventBridge.h"
#include "ThreadPool.h"
#include "WorkerTypes.h"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tsh {

class HttpApi {
public:
  HttpApi(int p, std::string bind_addr, std::string token, JobScheduler &sched,
          std::atomic<int> &risk, std::atomic<bool> &drift, Metrics &metrics,
          std::function<std::size_t()> queue_depth_fn,
          ExecutionEventBus *event_bus = nullptr,
          std::vector<RemoteNode> workers = {},
          SpineEventBridge *bridge = nullptr)
      : port_(p), bind_addr_(std::move(bind_addr)),
        expected_token_(std::move(token)), scheduler_(sched),
        current_risk_(risk), intent_drift_(drift), metrics_(metrics),
        queue_depth_fn_(std::move(queue_depth_fn)),
        event_bus_(event_bus ? event_bus : &owned_event_bus_),
        execution_engine_(*event_bus_, current_risk_, intent_drift_,
                          &scheduler_),
        spine_bridge_(bridge), workers_(std::move(workers)),
        started_at_(std::chrono::steady_clock::now()) {

    if (expected_token_.empty()) {
      throw std::runtime_error("HttpApi: TSH_API_TOKEN is required");
    }
    execution_engine_.set_workers(workers_);
    admin_token_ = tsh::Config::read_string("TSH_ADMIN_TOKEN", "");
    viewer_token_ = tsh::Config::read_string("TSH_VIEWER_TOKEN", "");
  }

  void start() {
    if (expected_token_.empty()) {
      // FIX[CRIT-1]: Never start an exec-capable API without bearer auth
      // configured.
      throw std::runtime_error("HttpApi: TSH_API_TOKEN is required");
    }

    const std::string cert_path = env_value("TSH_TLS_CERT").empty()
                                      ? "certs/server.crt"
                                      : env_value("TSH_TLS_CERT");
    const std::string key_path = env_value("TSH_TLS_KEY").empty()
                                     ? "certs/server.key"
                                     : env_value("TSH_TLS_KEY");
    if (!std::filesystem::exists(cert_path) ||
        !std::filesystem::exists(key_path)) {
      // BUG: bearer tokens crossed the API socket in plaintext HTTP.
      // FIX: startup fails closed unless TLS cert and key are present.
      throw std::runtime_error("HttpApi: TLS certificate/key missing. Set "
                               "TSH_TLS_CERT and TSH_TLS_KEY or generate "
                               "certs/server.crt and certs/server.key.");
    }

    SSL_library_init();
    SSL_load_error_strings();
    tls_ctx_ = SSL_CTX_new(TLS_server_method());
    if (!tls_ctx_) {
      throw std::runtime_error("HttpApi: failed to initialize TLS context");
    }
    if (SSL_CTX_use_certificate_file(tls_ctx_, cert_path.c_str(),
                                     SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(tls_ctx_, key_path.c_str(),
                                    SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(tls_ctx_) != 1) {
      SSL_CTX_free(tls_ctx_);
      tls_ctx_ = nullptr;
      throw std::runtime_error("HttpApi: TLS certificate/key could not be "
                               "loaded or do not match");
    }

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
      throw std::runtime_error(std::string("HttpApi: socket() failed: ") +
                               std::strerror(errno));
    }
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
        0) {
      throw std::runtime_error(std::string("HttpApi: setsockopt() failed: ") +
                               std::strerror(errno));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    // FIX[CRIT-1]: Bind localhost by default; external exposure requires
    // explicit TSH_API_BIND_ADDR.
    if (inet_pton(AF_INET, bind_addr_.c_str(), &address.sin_addr) != 1) {
      throw std::runtime_error("HttpApi: invalid bind address " + bind_addr_);
    }
    address.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr *>(&address),
             sizeof(address)) < 0) {
      // FIX[REL-1]: Binding failures must be loud instead of silently disabling
      // the API.
      throw std::runtime_error(std::string("HttpApi: bind() failed on port ") +
                               std::to_string(port_) + ": " +
                               std::strerror(errno));
    }
    if (listen(server_fd_, 10) < 0) {
      throw std::runtime_error(std::string("HttpApi: listen() failed: ") +
                               std::strerror(errno));
    }
    const int flags = fcntl(server_fd_, F_GETFL, 0);
    if (flags >= 0) {
      // FIX[4.7]: Nonblocking API accept lets stop() join promptly during
      // shutdown.
      fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    running_.store(true);
    request_pool_ = std::make_unique<ThreadPool>(configured_http_workers());
    api_thread_ = std::thread([this]() { serve_loop(); });
    // FIX[P-4]: Start background worker-health probe thread.
    start_probe_thread();
    std::cout << "[API] HTTPS API active on " << bind_addr_ << ":" << port_
              << "\n";
  }

  void stop() {
    // FIX[C-1]: stop() now joins every in-flight request thread before
    // returning.  Previously stop() only joined api_thread_ (the accept loop)
    // but left detached request threads running.  Those threads held a raw
    // `this` pointer and would dereference destroyed members after the HttpApi
    // object was torn down, causing undefined behaviour.
    running_.store(false);
    if (server_fd_ >= 0) {
      close(server_fd_);
      server_fd_ = -1;
    }
    if (probe_thread_.joinable()) {
      probe_thread_.join();
    }
    if (api_thread_.joinable()) {
      api_thread_.join();
    }
    if (request_pool_) {
      request_pool_->shutdown();
      request_pool_.reset();
    }
    if (tls_ctx_) {
      SSL_CTX_free(tls_ctx_);
      tls_ctx_ = nullptr;
    }
  }

  ~HttpApi() { stop(); }

private:
  static std::string url_decode(std::string_view in) {
    // FIX[M2]: Decode %XX and + before allowlist checks to prevent encoded
    // bypasses.
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '%' && i + 2 < in.size() &&
          std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
          std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
        out += static_cast<char>(
            std::stoi(std::string{in[i + 1], in[i + 2]}, nullptr, 16));
        i += 2;
      } else if (in[i] == '+') {
        out += ' ';
      } else {
        out += in[i];
      }
    }
    return out;
  }

  static std::string header_value(const std::string &request,
                                  const std::string &name) {
    const auto pos = request.find(name + ": ");
    if (pos == std::string::npos)
      return "";
    const auto start = pos + name.size() + 2;
    const auto end = request.find("\r\n", start);
    return request.substr(start, end == std::string::npos ? std::string::npos
                                                          : end - start);
  }

  // FIX[S-6]: std::string::operator== short-circuits on the first differing
  // byte, leaking the token value through response-time differences (~10K
  // requests at microsecond precision are enough to recover a short secret).
  // This constant-time helper always touches every byte of both strings.
  static bool constant_time_eq(const std::string &a, const std::string &b) {
    // Length mismatch still leaks the length, but that is unavoidable with a
    // fixed-format "Bearer <token>" header.  What matters is that two strings
    // of equal length are compared without early-out.
    if (a.size() != b.size()) {
      // Perform a dummy loop so the compiler cannot trivially optimize away
      // the branch and produce a timing difference proportional to length.
      unsigned char dummy = 0;
      for (std::size_t i = 0; i < a.size(); ++i)
        dummy |= static_cast<unsigned char>(a[i]);
      (void)dummy;
      return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
      diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
  }

  static std::size_t configured_http_workers() {
    if (const char *value = std::getenv("TSH_HTTP_WORKERS")) {
      try {
        return std::clamp<std::size_t>(std::stoull(value), 4, 256);
      } catch (...) {
      }
    }
    const auto hw = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hw == 0 ? 32 : hw * 4, 16, 128);
  }

  struct AuthIdentity {
    bool ok = false;
    bool forbidden = false;
    std::string token;
    std::string role;
    std::string user;
    std::string tenant;
  };

  static std::string bearer_token(const std::string &request) {
    const auto auth = header_value(request, "Authorization");
    constexpr std::string_view prefix = "Bearer ";
    if (auth.rfind(std::string(prefix), 0) != 0) {
      return "";
    }
    return auth.substr(prefix.size());
  }

  static std::string env_value(const char *name) {
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
  }

  AuthIdentity identity_for_token(const std::string &token) const {
    // BUG: request headers controlled user and tenant identity in audit logs.
    // FIX: identity is derived only from the authenticated bearer token.
    AuthIdentity identity;
    identity.token = token;
    if (!expected_token_.empty() && constant_time_eq(token, expected_token_)) {
      identity.ok = true;
      identity.role = "operator";
      identity.user = "service";
      identity.tenant = "default";
      return identity;
    }
    const auto &admin_token = admin_token_;
    if (!admin_token.empty() && constant_time_eq(token, admin_token)) {
      identity.ok = true;
      identity.role = "admin";
      identity.user = "admin";
      identity.tenant = "system";
      return identity;
    }
    const auto &viewer_token = viewer_token_;
    if (!viewer_token.empty() && constant_time_eq(token, viewer_token)) {
      identity.ok = true;
      identity.role = "viewer";
      identity.user = "viewer";
      identity.tenant = "default";
      return identity;
    }
    return identity;
  }

  static std::string permission_name(Permission perm) {
    switch (perm) {
    case Permission::EXECUTE_COMMAND:
      return "EXECUTE_COMMAND";
    case Permission::EXECUTE_PIPELINE:
      return "EXECUTE_PIPELINE";
    case Permission::READ_PROCESS_INFO:
      return "READ_PROCESS_INFO";
    case Permission::READ_SYSTEM_METRICS:
      return "READ_SYSTEM_METRICS";
    case Permission::MANAGE_JOBS:
      return "MANAGE_JOBS";
    case Permission::MANAGE_RESOURCES:
      return "MANAGE_RESOURCES";
    case Permission::READ_AUDIT_LOG:
      return "READ_AUDIT_LOG";
    case Permission::MANAGE_CLUSTER:
      return "MANAGE_CLUSTER";
    case Permission::READ_CLUSTER_STATUS:
      return "READ_CLUSTER_STATUS";
    case Permission::MANAGE_TENANTS:
      return "MANAGE_TENANTS";
    case Permission::ADMIN:
      return "ADMIN";
    }
    return "UNKNOWN";
  }

  static std::string token_hash(const std::string &token) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(token.data(), token.size(), digest, &len, EVP_sha256(), nullptr);
    std::ostringstream out;
    for (unsigned int i = 0; i < len; ++i) {
      out << std::hex << std::setw(2) << std::setfill('0')
          << static_cast<int>(digest[i]);
    }
    return out.str();
  }

  void publish_security_event(const AuthIdentity &identity,
                              const std::string &client_ip,
                              const std::string &type,
                              const std::string &action,
                              const std::string &resource,
                              const std::string &detail) {
    // BUG: security violations were not connected to the security feed.
    // FIX: every denied/blocked action is published as a SecurityViolation.
    ExecutionEvent event;
    event.type = ExecutionEventType::SecurityViolation;
    event.request_id = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    event.user = identity.user.empty() ? "unauthenticated" : identity.user;
    event.tenant = identity.tenant.empty() ? "unknown" : identity.tenant;
    event.client_ip = client_ip;
    event.command = resource;
    event.state = type;
    event.detail = action + ": " + detail;
    event.result = "denied";
    event_bus_->publish(std::move(event));
  }

  AuthIdentity authorize(const std::string &request,
                         const std::string &client_ip, Permission perm,
                         const std::string &resource) {
    const auto token = bearer_token(request);
    auto identity = identity_for_token(token);
    if (!identity.ok) {
      metrics_.increment_auth_failure(client_ip);
      publish_security_event(identity, client_ip, "INVALID_TOKEN",
                             permission_name(perm), resource,
                             "Bearer token rejected");
      return identity;
    }

    if (!rbac_manager_.verify_permission(identity.role, perm)) {
      identity.forbidden = true;
      publish_security_event(identity, client_ip, "AUTH_DENIED",
                             permission_name(perm), resource,
                             "role " + identity.role + " lacks permission");
      return identity;
    }
    ExecutionEvent event;
    event.type = ExecutionEventType::Authenticated;
    event.request_id = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    event.user = identity.user;
    event.tenant = identity.tenant;
    event.client_ip = client_ip;
    event.state = "authenticated";
    event.detail =
        "role=" + identity.role + " token_sha256=" + token_hash(identity.token);
    event.result = "ok";
    if (perm == Permission::EXECUTE_COMMAND || perm == Permission::MANAGE_JOBS)
      event_bus_->publish(std::move(event));
    return identity;
  }

  static std::string json_escape(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char c : value) {
      if (c == '"')
        escaped += "\\\"";
      else if (c == '\\')
        escaped += "\\\\";
      else if (c == '\n')
        escaped += "\\n";
      else if (c == '\r')
        escaped += "\\r";
      else if (c == '\t')
        escaped += "\\t";
      else if (c < 0x20) {
        std::ostringstream hex;
        hex << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(c);
        escaped += hex.str();
      } else {
        escaped += static_cast<char>(c);
      }
    }
    return escaped;
  }

  static std::string body_of(const std::string &request) {
    const auto pos = request.find("\r\n\r\n");
    if (pos == std::string::npos)
      return "";
    return request.substr(pos + 4);
  }

  static std::string form_value(const std::string &body,
                                const std::string &key) {
    std::string needle = key + "=";
    size_t start = 0;
    while (start <= body.size()) {
      const auto end = body.find('&', start);
      const auto part = body.substr(
          start, end == std::string::npos ? std::string::npos : end - start);
      if (part.rfind(needle, 0) == 0) {
        return url_decode(part.substr(needle.size()));
      }
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
    return "";
  }

  static std::string query_value(const std::string &request,
                                 const std::string &key) {
    const auto line_end = request.find("\r\n");
    const auto first_line = request.substr(0, line_end);
    const auto q = first_line.find('?');
    if (q == std::string::npos)
      return "";
    const auto space = first_line.find(' ', q);
    const auto query = first_line.substr(
        q + 1, space == std::string::npos ? std::string::npos : space - q - 1);
    return form_value(query, key);
  }

  std::string extract_command(const std::string &request) const {
    const size_t q_start = request.find("command=");
    if (q_start != std::string::npos &&
        request.substr(0, request.find("\r\n")).find('?') !=
            std::string::npos) {
      std::string raw = request.substr(q_start + 8);
      const size_t q_end = raw.find_first_of(" &\r\n");
      if (q_end != std::string::npos)
        raw = raw.substr(0, q_end);
      return url_decode(raw);
    }
    return form_value(body_of(request), "command");
  }

  static bool command_allowed(const std::string &command) {
    std::istringstream in(command);
    std::string root;
    in >> root;
    std::vector<std::string> args;
    for (std::string arg; in >> arg;) {
      args.push_back(arg);
    }
    // Normalize: strip common path prefixes so "df", "/bin/df", "/usr/bin/df"
    // all resolve to the same allowlist entry.
    const auto slash = root.rfind('/');
    const std::string name =
        (slash != std::string::npos) ? root.substr(slash + 1) : root;
    // BUG: allowlist checked only argv[0], so dangerous arguments passed.
    // FIX: every permitted command has explicit argument-count/flag rules.
    if (name == "uptime" || name == "who") {
      return args.empty();
    }
    if (name == "df") {
      if (args.size() > 1)
        return false;
      if (args.empty())
        return true;
      return args[0] == "-h" || args[0] == "--human-readable";
    }
    if (name == "ps") {
      if (args.size() > 2)
        return false;
      for (const auto &arg : args) {
        if (arg != "-ef" && arg != "aux")
          return false;
      }
      return true;
    }
    return false;
  }

  void send_http_response(
      SSL *ssl, int client_fd, int status, const std::string &body,
      const std::string &content_type = "application/json") const {
    std::string reason = "OK";
    if (status == 401)
      reason = "Unauthorized";
    if (status == 400)
      reason = "Bad Request";
    if (status == 403)
      reason = "Forbidden";
    if (status == 404)
      reason = "Not Found";
    if (status == 413)
      reason = "Request Entity Too Large";
    if (status == 429)
      reason = "Too Many Requests";
    if (status == 503)
      reason = "Service Unavailable";
    if (status == 500)
      reason = "Internal Server Error";
    std::ostringstream response;
    response << "HTTP/1.1 " << status << " " << reason << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n";
    if (status == 401)
      response << "WWW-Authenticate: Bearer\r\n";
    if (status == 429)
      response << "Retry-After: 60\r\n";
    response << "Connection: close\r\n";
    response << "\r\n" << body;
    const auto text = response.str();
    const char *data = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0) {
      ssize_t n = -1;
      if (ssl) {
        n = SSL_write(ssl, data, static_cast<int>(remaining));
      } else {
#if defined(MSG_NOSIGNAL)
        n = send(client_fd, data, remaining, MSG_NOSIGNAL);
#else
        n = send(client_fd, data, remaining, 0);
#endif
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      data += n;
      remaining -= static_cast<std::size_t>(n);
    }
  }

  struct WorkerTelemetry {
    double cpu_usage = 0.0;
    double ram_usage = 0.0;
    int active_jobs = 0;
    std::string running_command;
    double health_score = 100.0;
  };

  struct RateBucket {
    double tokens = 0.0;
    std::chrono::steady_clock::time_point last;
  };

  static double read_cpu_usage() {
    double loadavg = 0.0;
    if (getloadavg(&loadavg, 1) != 1) {
      return 0.0;
    }
    const long cpu_count = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    return std::min(100.0, (loadavg / static_cast<double>(cpu_count)) * 100.0);
  }

  static double read_ram_usage() {
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    long long value = 0;
    std::string unit;
    long long total_kb = 0;
    long long available_kb = 0;
    while (meminfo >> key >> value >> unit) {
      if (key == "MemTotal:")
        total_kb = value;
      else if (key == "MemAvailable:")
        available_kb = value;
    }
    if (total_kb <= 0)
      return 0.0;
    const auto used_kb = std::max(0LL, total_kb - available_kb);
    return (static_cast<double>(used_kb) / static_cast<double>(total_kb)) *
           100.0;
  }

  static WorkerTelemetry
  worker_telemetry(const std::vector<ExecutionEvent> &events,
                   const std::string &worker_id) {
    WorkerTelemetry telemetry;
    telemetry.cpu_usage = read_cpu_usage();
    telemetry.ram_usage = read_ram_usage();

    std::set<std::string> active;
    std::string latest_running_command;
    for (const auto &event : events) {
      if (event.worker != worker_id)
        continue;
      if (event.type == ExecutionEventType::ExecutionStarted) {
        active.insert(event.request_id);
        latest_running_command = event.command;
      } else if (event.type == ExecutionEventType::ExecutionCompleted ||
                 event.type == ExecutionEventType::ExecutionFailed ||
                 event.type == ExecutionEventType::SecurityViolation) {
        active.erase(event.request_id);
      }
    }
    telemetry.active_jobs = static_cast<int>(active.size());
    telemetry.running_command = active.empty() ? "" : latest_running_command;

    const double pressure = (telemetry.cpu_usage * 0.45) +
                            (telemetry.ram_usage * 0.45) +
                            (std::min(10, telemetry.active_jobs) * 1.0);
    telemetry.health_score = std::clamp(100.0 - pressure, 0.0, 100.0);
    return telemetry;
  }

  // ── Background worker-health probe ─────────────────────────────────────
  // FIX[P-4]: probe_worker() was called synchronously inside events_json()
  // which is invoked on every GET /control/events request.  With N configured
  // workers and a 200 ms select timeout each, one dashboard poll could block
  // for N×200 ms.  The GUI polls at ~1 Hz, so with 5 workers the HTTP handler
  // was spending up to 1 second in TCP probes per request.
  //
  // Fix: a dedicated background thread re-probes all workers every
  // kProbeIntervalMs and stores the results in worker_health_cache_.
  // events_json() reads the cache under a short lock instead of blocking.

  void start_probe_thread() {
    probe_thread_ = std::thread([this]() {
      for (int i = 0; i < 30 && running_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

      while (running_.load()) {
        {
          std::unordered_map<std::string, bool> fresh;
          for (const auto &w : workers_) {
            const auto id = worker_node_id(w);
            bool online = true;
            {
              std::lock_guard<std::mutex> lk(probe_cache_mutex_);
              online =
                  worker_health_cache_.find(id) == worker_health_cache_.end()
                      ? true
                      : worker_health_cache_[id];
            }
            const auto now = std::chrono::steady_clock::now();
            if (!online && reconnect_due_[id] > now) {
              fresh[id] = false;
              execution_engine_.set_worker_health(
                  id, WorkerHealthStatus::UNREACHABLE);
              continue;
            }

            const bool probed = probe_worker(w);
            if (probed) {
              // BUG: recovered workers stayed excluded after reconnect.
              // FIX: a successful probe marks the worker HEALTHY again.
              missed_probe_counts_[id] = 0;
              fresh[id] = true;
              scheduler_.mark_node_healthy(id);
              execution_engine_.set_worker_health(id,
                                                  WorkerHealthStatus::HEALTHY);
            } else {
              const int missed = ++missed_probe_counts_[id];
              if (missed >= 3) {
                // BUG: failed probes did not remove workers from routing.
                // FIX: three misses mark UNREACHABLE and remove scheduling.
                if (online) {
                  std::cerr
                      << "WORKER " << id
                      << " marked unreachable after 3 missed heartbeats\n";
                }
                fresh[id] = false;
                scheduler_.mark_node_unreachable(id);
                execution_engine_.set_worker_health(
                    id, WorkerHealthStatus::UNREACHABLE);
                reconnect_due_[id] = now + std::chrono::seconds(30);
              } else {
                fresh[id] = online;
              }
            }
          }
          std::lock_guard<std::mutex> lk(probe_cache_mutex_);
          worker_health_cache_ = std::move(fresh);
        }
        // Sleep in short increments so we notice running_==false quickly.
        for (int i = 0; i < kProbeIntervalMs / 100 && running_.load(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }

  bool cached_worker_online(const std::string &id) const {
    std::lock_guard<std::mutex> lk(probe_cache_mutex_);
    const auto it = worker_health_cache_.find(id);
    // Default to true (optimistic) before the first probe completes.
    return it == worker_health_cache_.end() ? true : it->second;
  }

  static constexpr int kProbeIntervalMs = 5000;
  static constexpr int kHttpReadTimeoutMs = 2000;
  static constexpr int kHttpWriteTimeoutMs = 2000;
  static constexpr std::size_t kMaxHttpRequestBytes = 16384;

  static bool probe_worker(const RemoteNode &worker) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return false;

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(worker.port);
    if (inet_pton(AF_INET, worker.host.c_str(), &address.sin_addr) != 1) {
      close(fd);
      return false;
    }

    const int rc =
        connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (rc == 0) {
      close(fd);
      return true;
    }
    if (errno != EINPROGRESS) {
      close(fd);
      return false;
    }

    fd_set writes;
    FD_ZERO(&writes);
    FD_SET(fd, &writes);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    const int ready = select(fd + 1, nullptr, &writes, nullptr, &timeout);
    if (ready <= 0) {
      close(fd);
      return false;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    close(fd);
    return err == 0;
  }

  int configured_rate_limit_rpm() const {
    if (const char *raw = std::getenv("TSH_RATE_LIMIT_RPM")) {
      try {
        return std::max(1, std::stoi(raw));
      } catch (...) {
      }
    }
    return 60;
  }

  bool allow_exec_request(const std::string &client_ip) {
    const int rpm = configured_rate_limit_rpm();
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(rate_mutex_);
    auto &bucket = rate_buckets_[client_ip];
    if (bucket.last.time_since_epoch().count() == 0) {
      bucket.tokens = rpm;
      bucket.last = now;
    }
    const double elapsed_seconds =
        std::chrono::duration<double>(now - bucket.last).count();
    bucket.last = now;
    bucket.tokens =
        std::min<double>(rpm, bucket.tokens + elapsed_seconds * rpm / 60.0);
    if (bucket.tokens < 1.0) {
      // BUG: authenticated clients could hammer /exec with no backpressure.
      // FIX: token bucket enforces per-IP request rate and fails closed.
      return false;
    }
    bucket.tokens -= 1.0;
    return true;
  }

  std::string events_json(const std::string &request) const {
    std::uint64_t since = 0;
    try {
      const auto raw = query_value(request, "since_sequence");
      if (!raw.empty())
        since = std::stoull(raw);
    } catch (...) {
      since = 0;
    }
    // BUG: /control/events serialized all events on every dashboard poll.
    // FIX: return only events after since_sequence, capped at 1000 entries.
    const auto events = event_bus_->snapshot_since(since, 1000);
    std::ostringstream json;
    json << "{\"events\":[";
    bool first = true;
    for (const auto &event : events) {
      if (!first)
        json << ',';
      first = false;
      json << "{\"sequence\":" << event.sequence << ",\"timestamp\":\""
           << json_escape(event.timestamp) << "\",\"type\":\""
           << ExecutionEventBus::type_name(event.type) << "\",\"request_id\":\""
           << json_escape(event.request_id) << "\",\"user\":\""
           << json_escape(event.user) << "\",\"tenant\":\""
           << json_escape(event.tenant) << "\",\"client_ip\":\""
           << json_escape(event.client_ip) << "\",\"command\":\""
           << json_escape(event.command) << "\",\"worker\":\""
           << json_escape(event.worker) << "\",\"state\":\""
           << json_escape(event.state) << "\",\"detail\":\""
           << json_escape(event.detail) << "\",\"result\":\""
           << json_escape(event.result)
           << "\",\"duration_ms\":" << event.duration_ms << "}";
    }
    const std::uint64_t next_sequence =
        events.empty() ? event_bus_->latest_sequence() : events.back().sequence;
    json << "],\"next_sequence\":" << next_sequence << ",\"workers\":[";
    bool first_worker = true;
    const auto emit_worker = [&](const std::string &id, bool online,
                                 const WorkerTelemetry &worker) {
      if (!first_worker)
        json << ',';
      first_worker = false;
      json << "{\"node_id\":\"" << json_escape(id)
           << "\",\"online\":" << (online ? "true" : "false")
           << ",\"cpu_usage\":" << worker.cpu_usage
           << ",\"ram_usage\":" << worker.ram_usage
           << ",\"active_jobs\":" << worker.active_jobs
           << ",\"last_heartbeat\":\""
           << json_escape(ExecutionEventBus::now_iso8601())
           << "\",\"running_command\":\"" << json_escape(worker.running_command)
           << "\",\"health_score\":" << (online ? worker.health_score : 0.0)
           << "}";
    };
    if (workers_.empty()) {
      emit_worker("local-worker", true,
                  worker_telemetry(events, "local-worker"));
    } else {
      for (const auto &configured_worker : workers_) {
        const auto id = worker_node_id(configured_worker);
        // FIX[P-4]: was probe_worker(configured_worker) — blocking TCP connect
        // with 200ms timeout per worker, executed synchronously on the HTTP
        // handler thread.  Now reads from the background-refreshed cache.
        emit_worker(id, cached_worker_online(id), worker_telemetry(events, id));
      }
    }
    json << "],"
         << "\"clients_connected\":" << metrics_.active_connections()
         << ",\"queue_depth\":" << queue_depth_fn_()
         << ",\"event_bus_size_current\":" << event_bus_->size()
         << ",\"event_bus_evictions_total\":" << event_bus_->evictions() << "}";
    return json.str();
  }

  std::string workers_json() const {
    const auto events = event_bus_->snapshot();
    std::ostringstream json;
    json << "{\"workers\":[";
    bool first_worker = true;
    const auto emit_worker = [&](const std::string &id, bool online,
                                 const WorkerTelemetry &worker) {
      if (!first_worker)
        json << ',';
      first_worker = false;
      json << "{\"node_id\":\"" << json_escape(id)
           << "\",\"online\":" << (online ? "true" : "false")
           << ",\"cpu_usage\":" << worker.cpu_usage
           << ",\"ram_usage\":" << worker.ram_usage
           << ",\"active_jobs\":" << worker.active_jobs
           << ",\"last_heartbeat\":\""
           << json_escape(ExecutionEventBus::now_iso8601())
           << "\",\"running_command\":\"" << json_escape(worker.running_command)
           << "\",\"health_score\":" << (online ? worker.health_score : 0.0)
           << "}";
    };

    if (workers_.empty()) {
      emit_worker("local-worker", true,
                  worker_telemetry(events, "local-worker"));
    } else {
      for (const auto &configured_worker : workers_) {
        const auto id = worker_node_id(configured_worker);
        emit_worker(id, cached_worker_online(id), worker_telemetry(events, id));
      }
    }
    json << "]}";
    return json.str();
  }

  std::string tenants_json() const {
    std::ostringstream json;
    json << "{\"tenants\":[";
    bool first = true;
    for (const auto &tenant : multi_tenant_manager_.list_tenants()) {
      if (!first)
        json << ',';
      first = false;
      json << "{\"id\":\"" << json_escape(tenant.tenant_id)
           << "\",\"name\":\"" << json_escape(tenant.tenant_name)
           << "\",\"owner\":\"" << json_escape(tenant.owner)
           << "\",\"active\":" << (tenant.is_active ? "true" : "false")
           << ",\"quota_jobs\":" << tenant.resource_quota_jobs
           << ",\"quota_memory_mb\":" << tenant.resource_quota_memory_mb
           << ",\"quota_cpu_percent\":" << tenant.resource_quota_cpu_percent
           << "}";
    }
    json << "],\"users\":[";
    first = true;
    for (const auto &user : multi_tenant_manager_.list_users()) {
      if (!first)
        json << ',';
      first = false;
      json << "{\"id\":\"" << json_escape(user.user_id)
           << "\",\"username\":\"" << json_escape(user.username)
           << "\",\"tenant_id\":\"" << json_escape(user.tenant_id)
           << "\",\"role\":\"" << json_escape(user.role)
           << "\",\"active\":" << (user.is_active ? "true" : "false")
           << "}";
    }
    json << "]}";
    return json.str();
  }

  std::string security_events_json() const {
    // BUG: security page read from a feed that did not include real denies.
    // FIX: expose SecurityViolation events from the authoritative event bus.
    const auto events = event_bus_->snapshot_since(0, 1000);
    std::ostringstream json;
    json << "{\"events\":[";
    bool first = true;
    for (const auto &event : events) {
      if (event.type != ExecutionEventType::SecurityViolation)
        continue;
      if (!first)
        json << ',';
      first = false;
      json << "{\"sequence\":" << event.sequence << ",\"timestamp\":\""
           << json_escape(event.timestamp) << "\",\"type\":\""
           << json_escape(event.state) << "\",\"user\":\""
           << json_escape(event.user) << "\",\"tenant\":\""
           << json_escape(event.tenant) << "\",\"ip\":\""
           << json_escape(event.client_ip) << "\",\"resource\":\""
           << json_escape(event.command) << "\",\"detail\":\""
           << json_escape(event.detail) << "\"}";
    }
    json << "]}";
    return json.str();
  }

  struct TraceRecord {
    std::string job_id;
    std::string command;
    std::string user;
    std::string tenant;
    std::string worker;
    std::string start_time;
    std::string end_time;
    long long duration_ms = 0;
    int exit_code = 0;
    std::vector<ExecutionEvent> events;
  };

  void record_trace(const ExecutionRequest &request,
                    const ExecutionResult &result) {
    // BUG: executions emitted events but no trace objects reached the GUI.
    // FIX: persist the latest 1000 request traces with their sequenced events.
    TraceRecord trace;
    trace.job_id = request.request_id;
    trace.command = request.command;
    trace.user = request.user;
    trace.tenant = request.tenant;
    trace.worker = result.worker;
    trace.start_time = ExecutionEventBus::now_iso8601();
    trace.end_time = trace.start_time;
    trace.duration_ms = result.duration_ms;
    trace.exit_code = result.ok ? 0 : -1;
    for (const auto &event : event_bus_->snapshot()) {
      if (event.request_id == request.request_id) {
        trace.events.push_back(event);
      }
    }
    std::lock_guard<std::mutex> lock(traces_mutex_);
    traces_.push_front(std::move(trace));
    while (traces_.size() > 1000) {
      traces_.pop_back();
    }
  }

  std::string traces_json() const {
    std::lock_guard<std::mutex> lock(traces_mutex_);
    std::ostringstream json;
    json << "[";
    bool first_trace = true;
    for (const auto &trace : traces_) {
      if (!first_trace)
        json << ',';
      first_trace = false;
      json << "{\"job_id\":\"" << json_escape(trace.job_id)
           << "\",\"command\":\"" << json_escape(trace.command)
           << "\",\"user\":\"" << json_escape(trace.user) << "\",\"tenant\":\""
           << json_escape(trace.tenant) << "\",\"worker\":\""
           << json_escape(trace.worker) << "\",\"start_time\":\""
           << json_escape(trace.start_time) << "\",\"end_time\":\""
           << json_escape(trace.end_time)
           << "\",\"duration_ms\":" << trace.duration_ms
           << ",\"exit_code\":" << trace.exit_code << ",\"events\":[";
      bool first_event = true;
      for (const auto &event : trace.events) {
        if (!first_event)
          json << ',';
        first_event = false;
        json << "{\"sequence\":" << event.sequence << ",\"type\":\""
             << ExecutionEventBus::type_name(event.type) << "\",\"state\":\""
             << json_escape(event.state) << "\",\"detail\":\""
             << json_escape(event.detail) << "\"}";
      }
      json << "]}";
    }
    json << "]";
    return json.str();
  }

  static void set_socket_timeout(int fd, int opt, int timeout_ms) {
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof(tv));
  }

  static std::size_t content_length_of(const std::string &request) {
    const auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      return 0;
    }
    const auto pos = request.find("Content-Length:");
    if (pos == std::string::npos || pos > header_end) {
      return 0;
    }
    const auto start = request.find_first_not_of(" \t", pos + 15);
    if (start == std::string::npos || start > header_end) {
      return 0;
    }
    const auto end = request.find("\r\n", start);
    try {
      return std::stoull(request.substr(start, end - start));
    } catch (...) {
      return 0;
    }
  }

  bool read_http_request(SSL *ssl, int client_fd, std::string *request) const {
    request->clear();
    request->reserve(4096);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kHttpReadTimeoutMs);
    while (running_.load() && std::chrono::steady_clock::now() < deadline) {
      fd_set reads;
      FD_ZERO(&reads);
      FD_SET(client_fd, &reads);
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      timeval tv{};
      tv.tv_sec = static_cast<long>(remaining.count() / 1000);
      tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
      const int ready = select(client_fd + 1, &reads, nullptr, nullptr, &tv);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        return false;
      }

      char buffer[2048];
      const ssize_t n = ssl ? SSL_read(ssl, buffer, sizeof(buffer))
                            : recv(client_fd, buffer, sizeof(buffer), 0);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        return false;
      }
      request->append(buffer, static_cast<std::size_t>(n));
      if (request->size() >= kMaxHttpRequestBytes) {
        return true;
      }
      const auto header_end = request->find("\r\n\r\n");
      if (header_end != std::string::npos) {
        const auto body_needed = content_length_of(*request);
        if (request->size() >= header_end + 4 + body_needed) {
          return true;
        }
      }
    }
    return false;
  }

  void handle_request(int client_fd, std::string client_ip) {
    struct ConnectionGuard {
      Metrics &metrics;
      int fd;
      SSL *ssl = nullptr;
      ConnectionGuard(Metrics &m, int f) : metrics(m), fd(f) {
        metrics.connection_opened();
      }
      ~ConnectionGuard() {
        if (ssl) {
          SSL_shutdown(ssl);
          SSL_free(ssl);
        }
        if (fd >= 0) {
          close(fd);
        }
        metrics.connection_closed();
      }
    } guard(metrics_, client_fd);

    set_socket_timeout(client_fd, SO_RCVTIMEO, kHttpReadTimeoutMs);
    set_socket_timeout(client_fd, SO_SNDTIMEO, kHttpWriteTimeoutMs);
    guard.ssl = SSL_new(tls_ctx_);
    if (!guard.ssl) {
      return;
    }
    SSL_set_fd(guard.ssl, client_fd);
    if (SSL_accept(guard.ssl) != 1) {
      // BUG: plaintext requests could carry bearer tokens over the API port.
      // FIX: non-TLS clients are closed before any HTTP parsing occurs.
      return;
    }

    std::string request;
    if (!read_http_request(guard.ssl, client_fd, &request)) {
      return;
    }
    if (request.size() >= kMaxHttpRequestBytes) {
      send_http_response(guard.ssl, client_fd, 413,
                         "{\"error\":\"request_too_large\"}");
      return;
    }

    if (request.rfind("GET /healthz", 0) == 0 ||
        request.rfind("GET /health ", 0) == 0) {
      const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - started_at_)
                              .count();
      const bool degraded = queue_depth_fn_() > 1000;
      std::ostringstream body;
      body << "{\"status\":\"" << (degraded ? "degraded" : "ok")
           << "\",\"uptime_s\":" << uptime
           << ",\"clients_connected\":" << metrics_.active_connections() << "}";
      send_http_response(guard.ssl, client_fd, degraded ? 503 : 200,
                         body.str());
    } else if (request.rfind("GET /metrics", 0) == 0) {
      auto auth = authorize(request, client_ip, Permission::READ_SYSTEM_METRICS,
                            "/metrics");
      if (!auth.ok) {
        send_http_response(guard.ssl, client_fd, 401,
                           "{\"error\":\"unauthorized\"}");
      } else if (auth.forbidden) {
        send_http_response(guard.ssl, client_fd, 403,
                           "{\"error\":\"forbidden\"}");
      } else {
        send_http_response(guard.ssl, client_fd, 200,
                           metrics_.prometheus(queue_depth_fn_()),
                           "text/plain");
      }
    } else if (request.rfind("GET /control/events", 0) == 0) {
      auto auth = authorize(request, client_ip, Permission::READ_CLUSTER_STATUS,
                            "/control/events");
      if (!auth.ok) {
        send_http_response(guard.ssl, client_fd, 401,
                           "{\"error\":\"unauthorized\"}");
      } else if (auth.forbidden) {
        send_http_response(guard.ssl, client_fd, 403,
                           "{\"error\":\"forbidden\"}");
      } else {
        send_http_response(guard.ssl, client_fd, 200, events_json(request));
      }
    } else if (request.rfind("GET /control/workers", 0) == 0) {
      auto auth = authorize(request, client_ip, Permission::READ_CLUSTER_STATUS,
                            "/control/workers");
      if (!auth.ok) {
        send_http_response(guard.ssl, client_fd, 401,
                           "{\"error\":\"unauthorized\"}");
      } else if (auth.forbidden) {
        send_http_response(guard.ssl, client_fd, 403,
                           "{\"error\":\"forbidden\"}");
      } else {
        send_http_response(guard.ssl, client_fd, 200, workers_json());
      }
    } else if (request.rfind("GET /control/tenants", 0) == 0) {
      auto auth = authorize(request, client_ip, Permission::READ_CLUSTER_STATUS,
                            "/control/tenants");
      if (!auth.ok) {
        send_http_response(guard.ssl, client_fd, 401,
                           "{\"error\":\"unauthorized\"}");
      } else if (auth.forbidden) {
        send_http_response(guard.ssl, client_fd, 403,
                           "{\"error\":\"forbidden\"}");
      } else {
        send_http_response(guard.ssl, client_fd, 200, tenants_json());
      }
    } else if (request.rfind("GET /control/security", 0) == 0) {
      auto auth = authorize(request, client_ip, Permission::READ_AUDIT_LOG,
                            "/control/security");
      if (!auth.ok) {
        send_http_response(guard.ssl, client_fd, 401,
                           "{\"error\":\"unauthorized\"}");
      } else if (auth.forbidden) {
        send_http_response(guard.ssl, client_fd, 403,
                           "{\"error\":\"forbidden\"}");
      } else if (!allow_exec_request(client_ip)) {
        send_http_response(guard.ssl, client_fd, 429,
                           "{\"error\":\"rate_limited\"}");
      } else {
        send_http_response(guard.ssl, client_fd, 200, security_events_json());
      }
    } else if (request.rfind("GET /control/traces", 0) == 0) {
      auto auth = authorize(request, client_ip, Permission::READ_CLUSTER_STATUS,
                            "/control/traces");
      if (!auth.ok) {
        send_http_response(guard.ssl, client_fd, 401,
                           "{\"error\":\"unauthorized\"}");
      } else if (auth.forbidden) {
        send_http_response(guard.ssl, client_fd, 403,
                           "{\"error\":\"forbidden\"}");
      } else {
        send_http_response(guard.ssl, client_fd, 200, traces_json());
      }
    } else if (request.rfind("GET /exec", 0) == 0 ||
               request.rfind("POST /exec", 0) == 0) {
      auto auth =
          authorize(request, client_ip, Permission::EXECUTE_COMMAND, "/exec");
      if (!auth.ok) {
        send_http_response(guard.ssl, client_fd, 401,
                           "{\"error\":\"unauthorized\"}");
      } else if (auth.forbidden) {
        send_http_response(guard.ssl, client_fd, 403,
                           "{\"error\":\"forbidden\"}");
      } else if (!allow_exec_request(client_ip)) {
        send_http_response(guard.ssl, client_fd, 429,
                           "{\"error\":\"rate_limited\"}");
      } else {
        std::string cmd = extract_command(request);
        if (cmd.empty()) {
          // BUG-11 FIX: A missing command= parameter should be 400 Bad Request,
          // not 403 Forbidden.  The old code let an empty string reach
          // command_allowed() which returned false (not in allowed set) → 403,
          // and the spurious failure was counted in metrics.
          send_http_response(guard.ssl, client_fd, 400,
                             "{\"error\":\"missing command parameter\"}");
        } else if (!command_allowed(cmd)) {
          publish_security_event(auth, client_ip, "COMMAND_BLOCKED",
                                 "EXECUTE_COMMAND", cmd,
                                 "Command is outside HTTP /exec allowlist");
          send_http_response(guard.ssl, client_fd, 403,
                             "{\"error\":\"command_not_allowed\"}");
        } else {
          ExecutionRequest exec_request;
          exec_request.request_id = std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count());
          // BUG: X-TinyShell-User/Tenant let clients forge audit identity.
          // FIX: execution identity is copied from the verified token only.
          exec_request.user = auth.user;
          exec_request.tenant = auth.tenant;
          exec_request.client_ip = std::move(client_ip);
          exec_request.command = cmd;

          const auto result = execution_engine_.execute(exec_request);
          if (spine_bridge_) // ADD
            spine_bridge_->watch_job(exec_request.request_id);
          metrics_.record_command(
              result.ok, std::chrono::milliseconds(result.duration_ms));
          if (result.ok) {
            record_trace(exec_request, result);
            send_http_response(
                guard.ssl, client_fd, 200,
                "{\"ok\":true,\"worker\":\"" + json_escape(result.worker) +
                    "\",\"duration_ms\":" + std::to_string(result.duration_ms) +
                    ",\"output\":\"" + json_escape(result.output) + "\"}");
          } else {
            record_trace(exec_request, result);
            send_http_response(
                guard.ssl, client_fd, 500,
                "{\"ok\":false,\"worker\":\"" + json_escape(result.worker) +
                    "\",\"duration_ms\":" + std::to_string(result.duration_ms) +
                    ",\"error\":\"" + json_escape(result.error) + "\"}");
          }
        }
      }
    } else {
      std::string json = "{";
      json += "\"nodes\":" + std::to_string(scheduler_.cluster_size()) + ",";
      json += "\"commands_ok\":" + std::to_string(metrics_.ok_count()) + ",";
      json += "\"alerts\":0,";
      json += "\"risk\":" + std::to_string(current_risk_.load()) + ",";
      json +=
          "\"drift\":" + std::string(intent_drift_.load() ? "true" : "false") +
          ",";
      json += "\"status\":\"ACTIVE\"";
      json += "}";
      auto auth =
          authorize(request, client_ip, Permission::READ_SYSTEM_METRICS, "/");
      if (!auth.ok) {
        send_http_response(guard.ssl, client_fd, 401,
                           "{\"error\":\"unauthorized\"}");
      } else if (auth.forbidden) {
        send_http_response(guard.ssl, client_fd, 403,
                           "{\"error\":\"forbidden\"}");
      } else {
        send_http_response(guard.ssl, client_fd, 200, json);
      }
    }
  }

  void serve_loop() {
    while (running_.load()) {
      sockaddr_in client_addr{};
      socklen_t client_len = sizeof(client_addr);
      int client_fd =
          accept(server_fd_, reinterpret_cast<struct sockaddr *>(&client_addr),
                 &client_len);
      if (client_fd < 0) {
        if (!running_.load())
          break;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        continue;
      }

      char client_ip[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
      std::string ip(client_ip);

      if (!request_pool_ || !request_pool_->submit([this, client_fd, ip]() {
            handle_request(client_fd, ip);
          })) {
        // BUG: overload responses were written in plaintext on a TLS API port.
        // FIX: fail closed by closing before any bearer-bearing HTTP exchange.
        ::close(client_fd);
      }
    }
  }

  int port_;
  std::string bind_addr_;
  std::string expected_token_;
  int server_fd_ = -1;
  SSL_CTX *tls_ctx_ = nullptr;
  JobScheduler &scheduler_;
  std::atomic<int> &current_risk_;
  std::atomic<bool> &intent_drift_;
  Metrics &metrics_;
  std::function<std::size_t()> queue_depth_fn_;
  ExecutionEventBus owned_event_bus_;
  ExecutionEventBus *event_bus_;
  ExecutionEngine execution_engine_;
  RbacManager rbac_manager_;
  MultiTenantManager multi_tenant_manager_;
  SpineEventBridge *spine_bridge_ = nullptr;
  std::vector<RemoteNode> workers_;
  std::chrono::steady_clock::time_point started_at_;
  std::atomic<bool> running_{false};
  std::thread api_thread_;
  std::thread probe_thread_;
  std::unique_ptr<ThreadPool> request_pool_;

  // FIX[P-4]: Background-refreshed worker health cache.
  mutable std::mutex probe_cache_mutex_;
  std::unordered_map<std::string, bool> worker_health_cache_;
  std::unordered_map<std::string, int> missed_probe_counts_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      reconnect_due_;
  mutable std::mutex traces_mutex_;
  std::deque<TraceRecord> traces_;
  std::mutex rate_mutex_;
  std::unordered_map<std::string, RateBucket> rate_buckets_;
  std::string admin_token_;
  std::string viewer_token_;
};

} // namespace tsh
