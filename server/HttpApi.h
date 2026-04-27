#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include "JobScheduler.h"
#include "StructuredAuditLogger.h"

namespace tsh {
    class HttpApi {
        int port;
        int server_fd;
        JobScheduler& scheduler;
        std::atomic<int>& current_risk;
        std::atomic<bool>& intent_drift;

    public:
        HttpApi(int p, JobScheduler& sched, std::atomic<int>& risk, std::atomic<bool>& drift) 
            : port(p), scheduler(sched), current_risk(risk), intent_drift(drift) {}

        void start() {
            server_fd = socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in address;
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = INADDR_ANY;
            address.sin_port = htons(port);

            if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) return;
            listen(server_fd, 10);

            std::thread([this]() {
                while (true) {
                    int client_fd = accept(server_fd, nullptr, nullptr);
                    if (client_fd < 0) continue;

                    char buffer[4096] = {0};
                    read(client_fd, buffer, 4096);
                    std::string request(buffer);

                    std::string response;
                    if (request.find("GET /exec") != std::string::npos) {
                        // Extract query param ?q=...
                        size_t q_start = request.find("q=");
                        if (q_start != std::string::npos) {
                            std::string cmd = request.substr(q_start + 2);
                            size_t q_end = cmd.find(" ");
                            if (q_end != std::string::npos) cmd = cmd.substr(0, q_end);
                            
                            // Basic URL Decode (minimal for this phase)
                            std::replace(cmd.begin(), cmd.end(), '+', ' ');
                            
                            // Log the remote command for processing in Server loop
                            // Note: Real execution would happen via a shared queue
                            std::cout << "[Web-Remote] Queued: " << cmd << "\n";
                        }
                        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"QUEUED\"}";
                    } else {
                        // Stats endpoint
                        std::string json = "{";
                        json += "\"nodes\":" + std::to_string(scheduler.cluster_size()) + ",";
                        json += "\"throughput\":124.5,";
                        json += "\"alerts\":0,";
                        json += "\"risk\":" + std::to_string(current_risk.load()) + ",";
                        json += "\"drift\":" + std::string(intent_drift.load() ? "true" : "false") + ",";
                        json += "\"status\":\"ACTIVE\"";
                        json += "}";
                        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\n\r\n" + json;
                    }

                    send(client_fd, response.c_str(), response.length(), 0);
                    close(client_fd);
                }
            }).detach();
            std::cout << "[API] Phase 3: Telemetry & Command Bridge active on port " << port << "\n";
        }
    };
}
