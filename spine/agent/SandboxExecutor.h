#pragma once

// SandboxExecutor.h — fixed
//
// Changes from audit:
//  BUG #6  — ExecResult was missing stdout_truncated, stderr_truncated,
//             and a typed ExecutionFailure member.  All three are added.

#include "tinyshell/v1/spine.pb.h"

#include <cstdint>
#include <functional>
#include <string>

namespace tsh::spine {

// Typed failure classification (Bug #3 / #6).
// Used in ExecResult and propagated to JobExit on the wire.
enum class ExecutionFailure {
  NONE,
  EXECVE_FAILED,
  TIMEOUT,
  SIGNAL_TERMINATED,
  OUTPUT_LIMIT,
  SECCOMP_VIOLATION,
  INTERNAL_ERROR
};

// Returns the string token written to JobExit.failure_class on the wire.
inline const char *failure_class_string(ExecutionFailure f) {
  switch (f) {
  case ExecutionFailure::NONE:
    return "NONE";
  case ExecutionFailure::EXECVE_FAILED:
    return "EXECVE_FAILED";
  case ExecutionFailure::TIMEOUT:
    return "TIMEOUT";
  case ExecutionFailure::SIGNAL_TERMINATED:
    return "SIGNAL_TERMINATED";
  case ExecutionFailure::OUTPUT_LIMIT:
    return "OUTPUT_LIMIT";
  case ExecutionFailure::SECCOMP_VIOLATION:
    return "SECCOMP_VIOLATION";
  case ExecutionFailure::INTERNAL_ERROR:
    return "INTERNAL_ERROR";
  }
  return "INTERNAL_ERROR";
}

struct ExecResult {
  int exit_code = -1;

  // Exit-path flags — exactly one should be true on a completed run.
  bool exited_normally = false; // WIFEXITED
  bool signaled = false;        // WIFSIGNALED
  bool timed_out = false;       // wall-clock deadline exceeded
  bool killed = false;          // output-limit kill
  bool exec_failed = false;     // execve(2) itself failed

  int signal = 0; // WTERMSIG value when signaled == true

  uint64_t runtime_ms = 0; // wall-clock ms from fork to reap

  // Stream accounting — populated for every exit path (Bug #11).
  uint64_t stdout_bytes = 0;
  uint64_t stderr_bytes = 0;
  bool stdout_truncated = false;
  bool stderr_truncated = false;

  // Typed failure class (Bug #3 / #6) and human-readable detail.
  ExecutionFailure failure = ExecutionFailure::NONE;
  std::string reason; // short English phrase for logs / proto
};

class SandboxExecutor {
public:
  using ChunkCallback = std::function<void(tinyshell::v1::StreamName,
                                           std::uint64_t, const std::string &)>;

  ExecResult run(const tinyshell::v1::JobSpec &spec, ChunkCallback on_chunk);
};

} // namespace tsh::spine