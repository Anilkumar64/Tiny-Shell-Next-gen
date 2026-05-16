#include "Server.h"
#include "../config/tsh_config.h"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const auto cfg = tsh::Config::load_from_env();
        cfg.require_server_secrets();

        std::cout << "[TinyShell] Starting server on port " << cfg.server_port << "\n" << std::flush;
        Server s(cfg.server_port);
        s.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[TinyShell] Startup failed: " << e.what() << "\n\n"
                  << "Set both required secrets before starting the server.\n"
                  << "Example:\n"
                  << "  sudo env TSH_API_TOKEN='change-me-api-token' "
                  << "TSH_ZK_SECRET='change-me-zk-secret-at-least-32-bytes' ./tsh_server\n";
        return 1;
    }
}
