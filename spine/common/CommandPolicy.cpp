#include "CommandPolicy.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace tsh::spine {
namespace {

bool contains_shell_syntax(const std::string &value) {
  static constexpr char kDenied[] = "|&;<>()`$\\\n\r";
  return value.find_first_of(kDenied) != std::string::npos;
}

bool is_safe_atom(const std::string &value) {
  if (value.empty() || value.size() > 256 || contains_shell_syntax(value)) {
    return false;
  }
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isprint(ch) != 0; });
}

// Resolve the absolute path for a bare command name, preferring /usr/bin then
// /bin.  Returns an empty string if neither path exists on the host.
std::string resolve_absolute(const std::string &name) {
  for (const char *prefix : {"/usr/bin/", "/bin/"}) {
    std::string candidate = prefix + name;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && !ec)
      return candidate;
  }
  return {};
}

struct AllowedCommand {
  const char *name; // bare name, e.g. "uptime"
  bool args_ok;     // whether arguments are permitted at all
};

// Phase 1 command table.  Add rows here to extend policy; no other code
// changes required.  The executor's seccomp allowlist must also permit any
// additional syscalls the new binary requires.
static const AllowedCommand kPhase1[] = {
    {"uptime", false}, {"who", false},
    {"df", true}, // df accepts a path argument
    {"ps", true}, // ps accepts flag arguments
    {"ls", true}, // ls accepts path and flag arguments
};

} // namespace

ValidatedCommand
CommandPolicy::validate(const std::string &command,
                        const std::vector<std::string> &args) const {
  if (!is_safe_atom(command)) {
    throw PolicyError("command contains denied characters");
  }
  for (const auto &arg : args) {
    if (!is_safe_atom(arg)) {
      throw PolicyError("argument contains denied characters");
    }
  }

  // Accept both bare names ("uptime") and absolute paths ("/usr/bin/uptime",
  // "/bin/uptime").  In all cases the returned absolute_path is the canonical
  // host path so the agent's execve() call is unambiguous.
  for (const auto &entry : kPhase1) {
    const std::string bare = entry.name;

    // Match bare name, /usr/bin/<name>, or /bin/<name>.
    bool matched = (command == bare) || (command == "/usr/bin/" + bare) ||
                   (command == "/bin/" + bare);
    if (!matched)
      continue;

    if (!entry.args_ok && !args.empty()) {
      throw PolicyError(bare + " arguments are not enabled in phase 1");
    }

    const std::string abs = resolve_absolute(bare);
    if (abs.empty()) {
      throw PolicyError("command binary not found on this host: " + bare);
    }
    return {abs, args};
  }

  throw PolicyError("command is not allowed by phase 1 execution policy: " +
                    command);
}

} // namespace tsh::spine