#pragma once
#include "ITransport.h"
#include <cerrno>
#include <cstdint>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

class TcpTransport : public ITransport {
public:
  explicit TcpTransport(int sockfd) : fd(sockfd) {
    // FIX[REL-3]: Bound socket operations so handshakes/reads cannot hang
    // forever.
    timeval timeout{};
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  }
  ~TcpTransport() {
    if (fd >= 0)
      close(fd);
  }

  bool send(const void *buffer, size_t length) override {
    const uint8_t *ptr = static_cast<const uint8_t *>(buffer);
    size_t total = 0;
    while (total < length) {
      ssize_t n = ::send(fd, ptr + total, length - total, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      total += static_cast<size_t>(n);
    }
    return true;
  }

  bool recv(void *buffer, size_t length) override {
    uint8_t *ptr = static_cast<uint8_t *>(buffer);
    size_t total = 0;
    while (total < length) {
      ssize_t n = ::recv(fd, ptr + total, length - total, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      total += static_cast<size_t>(n);
    }
    return true;
  }

  int raw_fd() const override { return fd; }

private:
  int fd;
};
