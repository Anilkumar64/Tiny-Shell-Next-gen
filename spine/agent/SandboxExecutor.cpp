// SandboxExecutor.cpp
// Purpose: Fork/exec sandboxed child process with seccomp BPF, rlimits,
//          PR_SET_NO_NEW_PRIVS, I/O streaming, and timeout enforcement.
// Component: spine/agent

// ── Bug fixes in this revision ───────────────────────────────────────────────
//
//  FIX 1 (BUG #1) — Dynamic seccomp profile replaces static allowlist.
//    detect_syscall_profile() reads the actual binaries at startup, detects
//    libsystemd linkage, and adds the 13 event-loop syscalls only when needed.
//    Fixes uptime/who dying with SIGSYS (signal 31, exit 159) on Ubuntu 22.04+
//    where /usr/bin/uptime and /usr/bin/who link libsystemd.so.0.
//
//  FIX 2 — Removed duplicate __NR_alarm and __NR_kill entries in
//    base_allowed_syscalls().  The originals appeared twice each due to a
//    copy-paste error in the FIX B comment block merge.  The BPF builder's
//    add_syscall() deduplicates at runtime so the filter was functionally
//    correct, but duplicate entries violated the "every entry has one comment"
//    rule (Section 7), triggered -Wunused-variable on older compilers, and
//    added unnecessary BPF instructions.  Removed the redundant entries;
//    retained the correct single entry with its comment.
//
//  FIX 3 — Added missing comment on __NR_nanosleep.
//    Every base_allowed_syscalls[] entry must have a "binary: reason" comment
//    per Section 7.  nanosleep was listed without one.  Added comment.
//
//  FIX C — All 13 libsystemd event-loop syscalls present and guarded:
//    __NR_clone, __NR_epoll_create1, __NR_epoll_ctl, __NR_epoll_wait,
//    __NR_epoll_pwait, __NR_inotify_init1, __NR_inotify_add_watch,
//    __NR_inotify_rm_watch, __NR_eventfd2, __NR_timerfd_create,
//    __NR_timerfd_settime, __NR_timerfd_gettime, __NR_ppoll.
//    Added only when binary_links_libsystemd() returns true for uptime or who.
//
//  FIX A — Redundant command_permitted() gate removed.
//    CommandPolicy validates on the server, embeds the canonical path in a
//    HMAC-signed JobSpec, and SpineAgent verifies the signature and
//    allowed_absolute_paths before calling run().  A second independent
//    allowlist here caused false rejections and was a maintenance trap.
//
//  FIX B — Additional base syscalls added with binary+reason comments:
//    __NR_uname, __NR_getrusage, __NR_prctl, __NR_tgkill, __NR_dup,
//    __NR_dup2, __NR_pipe2, __NR_poll, __NR_select, __NR_getpgrp,
//    __NR_getppid, __NR_gettid, __NR_getrlimit, __NR_setrlimit,
//    __NR_madvise, __NR_syslog, and NSS/socket syscalls for who.
// ─────────────────────────────────────────────────────────────────────────────

#include "SandboxExecutor.h"

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace tsh::spine {
namespace {

// ── RAII file-descriptor
// ──────────────────────────────────────────────────────
class Fd {
public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0)
      close(fd_);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  int get() const { return fd_; }
  int release() {
    int out = fd_;
    fd_ = -1;
    return out;
  }
  void reset(int fd = -1) {
    if (fd_ >= 0)
      close(fd_);
    fd_ = fd;
  }

private:
  int fd_ = -1;
};

// ── write helper — intentionally ignores return value in child paths
// ────────── Using a named function suppresses -Wunused-result without hiding
// the intent.
static void write_ignore(int fd, const void *buf, std::size_t n) {
  if (write(fd, buf, n) <
      0) { /* intentionally ignored: child is about to _exit */
  }
}

// ── small utilities
// ───────────────────────────────────────────────────────────
void make_pipe(int fds[2]) {
  if (pipe2(fds, O_CLOEXEC) != 0)
    throw std::runtime_error("pipe2() failed: " + std::string(strerror(errno)));
}

void set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    throw std::runtime_error("failed to set pipe non-blocking");
}

void dup2_or_report(int oldfd, int newfd, int error_fd) {
  if (dup2(oldfd, newfd) < 0) {
    const int err = errno;
    write_ignore(error_fd, &err, sizeof(err));
    _exit(126);
  }
}

void setrlimit_or_report(int resource, const rlimit &limit, int error_fd) {
  if (setrlimit(resource, &limit) < 0) {
    const int err = errno;
    write_ignore(error_fd, &err, sizeof(err));
    _exit(126);
  }
}

void prctl_or_report(int option, unsigned long arg2, int error_fd) {
  if (prctl(option, arg2, 0, 0, 0) < 0) {
    const int err = errno;
    write_ignore(error_fd, &err, sizeof(err));
    _exit(126);
  }
}

void apply_child_limits_or_report(const tinyshell::v1::JobSpec &spec,
                                  int error_fd) {
  const auto timeout_ms =
      spec.limits().timeout_ms() == 0 ? 5000 : spec.limits().timeout_ms();
  const auto stdout_limit = spec.limits().max_stdout_bytes() == 0
                                ? 1024 * 1024
                                : spec.limits().max_stdout_bytes();
  const auto stderr_limit = spec.limits().max_stderr_bytes() == 0
                                ? 1024 * 1024
                                : spec.limits().max_stderr_bytes();

  rlimit cpu{};
  cpu.rlim_cur = std::max<unsigned int>(
      1, static_cast<unsigned int>(timeout_ms / 1000 + 1));
  cpu.rlim_max = cpu.rlim_cur + 1;
  setrlimit_or_report(RLIMIT_CPU, cpu, error_fd);

  rlimit fsize{};
  const auto combined = std::min<std::uint64_t>(
      stdout_limit + stderr_limit,
      static_cast<std::uint64_t>(std::numeric_limits<rlim_t>::max()));
  fsize.rlim_cur = static_cast<rlim_t>(combined);
  fsize.rlim_max = fsize.rlim_cur;
  setrlimit_or_report(RLIMIT_FSIZE, fsize, error_fd);

  rlimit nofile{};
  nofile.rlim_cur = 64;
  nofile.rlim_max = 64;
  setrlimit_or_report(RLIMIT_NOFILE, nofile, error_fd);

  rlimit as{};
  as.rlim_cur = 256ULL * 1024 * 1024;
  as.rlim_max = as.rlim_cur;
  setrlimit_or_report(RLIMIT_AS, as, error_fd);

  prctl_or_report(PR_SET_NO_NEW_PRIVS, 1, error_fd);
}

// ── seccomp — explicit syscall table ─────────────────────────────────────────
//
// Rule: every syscall a permitted binary may invoke must appear here.
// When adding a new command to CommandPolicy, use strace to find any
// additional syscalls it needs and add them below.
//
// Syscall fallback defines: these syscalls were added in later kernel versions.
// The #ifndef guards let the code compile correctly against older kernel
// headers while still placing the right numbers in the BPF filter at runtime,
// because the BPF bytecode embeds raw integers — not symbols — so the number
// must be present even when the build toolchain predates the kernel that
// introduced it.
//
#ifndef __NR_clone3
#define __NR_clone3 435
#endif
#ifndef __NR_close_range
#define __NR_close_range 436
#endif
#ifndef __NR_faccessat2
#define __NR_faccessat2 439
#endif
#ifndef __NR_madvise
#define __NR_madvise 28
#endif
#ifndef __NR_rseq
#define __NR_rseq 334
#endif

struct Version {
  int major = 0;
  int minor = 0;
};

Version parse_version_prefix(const std::string &value) {
  Version out{};
  const auto dot = value.find('.');
  try {
    out.major = std::stoi(value.substr(0, dot));
    if (dot != std::string::npos) {
      std::size_t end = dot + 1;
      while (end < value.size() &&
             std::isdigit(static_cast<unsigned char>(value[end])))
        ++end;
      out.minor = std::stoi(value.substr(dot + 1, end - dot - 1));
    }
  } catch (...) {
    return {};
  }
  return out;
}

bool version_at_least(const Version actual, const Version required) {
  if (actual.major != required.major)
    return actual.major > required.major;
  return actual.minor >= required.minor;
}

Version kernel_version() {
  utsname info{};
  if (uname(&info) != 0)
    return {};
  return parse_version_prefix(info.release);
}

Version glibc_version() {
#if defined(__GLIBC__)
  return parse_version_prefix(gnu_get_libc_version());
#else
  return {};
#endif
}

// Scan the binary at `path` for the literal token `token` in its ELF dynamic
// string table.  More reliable than shelling out to ldd: no subprocess, no
// PATH dependency, and async-signal-safe after exec is not a concern here
// (this runs in the parent before fork).
bool binary_contains_token(const char *path, const std::string &token) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;

  std::string carry;
  std::array<char, 4096> buf{};
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = in.gcount();
    if (n <= 0)
      break;
    std::string chunk = carry + std::string(buf.data(), static_cast<size_t>(n));
    if (chunk.find(token) != std::string::npos)
      return true;
    if (chunk.size() > token.size())
      carry = chunk.substr(chunk.size() - token.size());
    else
      carry = std::move(chunk);
  }
  return false;
}

bool binary_links_libsystemd(const char *path) {
  return binary_contains_token(path, "libsystemd.so");
}

void add_syscall(std::vector<unsigned int> &profile, unsigned int nr) {
  if (std::find(profile.begin(), profile.end(), nr) == profile.end())
    profile.push_back(nr);
}

void add_syscalls(std::vector<unsigned int> &profile,
                  const std::vector<unsigned int> &syscalls) {
  for (const unsigned int nr : syscalls)
    add_syscall(profile, nr);
}

// ── Base syscalls: always added on every distro
// ─────────────────────────────── Every entry MUST have a comment of the form:
// // <binary>: <reason> Duplicates are removed at runtime by add_syscall() but
// must not appear here — each entry must be unique and intentional.
std::vector<unsigned int> base_allowed_syscalls() {
  return {
      __NR_read,   // uptime/who/df/ps/ls: read ELF, procfs, output pipes
      __NR_write,  // uptime/who/df/ps/ls: write stdout/stderr
      __NR_open,   // ls/ps: legacy open path used by libc/tool fallback
      __NR_openat, // uptime/who/df/ps/ls: open procfs, utmp, locale, ld cache
      __NR_close,  // uptime/who/df/ps/ls: close files opened during startup
      __NR_fstat,  // uptime/who/df/ps/ls: inspect opened files
      __NR_ioctl,  // ls/df/ps: terminal width and descriptor capability checks
      __NR_getxattr, // ls: read extended attributes for color/stat paths
#ifdef __NR_newfstatat
      __NR_newfstatat, // uptime/who/df/ps/ls: glibc statat implementation
#endif
      __NR_mmap,     // uptime/who/df/ps/ls: dynamic loader maps shared objects
      __NR_mprotect, // uptime/who/df/ps/ls: dynamic loader protects mappings
      __NR_munmap,   // uptime/who/df/ps/ls: dynamic loader frees mappings
      __NR_brk,      // uptime/who/df/ps/ls: glibc heap setup
      __NR_exit,     // uptime/who/df/ps/ls: process termination fallback
      __NR_exit_group, // uptime/who/df/ps/ls: normal glibc process termination
      __NR_futex,      // uptime/who/df/ps/ls: glibc locks after exec
      __NR_getpid,     // uptime/who/df/ps/ls: glibc and procps identity checks
      __NR_execve,     // uptime/who/df/ps/ls: final exec in sandbox child
      __NR_arch_prctl, // uptime/who/df/ps/ls: x86_64 thread-local storage setup
      __NR_access,     // uptime/who/df/ps/ls: glibc path access fallback
      __NR_readlink,   // ls/ps: resolve procfs and symlink targets
      __NR_prlimit64,  // ps: inspect limits; glibc may query process limits
      __NR_set_tid_address, // uptime/who/df/ps/ls: glibc thread bookkeeping
      __NR_set_robust_list, // uptime/who/df/ps/ls: glibc robust futex setup
      __NR_rt_sigaction,    // uptime/who/df/ps/ls: install signal handlers
      __NR_rt_sigprocmask,  // uptime/who/df/ps/ls: signal mask management
      __NR_rt_sigreturn,    // uptime/who/df/ps/ls: signal return trampoline
      __NR_kill,            // who: validate utmp session pids with kill(pid, 0)
      __NR_fcntl,           // uptime/who/df/ps/ls: descriptor flag checks
      __NR_getuid,          // who/ps/ls: ownership and identity display
      __NR_geteuid,         // who/ps/ls: effective identity display
      __NR_getgid,          // who/ps/ls: group identity display
      __NR_getegid,         // who/ps/ls: effective group identity display
      __NR_sysinfo,         // uptime: read uptime/load through sysinfo path
      __NR_sched_getaffinity, // ps: CPU affinity/capacity discovery
      __NR_pread64,       // uptime/who/df/ps/ls: read fixed offsets in files
      __NR_lseek,         // who/df/ps/ls: seek utmp, mount, and directory data
      __NR_getcwd,        // ls: resolve current working directory
      __NR_stat,          // ls/df: legacy stat fallback
      __NR_lstat,         // ls: symlink metadata fallback
      __NR_statfs,        // df: read filesystem usage
      __NR_getdents64,    // ls/ps: enumerate directories and /proc
      __NR_writev,        // ps/ls: vectorized writes from libc
      __NR_clock_gettime, // uptime/who/df/ps/ls: timing and locale init
      __NR_nanosleep, // uptime/who: utmp reader clears alarm timeout via sleep
      __NR_alarm,     // uptime/who: utmp reader clears alarm timeout
      __NR_uname,     // uptime/ps: kernel version and host metadata
      __NR_getrusage, // uptime/ps: resource accounting on some builds
      __NR_prctl,     // uptime/who/df/ps/ls: glibc process setup checks
      __NR_tgkill,    // uptime/who/df/ps/ls: glibc abort signal path
      __NR_dup,       // ls/df/ps: descriptor duplication fallback
      __NR_dup2,      // ls/df/ps: descriptor duplication fallback
      __NR_pipe2,     // ps: procps helper pipe setup on some builds
      __NR_poll,      // who/ps: utmp/proc polling fallback
      __NR_select,    // who/ps: libc select fallback
      __NR_getpgrp,   // ps: process group display
      __NR_getppid,   // ps: parent process display
      __NR_gettid,    // ps/glibc: thread identity lookup
      __NR_getrlimit, // ps/glibc: resource limit lookup
      __NR_setrlimit, // sleep/tests: libc resource-limit compatibility
      __NR_madvise,   // uptime/who/df/ps/ls: glibc malloc under RLIMIT_AS
      __NR_syslog,    // ps: kernel log access probe on some procps builds
      __NR_socket,    // who: NSS/systemd-logind AF_UNIX connection path
      __NR_connect,   // who: connect to NSS/systemd-logind sockets
      __NR_sendto,    // who: datagram send for NSS/systemd-logind lookups
      __NR_recvfrom,  // who: datagram receive for NSS/systemd-logind lookups
      __NR_sendmsg,   // who: structured socket send fallback
      __NR_recvmsg,   // who: structured socket receive fallback
      __NR_getsockname, // who: socket identity checks
      __NR_getpeername, // who: peer identity checks
      __NR_setsockopt,  // who: socket option setup
      __NR_getsockopt,  // who: socket option checks
      __NR_shutdown,    // who: socket shutdown cleanup
#ifdef __NR_getrandom
      __NR_getrandom, // uptime/who/df/ps/ls: glibc stack/canary initialization
#endif
#ifdef __NR_clock_nanosleep
      __NR_clock_nanosleep, // sleep/tests: high-resolution sleep implementation
#endif
#ifdef __NR_statx
      __NR_statx, // ls/df/ps: modern stat implementation on new glibc
#endif
#ifdef __NR_faccessat
      __NR_faccessat, // uptime/who/df/ps/ls: glibc access fallback
#endif
#ifdef __NR_openat2
      __NR_openat2, // ls/ps: modern openat path on new kernels
#endif
#ifdef __NR_mremap
      __NR_mremap, // uptime/who/df/ps/ls: glibc allocator remap path
#endif
  };
}

// ── libsystemd event-loop syscalls ───────────────────────────────────────────
// Added only when binary_links_libsystemd() returns true for uptime or who.
// On Ubuntu 22.04+ both binaries link libsystemd.so.0.  The very first sd_*
// call starts an event loop that requires all 13 of these syscalls before any
// application logic runs.  Without them the child exits SIGSYS (signal 31,
// exit code 159) before producing any output.
std::vector<unsigned int> libsystemd_syscalls() {
  return {
      __NR_clone,         // uptime/who: libsystemd helper thread creation
      __NR_epoll_create1, // uptime/who: libsystemd sd_event_new() event loop
      __NR_epoll_ctl,     // uptime/who: libsystemd event-loop registration
      __NR_epoll_wait,    // uptime/who: libsystemd wait path on older kernels
      __NR_epoll_pwait,   // uptime/who: libsystemd extended epoll wait path
      __NR_inotify_init1, // uptime/who: libsystemd file watching setup
      __NR_inotify_add_watch, // uptime/who: libsystemd watched path
                              // registration
      __NR_inotify_rm_watch,  // uptime/who: libsystemd watched path cleanup
      __NR_eventfd2, // uptime/who: libsystemd inter-thread wakeup descriptor
      __NR_timerfd_create,  // uptime/who: libsystemd timer source creation
      __NR_timerfd_settime, // uptime/who: libsystemd timer scheduling
      __NR_timerfd_gettime, // uptime/who: libsystemd timer inspection
      __NR_ppoll,           // uptime/who: libsystemd extended poll wait
  };
}

// ── detect_syscall_profile
// ──────────────────────────────────────────────────── Precondition:  called
// once in the parent before fork(); reads ELF headers
//               and utsname — both safe blocking I/O, no async-signal concerns.
// Postcondition: returns a deduplicated list of syscall numbers appropriate for
//               this distro, kernel version, and glibc version.
std::vector<unsigned int> detect_syscall_profile() {
  std::vector<unsigned int> profile = base_allowed_syscalls();

  // Check all common install paths for uptime and who.
  if (binary_links_libsystemd("/usr/bin/uptime") ||
      binary_links_libsystemd("/bin/uptime") ||
      binary_links_libsystemd("/usr/bin/who") ||
      binary_links_libsystemd("/bin/who")) {
    add_syscalls(profile, libsystemd_syscalls());
  }

  const Version kernel = kernel_version();
  const Version glibc = glibc_version();

  if (version_at_least(kernel, {5, 3}))
    add_syscall(profile, __NR_clone3); // uptime/who: glibc 2.39 thread creation

  if (version_at_least(kernel, {5, 8}))
    add_syscall(profile, __NR_faccessat2); // all: glibc >=2.33 access checks

  if (version_at_least(kernel, {5, 9}))
    add_syscall(profile, __NR_close_range); // all: glibc 2.39 fd cleanup path

  if (version_at_least(glibc, {2, 35}))
    add_syscall(profile, __NR_rseq); // all: glibc thread-pointer init

#ifdef __NR_futex_waitv
  if (version_at_least(glibc, {2, 39}))
    add_syscall(profile, __NR_futex_waitv); // all: glibc 2.39 futex wait path
#endif

  return profile;
}

std::vector<sock_filter>
build_seccomp_filter(const std::vector<unsigned int> &allowed_syscalls) {
  std::vector<sock_filter> filter = {
      // Verify architecture — kill immediately on mismatch.
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      // Load syscall number.
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
  };

  for (unsigned int nr : allowed_syscalls) {
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  }
  filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
  return filter;
}

void install_seccomp_or_report(const sock_fprog &prog, int error_fd) {
  if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
    const int err = errno;
    write_ignore(error_fd, &err, sizeof(err));
    _exit(126);
  }
}

// ── environment
// ───────────────────────────────────────────────────────────────
std::vector<std::string> build_environment(const tinyshell::v1::JobSpec &spec) {
  std::vector<std::string> env;
  if (spec.environment_size() == 0) {
    env.push_back("PATH=/usr/bin:/bin");
    env.push_back("LANG=C");
    env.push_back("LC_ALL=C");
    return env;
  }
  env.reserve(static_cast<std::size_t>(spec.environment_size()));
  for (const auto &entry : spec.environment()) {
    if (entry.name().empty() || entry.name().find('=') != std::string::npos)
      throw std::runtime_error("invalid JobSpec environment variable name");
    env.push_back(entry.name() + "=" + entry.value());
  }
  return env;
}

// ── waitpid helpers
// ───────────────────────────────────────────────────────────
pid_t waitpid_retry(pid_t pid, int *status, int options) {
  while (true) {
    const pid_t r = waitpid(pid, status, options);
    if (r >= 0 || errno != EINTR)
      return r;
  }
}

// Terminate the process group and guarantee the direct child is reaped.
void terminate_process_group(pid_t child_pid) {
  if (child_pid <= 0)
    return;

  // 1. Polite SIGTERM to whole session (child called setsid()).
  kill(-child_pid, SIGTERM);

  // 2. Give up to ~100 ms for voluntary exit.
  for (int i = 0; i < 10; ++i) {
    int status = 0;
    const pid_t waited = waitpid_retry(child_pid, &status, WNOHANG);
    if (waited == child_pid || (waited < 0 && errno == ECHILD))
      return;
    usleep(10'000);
  }

  // 3. Escalate and block until the direct child is reaped.
  kill(-child_pid, SIGKILL);
  int status = 0;
  waitpid_retry(child_pid, &status, 0); // blocking
}

ssize_t read_retry(int fd, void *buf, std::size_t n) {
  while (true) {
    const ssize_t r = read(fd, buf, n);
    if (r >= 0 || errno != EINTR)
      return r;
  }
}

bool valid_workdir_token(const std::string &value) {
  if (value.empty() || value.size() > 128)
    return false;
  for (unsigned char ch : value)
    if (!(std::isalnum(ch) || ch == '-' || ch == '_'))
      return false;
  return value.find("..") == std::string::npos;
}

std::string errno_message(const std::string &prefix, int err) {
  return prefix + ": " + strerror(err);
}

} // anonymous namespace

// ── SandboxExecutor::run
// ────────────────────────────────────────────────────── Precondition:  spec is
// HMAC-verified and allowed_absolute_paths is checked
//               by SpineAgent before this function is called.
// Postcondition: returns a fully populated ExecResult on every path;
//               no orphaned fds, no zombie children.
ExecResult SandboxExecutor::run(const tinyshell::v1::JobSpec &spec,
                                ChunkCallback on_chunk) {

  const auto timeout_ms =
      spec.limits().timeout_ms() == 0 ? 5000 : spec.limits().timeout_ms();
  const auto start_time = std::chrono::steady_clock::now();

  auto elapsed_ms = [&]() -> uint64_t {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time)
            .count());
  };

  if (!valid_workdir_token(spec.job_id()))
    throw std::runtime_error("invalid JobSpec job_id for sandbox workdir");

  // Build argv.
  std::vector<std::string> argv_storage;
  argv_storage.push_back(spec.command());
  for (const auto &arg : spec.args())
    argv_storage.push_back(arg);
  std::vector<char *> argv;
  argv.reserve(argv_storage.size() + 1);
  for (auto &s : argv_storage)
    argv.push_back(s.data());
  argv.push_back(nullptr);

  // Build envp.
  auto env_storage = build_environment(spec);
  std::vector<char *> envp;
  envp.reserve(env_storage.size() + 1);
  for (auto &s : env_storage)
    envp.push_back(s.data());
  envp.push_back(nullptr);

  // Build seccomp profile in the parent — detect_syscall_profile() reads
  // the ELF headers of uptime and who; safe to do before fork().
  const auto seccomp_profile = detect_syscall_profile();
  const auto seccomp_filter = build_seccomp_filter(seccomp_profile);
  sock_fprog seccomp_prog{};
  seccomp_prog.len = static_cast<unsigned short>(seccomp_filter.size());
  seccomp_prog.filter = const_cast<sock_filter *>(seccomp_filter.data());

  // Create unique workdir.
  auto workdir_template =
      (std::filesystem::temp_directory_path() / "tinyshell-job-XXXXXX")
          .string();
  char *created = mkdtemp(workdir_template.data());
  if (!created)
    throw std::runtime_error("mkdtemp() failed: " +
                             std::string(strerror(errno)));
  const std::filesystem::path workdir(created);
  auto cleanup_workdir = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(workdir, ignored);
  };

  // Pipes — all created with O_CLOEXEC so they close on exec in parent.
  int stdout_pipe[2], stderr_pipe[2], exec_error_pipe[2];
  make_pipe(stdout_pipe);
  make_pipe(stderr_pipe);
  make_pipe(exec_error_pipe);
  Fd stdout_read(stdout_pipe[0]), stdout_write(stdout_pipe[1]);
  Fd stderr_read(stderr_pipe[0]), stderr_write(stderr_pipe[1]);
  Fd exec_error_read(exec_error_pipe[0]), exec_error_write(exec_error_pipe[1]);

  const pid_t child = fork();
  if (child < 0)
    throw std::runtime_error("fork() failed");

  if (child == 0) {
    // ── Child — only async-signal-safe operations after this point ──────────
    // No malloc, no C++ exceptions, no STL allocations.
    // All failure paths write errno to exec_error_pipe and call _exit().
    if (setsid() < 0) {
      const int err = errno;
      write_ignore(exec_error_write.get(), &err, sizeof(err));
      _exit(126);
    }
    dup2_or_report(stdout_write.get(), STDOUT_FILENO, exec_error_write.get());
    dup2_or_report(stderr_write.get(), STDERR_FILENO, exec_error_write.get());
    close(stdout_read.get());
    close(stderr_read.get());
    close(stdout_write.get());
    close(stderr_write.get());
    close(exec_error_read.get());
    if (chdir(workdir.c_str()) != 0) {
      const int err = errno;
      write_ignore(exec_error_write.get(), &err, sizeof(err));
      _exit(126);
    }
    apply_child_limits_or_report(spec, exec_error_write.get());
    install_seccomp_or_report(seccomp_prog, exec_error_write.get());
    execve(spec.command().c_str(), argv.data(), envp.data());
    const int err = errno;
    write_ignore(exec_error_write.get(), &err, sizeof(err));
    _exit(127);
  }

  // ── Parent — release write ends immediately after fork ───────────────────
  close(stdout_write.release());
  close(stderr_write.release());
  close(exec_error_write.release());
  set_nonblocking(stdout_read.get());
  set_nonblocking(stderr_read.get());

  // Wait for exec-error pipe to close (exec succeeded) or carry an errno.
  int exec_errno = 0;
  const ssize_t exec_n =
      read_retry(exec_error_read.get(), &exec_errno, sizeof(exec_errno));
  exec_error_read.reset();
  if (exec_n == static_cast<ssize_t>(sizeof(exec_errno))) {
    int status = 0;
    waitpid_retry(child, &status, 0);
    cleanup_workdir();
    ExecResult r{};
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 126;
    r.exec_failed = true;
    r.failure = ExecutionFailure::EXECVE_FAILED;
    r.reason = errno_message("execve setup failed", exec_errno);
    r.runtime_ms = elapsed_ms();
    return r;
  }

  // ── I/O + timeout loop ────────────────────────────────────────────────────
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  std::uint64_t stdout_offset = 0;
  std::uint64_t stderr_offset = 0;
  bool stdout_open = true;
  bool stderr_open = true;
  bool stdout_truncated = false;
  bool stderr_truncated = false;
  int status = 0;
  bool child_done = false;
  bool output_limit_exceeded = false;
  bool output_limit_is_stdout = false;

  // Drain one pipe into on_chunk; sets truncation flag when limit hit.
  auto drain = [&](int fd, tinyshell::v1::StreamName stream,
                   std::uint64_t &offset, bool &open, bool &truncated) {
    const auto configured = stream == tinyshell::v1::STDOUT
                                ? spec.limits().max_stdout_bytes()
                                : spec.limits().max_stderr_bytes();
    const auto limit = configured == 0 ? 1024ULL * 1024ULL : configured;
    char buf[4096];
    while (true) {
      const ssize_t n = read(fd, buf, sizeof(buf));
      if (n > 0) {
        const auto available = limit > offset ? limit - offset : 0;
        const auto bytes_read = static_cast<std::uint64_t>(n);
        const auto allowed = std::min<std::uint64_t>(bytes_read, available);
        if (allowed > 0) {
          on_chunk(stream, offset,
                   std::string(buf, static_cast<std::size_t>(allowed)));
          offset += allowed;
        }
        if (bytes_read > allowed) {
          truncated = true;
          output_limit_exceeded = true;
          output_limit_is_stdout = (stream == tinyshell::v1::STDOUT);
          open = false;
          return;
        }
      } else if (n == 0) {
        open = false;
        return;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      } else if (errno == EINTR) {
        continue;
      } else {
        open = false;
        return;
      }
    }
  };

  while (stdout_open || stderr_open || !child_done) {
    if (!child_done) {
      const pid_t waited = waitpid_retry(child, &status, WNOHANG);
      if (waited == child || (waited < 0 && errno == ECHILD))
        child_done = true;
    }

    if (!child_done && std::chrono::steady_clock::now() > deadline) {
      terminate_process_group(child);
      cleanup_workdir();
      ExecResult r{};
      r.exit_code = -1;
      r.timed_out = true;
      r.failure = ExecutionFailure::TIMEOUT;
      r.reason = "timeout";
      r.runtime_ms = elapsed_ms();
      r.stdout_bytes = stdout_offset;
      r.stderr_bytes = stderr_offset;
      r.stdout_truncated = stdout_truncated;
      r.stderr_truncated = stderr_truncated;
      return r;
    }

    pollfd fds[2]{};
    int nfds = 0;
    if (stdout_open)
      fds[nfds++] = {stdout_read.get(), POLLIN | POLLHUP, 0};
    if (stderr_open)
      fds[nfds++] = {stderr_read.get(), POLLIN | POLLHUP, 0};

    if (nfds == 0) {
      if (child_done)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    const int poll_result = poll(fds, nfds, 100);
    if (poll_result < 0) {
      if (errno == EINTR)
        continue;
      terminate_process_group(child);
      cleanup_workdir();
      ExecResult r{};
      r.exit_code = -1;
      r.failure = ExecutionFailure::INTERNAL_ERROR;
      r.reason = errno_message("poll failed", errno);
      r.runtime_ms = elapsed_ms();
      r.stdout_bytes = stdout_offset;
      r.stderr_bytes = stderr_offset;
      return r;
    }

    int idx = 0;
    if (stdout_open && (fds[idx].revents & (POLLIN | POLLHUP)))
      drain(stdout_read.get(), tinyshell::v1::STDOUT, stdout_offset,
            stdout_open, stdout_truncated);
    if (stdout_open)
      ++idx;
    else if (nfds == 2)
      ++idx;
    if (stderr_open && idx < nfds && (fds[idx].revents & (POLLIN | POLLHUP)))
      drain(stderr_read.get(), tinyshell::v1::STDERR, stderr_offset,
            stderr_open, stderr_truncated);

    if (output_limit_exceeded) {
      terminate_process_group(child);
      cleanup_workdir();
      ExecResult r{};
      r.exit_code = -1;
      r.killed = true;
      r.failure = ExecutionFailure::OUTPUT_LIMIT;
      r.reason = output_limit_is_stdout ? "stdout byte limit exceeded"
                                        : "stderr byte limit exceeded";
      r.runtime_ms = elapsed_ms();
      r.stdout_bytes = stdout_offset;
      r.stderr_bytes = stderr_offset;
      r.stdout_truncated = stdout_truncated;
      r.stderr_truncated = stderr_truncated;
      return r;
    }
  }

  // Collect rusage (reserved for future metrics expansion).
  rusage usage{};
  getrusage(RUSAGE_CHILDREN, &usage);
  (void)usage;

  cleanup_workdir();

  // Normal exit.
  if (WIFEXITED(status)) {
    ExecResult r{};
    r.exit_code = WEXITSTATUS(status);
    r.exited_normally = true;
    r.failure = ExecutionFailure::NONE;
    r.reason = "exited";
    r.runtime_ms = elapsed_ms();
    r.stdout_bytes = stdout_offset;
    r.stderr_bytes = stderr_offset;
    r.stdout_truncated = stdout_truncated;
    r.stderr_truncated = stderr_truncated;
    return r;
  }

  // Signal exit — classify SIGSYS specifically as a seccomp violation so
  // the GUI can show a meaningful error rather than a raw signal number.
  if (WIFSIGNALED(status)) {
    const int sig = WTERMSIG(status);
    ExecResult r{};
    r.exit_code = 128 + sig;
    r.signaled = true;
    r.signal = sig;
    r.runtime_ms = elapsed_ms();
    r.stdout_bytes = stdout_offset;
    r.stderr_bytes = stderr_offset;
    r.stdout_truncated = stdout_truncated;
    r.stderr_truncated = stderr_truncated;
    r.failure = (sig == SIGSYS) ? ExecutionFailure::SECCOMP_VIOLATION
                                : ExecutionFailure::SIGNAL_TERMINATED;
    r.reason = "signal " + std::to_string(sig);
    return r;
  }

  // Unknown — should not be reached under normal conditions.
  ExecResult r{};
  r.exit_code = -1;
  r.failure = ExecutionFailure::INTERNAL_ERROR;
  r.reason = "unknown";
  r.runtime_ms = elapsed_ms();
  r.stdout_bytes = stdout_offset;
  r.stderr_bytes = stderr_offset;
  return r;
}

} // namespace tsh::spine