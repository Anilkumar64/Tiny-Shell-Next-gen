#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool send(const void* buffer, size_t length) = 0;
    virtual bool recv(void* buffer, size_t length) = 0;
};
