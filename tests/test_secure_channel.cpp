#include "SecureChannel.h"
#include "transport/TcpTransport.h"
#include <cstdlib>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <fstream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
void run_or_fail(const std::string &command) {
  const int rc = std::system(command.c_str());
  ASSERT_EQ(rc, 0) << command;
}
} // namespace

TEST(SecureChannelTest, EncryptDecryptRoundTrip) {
  auto home =
      std::filesystem::temp_directory_path() / "tsh_secure_channel_test";
  std::filesystem::remove_all(home);
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(home / ".tsh");
  setenv("HOME", home.c_str(), 1);

  const auto server_key = home / "server.key";
  const auto server_crt = home / "server.crt";
  const auto client_key = home / ".tsh" / "client.key";
  const auto client_crt = home / ".tsh" / "client.crt";
  const auto authorized_keys = home / "authorized_keys";
  const auto config = home / ".tsh" / "config";

  run_or_fail("openssl genpkey -algorithm ED25519 -out " +
              client_key.string());
  run_or_fail("openssl req -new -x509 -key " + client_key.string() +
              " -out " + client_crt.string() +
              " -subj /CN=tsh-client-test -days 1");
  run_or_fail("openssl pkey -in " + client_key.string() +
              " -pubout -out " + authorized_keys.string());

  run_or_fail("openssl genpkey -algorithm ED25519 -out " +
              server_key.string());
  run_or_fail("openssl req -new -x509 -key " + server_key.string() +
              " -out " + server_crt.string() +
              " -subj /CN=tsh-server-test -days 1");
  run_or_fail("openssl x509 -in " + server_crt.string() +
              " -pubkey -noout > " + config.string());

  setenv("TSH_TLS_KEY", server_key.c_str(), 1);
  setenv("TSH_TLS_CERT", server_crt.c_str(), 1);
  setenv("TSH_AUTHORIZED_KEYS", authorized_keys.c_str(), 1);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  auto server_future = std::async(std::launch::async, [&]() {
    SecureChannel server(std::make_unique<TcpTransport>(fds[0]));
    EXPECT_TRUE(server.handshake(SecureChannel::Mode::SERVER));
    std::vector<uint8_t> msg;
    SecureChannel::MsgType type;
    EXPECT_TRUE(server.receive_message(msg, type));
    EXPECT_EQ(type, SecureChannel::MsgType::COMMAND);
    EXPECT_TRUE(server.send_message(msg, SecureChannel::MsgType::COMMAND));
  });

  SecureChannel client(std::make_unique<TcpTransport>(fds[1]));
  client.set_peer_identity("test:1");
  ASSERT_TRUE(client.handshake(SecureChannel::Mode::CLIENT));
  const std::vector<uint8_t> payload = {'o', 'k'};
  ASSERT_TRUE(client.send_message(payload, SecureChannel::MsgType::COMMAND));
  std::vector<uint8_t> response;
  SecureChannel::MsgType response_type;
  ASSERT_TRUE(client.receive_message(response, response_type));
  EXPECT_EQ(response, payload);
  server_future.get();
}
