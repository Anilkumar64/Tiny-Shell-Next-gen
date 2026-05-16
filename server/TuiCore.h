#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>
#include <sstream>

namespace tsh {
    // I-1: 60fps Decoupled TUI Engine
    // I-2: Incremental Diff / Double-Buffered Renderer
    class TuiCore {
        int width = 100, height = 30;
        std::vector<char> front_buffer;
        std::vector<char> back_buffer;
        std::mutex buf_mtx;
        std::vector<std::string> audit_logs;
        bool interactive = true;

    public:
        explicit TuiCore(bool enable_interactive = true) : interactive(enable_interactive) {
            front_buffer.assign(width * height, ' ');
            back_buffer.assign(width * height, ' ');
            if (interactive) {
                std::cout << "\033[?25l\033[2J\033[H";
            }
        }

        ~TuiCore() {
            if (interactive) {
                std::cout << "\033[?25h\033[0m\n";
            }
        }

        void write_at(int x, int y, const std::string& text, const std::string& color = "") {
            (void)color;
            if (!interactive) return;
            std::lock_guard<std::mutex> lock(buf_mtx);
            if (y < 0 || x < 0 || y >= height || x >= width) return;
            for (size_t i = 0; i < text.length() && (x + i) < (size_t)width; ++i) {
                back_buffer[y * width + (x + i)] = text[i];
            }
        }

        void log_message(const std::string& msg) {
            std::lock_guard<std::mutex> lock(buf_mtx);
            audit_logs.push_back(msg);
            if (audit_logs.size() > 10) audit_logs.erase(audit_logs.begin());
            if (!interactive) {
                std::cout << msg << "\n";
            }
        }

        void draw_audit_log(int x, int y) {
            if (!interactive) return;
            std::lock_guard<std::mutex> lock(buf_mtx);
            for (size_t i = 0; i < audit_logs.size(); ++i) {
                std::string line = audit_logs[i];
                if (y + static_cast<int>(i) >= height) break;
                for (size_t j = 0; j < line.length() && (x + j) < (size_t)width; ++j) {
                    back_buffer[(y + i) * width + (x + j)] = line[j];
                }
            }
        }

        std::string generate_sparkline(const std::vector<double>& values, int max_width) {
            std::string spark = "";
            static const char bars[] = {' ', '.', ':', '-', '=', '+', '*', '#', '#'};
            double max_val = *std::max_element(values.begin(), values.end());
            if (max_val == 0) max_val = 1.0;

            for (size_t i = std::max(0, (int)values.size() - max_width); i < values.size(); ++i) {
                int idx = static_cast<int>((values[i] / max_val) * 8);
                spark += bars[idx];
            }
            return spark;
        }

        void clear() {
            if (!interactive) return;
            std::lock_guard<std::mutex> lock(buf_mtx);
            std::fill(back_buffer.begin(), back_buffer.end(), ' ');
        }

        void draw_box(int x, int y, int w, int h, const std::string& title = "") {
            if (!interactive) return;
            std::lock_guard<std::mutex> lock(buf_mtx);
            if (w < 2 || h < 2) return;
            auto put = [&](int px, int py, char c) {
                if (px >= 0 && py >= 0 && px < width && py < height) {
                    back_buffer[py * width + px] = c;
                }
            };
            for (int i = 0; i < w; ++i) {
                put(x + i, y, i == 0 || i == w - 1 ? '+' : '-');
                put(x + i, y + h - 1, i == 0 || i == w - 1 ? '+' : '-');
            }
            for (int i = 1; i < h - 1; ++i) {
                put(x, y + i, '|');
                put(x + w - 1, y + i, '|');
            }
            if (!title.empty()) {
                const std::string label = " " + title + " ";
                for (size_t i = 0; i < label.size() && x + 2 + static_cast<int>(i) < x + w - 1; ++i) {
                    put(x + 2 + static_cast<int>(i), y, label[i]);
                }
            }
        }

        void render_frame() {
            if (!interactive) return;
            std::lock_guard<std::mutex> lock(buf_mtx);
            std::ostringstream frame;
            frame << "\033[H";
            for (int row = 0; row < height; ++row) {
                for (int col = 0; col < width; ++col) {
                    const int idx = row * width + col;
                    frame << back_buffer[idx];
                    front_buffer[idx] = back_buffer[idx];
                }
                frame << "\n";
            }
            std::cout << frame.str() << std::flush;
        }
    };
}
