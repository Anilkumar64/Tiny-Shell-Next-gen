#pragma once

#include "tinyshell/v1/spine.pb.h"

#include <string>

namespace tsh::spine {

const char *job_state_name(tinyshell::v1::JobState state);
tinyshell::v1::JobState job_state_from_name(const std::string &state);
tinyshell::v1::JobState state_after_event(tinyshell::v1::JobEventType type,
                                           tinyshell::v1::JobState current);
bool is_terminal_state(tinyshell::v1::JobState state);
bool is_valid_transition(tinyshell::v1::JobState from,
                         tinyshell::v1::JobState to);

} // namespace tsh::spine
