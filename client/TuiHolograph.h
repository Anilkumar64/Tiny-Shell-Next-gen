#pragma once
#include <string>
#include <iostream>

class TuiHolograph {
public:
    static void render_process_tree(const std::string& raw_data) {
        (void)raw_data;
        // Simple TUI simulation using ANSI colors
        std::cout << "\033[1;36m[Holograph] Interactive Process Topology\033[0m\n";
        std::cout << "├─ systemd (1)\n";
        std::cout << "│  ├─ sshd (1024)\n";
        std::cout << "│  └─ \033[1;32mtsh_server (2048) [ACTIVE]\033[0m\n";
        std::cout << "│     └─ \033[1;33mbash (2049)\033[0m\n";
        std::cout << "└─ kthreadd (2)\n";
    }
};
