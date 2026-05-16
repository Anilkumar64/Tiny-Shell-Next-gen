#pragma once
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <stdexcept>
#include <span>

namespace tsh {
    class FastFileLoader {
        int fd = -1;
        void* mapped_data = nullptr;
        size_t file_size = 0;

    public:
        explicit FastFileLoader(const std::string& path) {
            fd = open(path.c_str(), O_RDONLY);
            if (fd < 0) throw std::runtime_error("Could not open file for zero-copy loading.");

            struct stat st;
            fstat(fd, &st);
            file_size = st.st_size;

            // Zero-copy mapping into memory
            mapped_data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapped_data == MAP_FAILED) {
                close(fd);
                throw std::runtime_error("mmap failed.");
            }
        }

        ~FastFileLoader() {
            if (mapped_data != MAP_FAILED) munmap(mapped_data, file_size);
            if (fd >= 0) close(fd);
        }

        // Returns a C++20 span for high-performance non-owning access
        std::span<const uint8_t> data() const {
            return {static_cast<const uint8_t*>(mapped_data), file_size};
        }
    };
}
