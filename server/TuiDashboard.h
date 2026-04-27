#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <iomanip>

namespace tsh {
    class TuiDashboard {
    public:
        static void clear_screen() {
            std::cout << "\033[2J\033[H";
        }

        static void move_cursor(int row, int col) {
            std::cout << "\033[" << row << ";" << col << "H";
        }

        static void draw_header(const std::string& title) {
            std::cout << "\033[1;37;44m" << std::setw(80) << std::left << ("  " + title) << "\033[0m\n";
        }

        static void update_stats(int active_nodes, double throughput_mb, int security_alerts) {
            move_cursor(3, 2);
            std::cout << "\033[1;32mActive Nodes: \033[0m" << active_nodes << "    ";
            
            move_cursor(4, 2);
            std::cout << "\033[1;36mThroughput:   \033[0m" << std::fixed << std::setprecision(2) << throughput_mb << " MB/s    ";

            move_cursor(5, 2);
            std::cout << "\033[1;31mSecurity Alerts: \033[0m" << security_alerts << "    ";
        }

        static void draw_audit_log(const std::vector<std::string>& logs) {
            move_cursor(7, 1);
            std::cout << "\033[1;33m--- RECENT SECURITY EVENTS ---\033[0m\n";
            int row = 8;
            for (auto it = logs.rbegin(); it != logs.rend() && row < 20; ++it, ++row) {
                move_cursor(row, 2);
                std::cout << "\033[K" << *it; // Clear line and print
            }
        }

        static void render_frame(int nodes, double mb, int alerts, const std::vector<std::string>& logs) {
            clear_screen();
            draw_header("TINYSHELL NEXTGEN - DISTRIBUTED ORCHESTRATOR DASHBOARD");
            update_stats(nodes, mb, alerts);
            draw_audit_log(logs);
            std::cout.flush();
        }
    };
}
