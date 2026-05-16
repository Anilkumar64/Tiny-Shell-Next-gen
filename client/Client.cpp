#include "Client.h"
#include "SecureChannel.h"
#include "transport/TcpTransport.h"
#include "AiContext.h"
#include "AuditLedger.h"
#include "TuiHolograph.h"
#include "ControllerIdentity.h"
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <termios.h>

namespace {
std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c);
    }).base();
    if (first >= last) return "";
    return std::string(first, last);
}

class RawTerminal {
public:
    RawTerminal() {
        active_ = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original_) == 0;
        if (!active_) return;
        termios raw = original_;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            active_ = false;
        }
    }

    ~RawTerminal() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_);
        }
    }

private:
    termios original_{};
    bool active_ = false;
};

void run_shell_session(SecureChannel& sc) {
    std::cout << "Entering remote shell. Press Ctrl-] to close the session.\n";
    RawTerminal raw_terminal;
    std::atomic<bool> active{true};

    std::thread receiver([&]() {
        while (active.load()) {
            std::vector<uint8_t> resp;
            SecureChannel::MsgType resp_type;
            if (!sc.receive_message(resp, resp_type)) {
                active.store(false);
                break;
            }
            if (resp_type == SecureChannel::MsgType::PTY_OUTPUT ||
                resp_type == SecureChannel::MsgType::PTY_EXIT) {
                std::cout.write(reinterpret_cast<const char*>(resp.data()), static_cast<std::streamsize>(resp.size()));
                std::cout.flush();
            }
            if (resp_type == SecureChannel::MsgType::PTY_EXIT) {
                active.store(false);
                break;
            }
        }
    });

    while (active.load()) {
        char buffer[1024];
        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n <= 0) {
            active.store(false);
            break;
        }
        for (ssize_t i = 0; i < n; ++i) {
            if (buffer[i] == 0x1d) {
                active.store(false);
                sc.send_message({}, SecureChannel::MsgType::PTY_EXIT);
                break;
            }
        }
        if (!active.load()) break;
        std::vector<uint8_t> input(buffer, buffer + n);
        if (!sc.send_message(input, SecureChannel::MsgType::PTY_INPUT)) {
            active.store(false);
            break;
        }
    }

    if (receiver.joinable()) receiver.join();
    std::cout << "\nReturned to TinyShell command mode.\n";
}
} // namespace

Client::Client(const std::string& h, int p) : host(h), port(p) {}

void Client::run() {
    const auto controller_id = tsh::ControllerIdentity::load_or_create_id();
    int sock = -1;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr);

        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == 0) {
            break;
        }
        close(sock);
        sock = -1;
        // FIX[REL-3]: Retry transient startup/network races before failing the CLI.
        std::cerr << "Connection attempt " << attempt << " failed\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
    }

    if (sock < 0) {
        std::cerr << "Connection failed after 3 attempts\n";
        return;
    }

    auto transport = std::make_unique<TcpTransport>(sock);
    SecureChannel sc(std::move(transport));
    sc.set_peer_identity(host + ":" + std::to_string(port));

    if (sc.handshake(SecureChannel::Mode::CLIENT)) {
        std::cout << "Handshake successful! Connected to TinyShell NextGen.\n";
        const std::string hello = "controller-hello " + controller_id;
        if (!sc.send_message(std::vector<uint8_t>(hello.begin(), hello.end()), SecureChannel::MsgType::COMMAND)) {
            std::cerr << "Connection closed while announcing controller identity.\n";
            return;
        }
        std::vector<uint8_t> hello_resp;
        SecureChannel::MsgType hello_type;
        if (!sc.receive_message(hello_resp, hello_type)) {
            std::cerr << "Connection closed before controller identity was acknowledged.\n";
            return;
        }
        std::cout << std::string(hello_resp.begin(), hello_resp.end());
        
        while (true) {
            std::cout << "tsh> ";
            std::string line;
            if (!std::getline(std::cin, line) || line == "exit") break;
            line = trim(line);
            if (line.empty()) continue;
            if (line == "controller-id") {
                std::cout << controller_id << "\n";
                continue;
            }

            SecureChannel::MsgType type = SecureChannel::MsgType::COMMAND;

            // AI Intent Analysis (Feature 7)
            if (line.rfind("?", 0) == 0) {
                line = AiContext::analyze_intent(line.substr(1));
                std::cout << "[AI] Formulated command: " << line << "\n";
            }
            
            // Feature 14: TUI Holograph
            if (line == "holograph") {
                TuiHolograph::render_process_tree("");
                continue;
            }

            // Removed: steganographic command channels have no place in a legitimate production client.

            if (line.rfind("WASM:", 0) == 0) {
                type = SecureChannel::MsgType::WASM_PAYLOAD;
                line = line.substr(5);
            }
            // Removed: arbitrary remote binary execution is disabled outside controlled research builds.

            std::vector<uint8_t> msg(line.begin(), line.end());
            if (!sc.send_message(msg, type)) {
                std::cerr << "Connection closed while sending command.\n";
                break;
            }

            if (line == "shell" && type == SecureChannel::MsgType::COMMAND) {
                run_shell_session(sc);
                continue;
            }

            std::vector<uint8_t> resp;
            SecureChannel::MsgType resp_type;
            if (!sc.receive_message(resp, resp_type)) {
                std::cerr << "Connection closed before a response was received.\n";
                break;
            }

            std::string response(resp.begin(), resp.end());
            std::cout << response;

            // Feature 12: Audit Ledger
            AuditLedger::commit(line, response);
        }
    } else {
        std::cerr << "Handshake failed\n";
    }
}
