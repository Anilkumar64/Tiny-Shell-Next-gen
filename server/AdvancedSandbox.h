#pragma once
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <stddef.h>
#include <iostream>

#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif

namespace tsh {
    class AdvancedSandbox {
    public:
        static void apply_policy() {
            struct sock_filter filter[] = {
                /* Validate architecture */
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offsetof(struct seccomp_data, arch))),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

                /* Load syscall number */
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offsetof(struct seccomp_data, nr))),

                /* List of BANNED syscalls for shell leaf nodes */
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_execve, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
                
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (0x01 & SECCOMP_RET_DATA)),

                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ptrace, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

                /* Default: Allow everything else */
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
            };

            struct sock_fprog prog = {
                .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
                .filter = filter,
            };

            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
                perror("prctl(NO_NEW_PRIVS)");
                return;
            }

            if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
                perror("prctl(SECCOMP)");
                std::cerr << "[Sandbox] BPF Filter installation failed. Host security downgraded.\n";
            } else {
                std::cout << "[Sandbox] Fine-grained BPF filter active for leaf node.\n";
            }
        }
    };
}
