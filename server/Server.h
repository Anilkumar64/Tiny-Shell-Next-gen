#pragma once

class Server {
public:
    Server(int port = 4444);
    void run();
private:
    int port;
};
