#pragma once
#include <string>

class Client {
public:
    Client(const std::string& host = "127.0.0.1", int port = 4444);
    void run();
private:
    std::string host;
    int port;
};
