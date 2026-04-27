#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>

namespace tsh {
    // I-1: 60fps Decoupled TUI Engine
    // I-2: Incremental Diff / Double-Buffered Renderer
    class TuiCore {
        int width = 100, height = 30;
        std::vector<char> front_buffer;
        std::vector<char> back_buffer;
        std::mutex buf_mtx;
        std::vector<std::string> audit_logs;

    public:
        TuiCore() {
            front_buffer.assign(width * height, ' ');
            back_buffer.assign(width * height, ' ');
            // Hide cursor and clear screen
            std::cout << "\033[?25l\033[2J"; 
        }

        ~TuiCore() {
            // Show cursor and reset colors
            std::cout << "\033[?25h\033[0m\n";
        }

        void write_at(int x, int y, const std::string& text, const std::string& color = "") {
            std::lock_guard<std::mutex> lock(buf_mtx);
            if (y >= height || x >= width) return;
            for (size_t i = 0; i < text.length() && (x + i) < (size_t)width; ++i) {
                back_buffer[y * width + (x + i)] = text[i];
            }
        }

        void log_message(const std::string& msg) {
            std::lock_guard<std::mutex> lock(buf_mtx);
            audit_logs.push_back(msg);
            if (audit_logs.size() > 10) audit_logs.erase(audit_logs.begin());
        }

        void draw_audit_log(int x, int y) {
            std::lock_guard<std::mutex> lock(buf_mtx);
            for (size_t i = 0; i < audit_logs.size(); ++i) {
                std::string line = audit_logs[i];
                for (size_t j = 0; j < line.length() && (x + j) < (size_t)width; ++j) {
                    back_buffer[(y + i) * width + (x + j)] = line[j];
                }
            }
        }

        // B-1: Live Sparkline Generator using Unicode Braille (sub-character resolution)
        std::string generate_sparkline(const std::vector<double>& values, int max_width) {
            std::string spark = "";
            static const char* bars[] = {" ", " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
            double max_val = *std::max_element(values.begin(), values.end());
            if (max_val == 0) max_val = 1.0;

            for (size_t i = std::max(0, (int)values.size() - max_width); i < values.size(); ++i) {
                int idx = static_cast<int>((values[i] / max_val) * 8);
                spark += bars[idx];
            }
            return spark;
        }

        // I-2: Incremental Renderer (only draws changed cells)
        void render_frame() {
            std::lock_guard<std::mutex> lock(buf_mtx);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    int idx = y * width + x;
                    if (back_buffer[idx] != front_buffer[idx]) {
                        // Move cursor to x,y and print new char
                        std::cout << "\033[" << (y + 1) << ";" << (x + 1) << "H" << back_buffer[idx];
                        front_buffer[idx] = back_buffer[idx];
                    }
                }
            }
            std::cout.flush();
        }
    };
}
