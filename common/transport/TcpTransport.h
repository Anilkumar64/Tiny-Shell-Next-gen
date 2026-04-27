#pragma once
#include "ITransport.h"
#include <sys/socket.h>
#include <unistd.h>

class TcpTransport : public ITransport {
public:
    explicit TcpTransport(int sockfd) : fd(sockfd) {}
    ~TcpTransport() { if (fd >= 0) close(fd); }

    bool send(const void* buffer, size_t length) override {
        const uint8_t* ptr = static_cast<const uint8_t*>(buffer);
        size_t total = 0;
        while (total < length) {
            ssize_t n = ::send(fd, ptr + total, length - total, 0);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    bool recv(void* buffer, size_t length) override {
        uint8_t* ptr = static_cast<uint8_t*>(buffer);
        size_t total = 0;
        while (total < length) {
            ssize_t n = ::recv(fd, ptr + total, length - total, 0);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

private:
    int fd;
};
