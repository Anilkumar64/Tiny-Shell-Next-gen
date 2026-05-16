#include "JobLifecycle.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <stdexcept>

namespace tsh::spine {
namespace {

using tinyshell::v1::JOB_STATE_ASSIGNED;
using tinyshell::v1::JOB_STATE_DELIVERED;
using tinyshell::v1::JOB_STATE_EXITED;
using tinyshell::v1::JOB_STATE_FAILED;
using tinyshell::v1::JOB_STATE_KILLED;
using tinyshell::v1::JOB_STATE_LOST;
using tinyshell::v1::JOB_STATE_PENDING;
using tinyshell::v1::JOB_STATE_REJECTED;
using tinyshell::v1::JOB_STATE_RUNNING;
using tinyshell::v1::JOB_STATE_STARTING;
using tinyshell::v1::JOB_STATE_STREAMING;
using tinyshell::v1::JOB_STATE_TIMED_OUT;
using tinyshell::v1::JobState;

bool one_of(JobState state, std::initializer_list<JobState> states) {
  return std::find(states.begin(), states.end(), state) != states.end();
}

std::string normalize(std::string state) {
  std::transform(
      state.begin(), state.end(), state.begin(),
      [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  if (state.rfind("JOB_STATE_", 0) == 0) {
    state.erase(0, 10);
  }
  return state;
}

} // namespace

const char *job_state_name(JobState state) {
  switch (state) {
  case JOB_STATE_PENDING:
    return "PENDING";
  case JOB_STATE_ASSIGNED:
    return "ASSIGNED";
  case JOB_STATE_DELIVERED:
    return "DELIVERED";
  case JOB_STATE_STARTING:
    return "STARTING";
  case JOB_STATE_RUNNING:
    return "RUNNING";
  case JOB_STATE_STREAMING:
    return "STREAMING";
  case JOB_STATE_EXITED:
    return "EXITED";
  case JOB_STATE_FAILED:
    return "FAILED";
  case JOB_STATE_TIMED_OUT:
    return "TIMED_OUT";
  case JOB_STATE_KILLED:
    return "KILLED";
  case JOB_STATE_LOST:
    return "LOST";
  case JOB_STATE_REJECTED:
    return "REJECTED";
  default:
    return "UNSPECIFIED";
  }
}

JobState job_state_from_name(const std::string &state) {
  const auto normalized = normalize(state);
  if (normalized == "PENDING") {
    return JOB_STATE_PENDING;
  }
  if (normalized == "ASSIGNED") {
    return JOB_STATE_ASSIGNED;
  }
  if (normalized == "DELIVERED") {
    return JOB_STATE_DELIVERED;
  }
  if (normalized == "STARTING") {
    return JOB_STATE_STARTING;
  }
  if (normalized == "RUNNING") {
    return JOB_STATE_RUNNING;
  }
  if (normalized == "STREAMING") {
    return JOB_STATE_STREAMING;
  }
  if (normalized == "EXITED" || normalized == "SUCCEEDED") {
    return JOB_STATE_EXITED;
  }
  if (normalized == "FAILED") {
    return JOB_STATE_FAILED;
  }
  if (normalized == "TIMED_OUT" || normalized == "TIMEOUT") {
    return JOB_STATE_TIMED_OUT;
  }
  if (normalized == "KILLED") {
    return JOB_STATE_KILLED;
  }
  if (normalized == "LOST") {
    return JOB_STATE_LOST;
  }
  if (normalized == "REJECTED") {
    return JOB_STATE_REJECTED;
  }
  return tinyshell::v1::JOB_STATE_UNSPECIFIED;
}

bool is_terminal_state(JobState state) {
  return one_of(state, {JOB_STATE_EXITED, JOB_STATE_FAILED, JOB_STATE_TIMED_OUT,
                        JOB_STATE_KILLED, JOB_STATE_LOST, JOB_STATE_REJECTED});
}

bool is_valid_transition(JobState from, JobState to) {
  if (to == tinyshell::v1::JOB_STATE_UNSPECIFIED) {
    return false;
  }
  if (from == tinyshell::v1::JOB_STATE_UNSPECIFIED || from == to) {
    return true;
  }
  if (is_terminal_state(from)) {
    return false;
  }
  switch (to) {
  case JOB_STATE_ASSIGNED:
    return from == JOB_STATE_PENDING;
  case JOB_STATE_DELIVERED:
    return from == JOB_STATE_ASSIGNED;
  case JOB_STATE_STARTING:
    return one_of(from, {JOB_STATE_ASSIGNED, JOB_STATE_DELIVERED});
  case JOB_STATE_RUNNING:
    return one_of(
        from, {JOB_STATE_ASSIGNED, JOB_STATE_DELIVERED, JOB_STATE_STARTING});
  case JOB_STATE_STREAMING:
    return one_of(from, {JOB_STATE_RUNNING, JOB_STATE_STREAMING});
  case JOB_STATE_EXITED:
  case JOB_STATE_FAILED:
  case JOB_STATE_TIMED_OUT:
  case JOB_STATE_KILLED:
  case JOB_STATE_LOST:
    return one_of(from,
                  {JOB_STATE_ASSIGNED, JOB_STATE_DELIVERED, JOB_STATE_STARTING,
                   JOB_STATE_RUNNING, JOB_STATE_STREAMING});
  case JOB_STATE_REJECTED:
    return one_of(from,
                  {JOB_STATE_PENDING, JOB_STATE_ASSIGNED, JOB_STATE_DELIVERED});
  default:
    return false;
  }
}

JobState state_after_event(tinyshell::v1::JobEventType type, JobState current) {
  switch (type) {
  case tinyshell::v1::JOB_CREATED:
  case tinyshell::v1::JOB_VALIDATED:
  case tinyshell::v1::JOB_SIGNED:
  case tinyshell::v1::JOB_SCHEDULED:
  case tinyshell::v1::AUDIT_RECORDED:
    return current == tinyshell::v1::JOB_STATE_UNSPECIFIED ? JOB_STATE_PENDING
                                                           : current;
  case tinyshell::v1::JOB_ASSIGNED:
    if (one_of(current,
               {tinyshell::v1::JOB_STATE_UNSPECIFIED, JOB_STATE_PENDING})) {
      return JOB_STATE_ASSIGNED;
    }
    return current;
  case tinyshell::v1::JOB_DELIVERED:
    if (current == JOB_STATE_ASSIGNED) {
      return JOB_STATE_DELIVERED;
    }
    return current;
  case tinyshell::v1::JOB_AGENT_ACCEPTED:
    if (one_of(current, {JOB_STATE_ASSIGNED, JOB_STATE_DELIVERED})) {
      return JOB_STATE_STARTING;
    }
    return current;
  case tinyshell::v1::JOB_STARTED:
    if (one_of(current,
               {JOB_STATE_ASSIGNED, JOB_STATE_DELIVERED, JOB_STATE_STARTING})) {
      return JOB_STATE_RUNNING;
    }
    return current;
  case tinyshell::v1::JOB_STDOUT:
  case tinyshell::v1::JOB_STDERR:
    if (one_of(current, {JOB_STATE_RUNNING, JOB_STATE_STREAMING})) {
      return JOB_STATE_STREAMING;
    }
    return current;
  case tinyshell::v1::JOB_EXITED:
    return JOB_STATE_EXITED;
  case tinyshell::v1::JOB_FAILED:
    return JOB_STATE_FAILED;
  case tinyshell::v1::JOB_TIMED_OUT:
    return JOB_STATE_TIMED_OUT;
  case tinyshell::v1::JOB_KILLED:
    return JOB_STATE_KILLED;
  case tinyshell::v1::JOB_LOST:
    return JOB_STATE_LOST;
  case tinyshell::v1::JOB_REJECTED:
    return JOB_STATE_REJECTED;
  default:
    return current;
  }
}

} // namespace tsh::spine
