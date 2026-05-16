#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

class ITransport {
public:
  virtual ~ITransport() = default;
  virtual bool send(const void *buffer, size_t length) = 0;
  virtual bool recv(void *buffer, size_t length) = 0;
  virtual int raw_fd() const { return -1; }
};
