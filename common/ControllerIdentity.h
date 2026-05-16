#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <openssl/rand.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tsh {

class ControllerIdentity {
public:
    static std::string load_or_create_id() {
        const auto path = state_dir() / "controller_id";
        if (std::filesystem::exists(path)) {
            std::ifstream in(path);
            std::string id;
            std::getline(in, id);
            if (!id.empty()) return id;
        }

        unsigned char bytes[16] = {};
        if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
            throw std::runtime_error("failed to generate controller identity");
        }
        const std::string id = "ctrl-" + to_hex(std::vector<unsigned char>(bytes, bytes + sizeof(bytes)));
        std::ofstream out(path, std::ios::trunc);
        out << id << "\n";
        out.close();
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        return id;
    }

private:
    static std::filesystem::path state_dir() {
        const char* home = std::getenv("HOME");
        if (!home) throw std::runtime_error("HOME is required for TinyShell controller identity");
        auto dir = std::filesystem::path(home) / ".tsh";
        std::filesystem::create_directories(dir);
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
        return dir;
    }

    static std::string to_hex(const std::vector<unsigned char>& bytes) {
        std::ostringstream out;
        for (unsigned char byte : bytes) {
            out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        return out.str();
    }
};

} // namespace tsh
