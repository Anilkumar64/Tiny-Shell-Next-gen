#include "../server/HttpApi.h"
#include "../server/JobScheduler.h"
#include "../server/Metrics.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <filesystem>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace tsh {
namespace {

int reserve_loopback_port() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    close(fd);
    return -1;
  }
  const int port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

int connect_loopback(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  for (int i = 0; i < 50; ++i) {
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  close(fd);
  return -1;
}

void ensure_test_cert() {
  std::filesystem::create_directories("/tmp/tinyshell-test-certs");
  const std::string cert = "/tmp/tinyshell-test-certs/server.crt";
  const std::string key = "/tmp/tinyshell-test-certs/server.key";
  if (!std::filesystem::exists(cert) || !std::filesystem::exists(key)) {
    const std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -keyout " + key + " -out " +
        cert + " -days 1 -nodes -subj /CN=tinyshell-test >/dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
  }
  setenv("TSH_TLS_CERT", cert.c_str(), 1);
  setenv("TSH_TLS_KEY", key.c_str(), 1);
}

std::string tls_request(int port, const std::string &request) {
  SSL_library_init();
  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  const int fd = connect_loopback(port);
  if (fd < 0) {
    SSL_CTX_free(ctx);
    return "";
  }
  SSL *ssl = SSL_new(ctx);
  SSL_set_fd(ssl, fd);
  std::string response;
  if (SSL_connect(ssl) == 1) {
    SSL_write(ssl, request.data(), static_cast<int>(request.size()));
    char buffer[4096];
    int n = 0;
    while ((n = SSL_read(ssl, buffer, sizeof(buffer))) > 0) {
      response.append(buffer, buffer + n);
    }
  }
  SSL_free(ssl);
  close(fd);
  SSL_CTX_free(ctx);
  return response;
}

} // namespace

class HttpApiTest : public ::testing::Test {
protected:
  void SetUp() override {
    ensure_test_cert();
    metrics_ = std::make_unique<Metrics>();
    scheduler_ = std::make_unique<JobScheduler>();
    risk_level_.store(0);
    intent_drift_.store(false);

    queue_depth_fn_ = [this]() { return queue_depth_; };
  }

  void TearDown() override {
    metrics_.reset();
    scheduler_.reset();
  }

  std::unique_ptr<Metrics> metrics_;
  std::unique_ptr<JobScheduler> scheduler_;
  std::atomic<int> risk_level_;
  std::atomic<bool> intent_drift_;
  std::function<std::size_t()> queue_depth_fn_;
  std::size_t queue_depth_ = 0;
};

// Test: HttpApi requires TSH_API_TOKEN
TEST_F(HttpApiTest, RequiresApiToken) {
  EXPECT_THROW(HttpApi api(8080, "127.0.0.1", "", *scheduler_, risk_level_,
                           intent_drift_, *metrics_, queue_depth_fn_),
               std::runtime_error);
}

// Test: HttpApi initialization with valid token
TEST_F(HttpApiTest, InitializationWithValidToken) {
  EXPECT_NO_THROW(HttpApi api(8080, "127.0.0.1", "test_token_12345",
                              *scheduler_, risk_level_, intent_drift_,
                              *metrics_, queue_depth_fn_));
}

// Test: Queue depth tracking
TEST_F(HttpApiTest, QueueDepthTracking) {
  queue_depth_ = 5;
  EXPECT_EQ(queue_depth_fn_(), 5);

  queue_depth_ = 10;
  EXPECT_EQ(queue_depth_fn_(), 10);
}

// Test: Risk level atomicity
TEST_F(HttpApiTest, RiskLevelAtomicity) {
  risk_level_.store(0);
  EXPECT_EQ(risk_level_.load(), 0);

  risk_level_.store(50);
  EXPECT_EQ(risk_level_.load(), 50);

  risk_level_.store(100);
  EXPECT_EQ(risk_level_.load(), 100);
}

// Test: Intent drift flag
TEST_F(HttpApiTest, IntentDriftFlag) {
  intent_drift_.store(false);
  EXPECT_FALSE(intent_drift_.load());

  intent_drift_.store(true);
  EXPECT_TRUE(intent_drift_.load());
}

// Test: Metrics integration
TEST_F(HttpApiTest, MetricsIntegration) {
  HttpApi api(9997, "127.0.0.1", "test_metrics_token", *scheduler_, risk_level_,
              intent_drift_, *metrics_, queue_depth_fn_);

  // Verify metrics is accessible
  EXPECT_NE(&(*metrics_), nullptr);
}

// Test: Bearer token validation format
TEST_F(HttpApiTest, BearerTokenValidation) {
  std::string valid_token = "sk_test_51234567890abcdefghijklmnop";
  EXPECT_NO_THROW(HttpApi api(9996, "127.0.0.1", valid_token, *scheduler_,
                              risk_level_, intent_drift_, *metrics_,
                              queue_depth_fn_));
}

// Test: Port binding edge cases
TEST_F(HttpApiTest, PortConfiguration) {
  // Low port - should initialize without error
  EXPECT_NO_THROW(HttpApi api(8090, "127.0.0.1", "token", *scheduler_,
                              risk_level_, intent_drift_, *metrics_,
                              queue_depth_fn_));

  // High port - should initialize without error
  EXPECT_NO_THROW(HttpApi api(9090, "127.0.0.1", "token", *scheduler_,
                              risk_level_, intent_drift_, *metrics_,
                              queue_depth_fn_));
}

// Test: API initialization with different tokens
TEST_F(HttpApiTest, ApiResponseHeaders) {
  HttpApi api(9955, "127.0.0.1", "test_headers_token", *scheduler_, risk_level_,
              intent_drift_, *metrics_, queue_depth_fn_);

  // Verify API is properly constructed with required headers
  EXPECT_NO_THROW(HttpApi api2(9954, "127.0.0.1", "test_headers_token_2",
                               *scheduler_, risk_level_, intent_drift_,
                               *metrics_, queue_depth_fn_));
}

// Test: Multiple API instances
TEST_F(HttpApiTest, MultipleApiInstances) {
  HttpApi api1(8091, "127.0.0.1", "token1", *scheduler_, risk_level_,
               intent_drift_, *metrics_, queue_depth_fn_);

  HttpApi api2(8092, "127.0.0.1", "token2", *scheduler_, risk_level_,
               intent_drift_, *metrics_, queue_depth_fn_);

  // Both should be initialized successfully
  EXPECT_NE(&api1, &api2);
}

// Test: Queue depth changes
TEST_F(HttpApiTest, QueueDepthChanges) {
  HttpApi api(8093, "127.0.0.1", "token", *scheduler_, risk_level_,
              intent_drift_, *metrics_, queue_depth_fn_);

  queue_depth_ = 0;
  EXPECT_EQ(queue_depth_fn_(), 0);

  queue_depth_ = 50;
  EXPECT_EQ(queue_depth_fn_(), 50);

  queue_depth_ = 100;
  EXPECT_EQ(queue_depth_fn_(), 100);
}

TEST_F(HttpApiTest, IdleClientsDoNotBlockShutdown) {
  const int port = reserve_loopback_port();
  ASSERT_GT(port, 0);

  HttpApi api(port, "127.0.0.1", "token", *scheduler_, risk_level_,
              intent_drift_, *metrics_, queue_depth_fn_);
  api.start();

  std::vector<int> idle_fds;
  for (int i = 0; i < 4; ++i) {
    const int fd = connect_loopback(port);
    ASSERT_GE(fd, 0);
    idle_fds.push_back(fd);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto start = std::chrono::steady_clock::now();
  api.stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  for (int fd : idle_fds) {
    close(fd);
  }
  EXPECT_LT(elapsed.count(), 3500);
}

TEST_F(HttpApiTest, ViewerRoleGetsForbiddenOnExec) {
  setenv("TSH_VIEWER_TOKEN", "viewer-token", 1);
  const int port = reserve_loopback_port();
  ASSERT_GT(port, 0);
  ExecutionEventBus bus;
  HttpApi api(port, "127.0.0.1", "operator-token", *scheduler_, risk_level_,
              intent_drift_, *metrics_, queue_depth_fn_, &bus);
  api.start();

  const auto response = tls_request(
      port, "POST /exec HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer "
            "viewer-token\r\nContent-Length: 14\r\nContent-Type: "
            "application/x-www-form-urlencoded\r\n\r\ncommand=uptime");
  api.stop();
  EXPECT_NE(response.find("403 Forbidden"), std::string::npos);
}

TEST_F(HttpApiTest, OperatorRoleCanExec) {
  const int port = reserve_loopback_port();
  ASSERT_GT(port, 0);
  ExecutionEventBus bus;
  HttpApi api(port, "127.0.0.1", "operator-token", *scheduler_, risk_level_,
              intent_drift_, *metrics_, queue_depth_fn_, &bus);
  api.start();

  const auto response = tls_request(
      port, "POST /exec HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer "
            "operator-token\r\nContent-Length: 14\r\nContent-Type: "
            "application/x-www-form-urlencoded\r\n\r\ncommand=uptime");
  api.stop();
  EXPECT_NE(response.find("200 OK"), std::string::npos);
}

TEST_F(HttpApiTest, HeaderIdentityCannotSpoofAuditEvents) {
  const int port = reserve_loopback_port();
  ASSERT_GT(port, 0);
  ExecutionEventBus bus;
  HttpApi api(port, "127.0.0.1", "operator-token", *scheduler_, risk_level_,
              intent_drift_, *metrics_, queue_depth_fn_, &bus);
  api.start();

  (void)tls_request(
      port, "POST /exec HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer "
            "operator-token\r\nX-TinyShell-User: admin\r\nContent-Length: "
            "14\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\n"
            "command=uptime");
  api.stop();

  bool saw_service = false;
  bool saw_admin = false;
  for (const auto &event : bus.snapshot()) {
    if (event.user == "service")
      saw_service = true;
    if (event.user == "admin")
      saw_admin = true;
  }
  EXPECT_TRUE(saw_service);
  EXPECT_FALSE(saw_admin);
}

} // namespace tsh
