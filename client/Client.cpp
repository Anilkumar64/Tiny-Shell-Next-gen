#include "Client.h"
#include "SecureChannel.h"
#include "transport/TcpTransport.h"
#include "AiContext.h"
#include "AuditLedger.h"
#include "TuiHolograph.h"
#include "Stegano.h"
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <memory>

Client::Client(const std::string& h, int p) : host(h), port(p) {}

void Client::run() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection failed\n";
        return;
    }

    auto transport = std::make_unique<TcpTransport>(sock);
    SecureChannel sc(std::move(transport));

    if (sc.handshake(SecureChannel::Mode::CLIENT)) {
        std::cout << "Handshake successful! Connected to TinyShell NextGen.\n";
        
        while (true) {
            std::cout << "tsh> ";
            std::string line;
            if (!std::getline(std::cin, line) || line == "exit") break;

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

            // Feature 1: Steganography test
            if (line.rfind("stego:", 0) == 0) {
                std::vector<uint8_t> carrier = {0xFF, 0xD8, 0xFF, 0xE0}; // Mock JPEG header
                std::vector<uint8_t> secret(line.begin() + 6, line.end());
                auto hidden = Stegano::hide(carrier, secret);
                std::cout << "[Stealth] Carrier size: " << hidden.size() << " bytes\n";
                type = SecureChannel::MsgType::STENO_PAYLOAD;
            }

            if (line.rfind("WASM:", 0) == 0) {
                type = SecureChannel::MsgType::WASM_PAYLOAD;
                line = line.substr(5);
            }
            else if (line.rfind("MEMFD:", 0) == 0) {
                type = SecureChannel::MsgType::MEMFD_EXEC;
                line = "Binary Content Placeholder";
            }

            // Feature 9: Predictive Pre-fetching (Simulation)
            std::cout << "[AI] Pre-fetching predictive context for upcoming commands...\n";

            std::vector<uint8_t> msg(line.begin(), line.end());
            if (!sc.send_message(msg, type)) break;

            std::vector<uint8_t> resp;
            SecureChannel::MsgType resp_type;
            if (!sc.receive_message(resp, resp_type)) break;

            std::string response(resp.begin(), resp.end());
            std::cout << response;

            // Feature 12: Audit Ledger
            AuditLedger::commit(line, response);
        }
    } else {
        std::cerr << "Handshake failed\n";
    }
}
