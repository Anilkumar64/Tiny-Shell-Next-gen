#include "ProcReader.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace tsh {

std::vector<ProcessRecord> ProcReader::read_all_processes() {
  std::vector<ProcessRecord> processes;
  for (const auto &entry : std::filesystem::directory_iterator("/proc")) {
    const std::string pid_text = entry.path().filename().string();
    if (pid_text.empty())
      continue;
    bool numeric = true;
    for (unsigned char c : pid_text) {
      if (!std::isdigit(c)) {
        numeric = false;
        break;
      }
    }
    if (!numeric)
      continue;

    int pid = 0;
    try {
      pid = std::stoi(pid_text);
    } catch (...) {
      continue;
    }

    auto rec = read_process(pid);
    if (rec.pid > 0)
      processes.push_back(std::move(rec));
  }
  return processes;
}

ProcessRecord ProcReader::read_process(int pid) {
  ProcessRecord rec;
  rec.pid = pid;

  const std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream stat_file(stat_path);
  if (!stat_file.is_open())
    return {};

  std::string stat_line;
  std::getline(stat_file, stat_line);

  const auto lparen = stat_line.find('(');
  const auto rparen = stat_line.rfind(')');
  if (lparen == std::string::npos || rparen == std::string::npos)
    return {};

  rec.name = stat_line.substr(lparen + 1, rparen - lparen - 1);

  std::istringstream rest(stat_line.substr(rparen + 2));
  std::string state;
  int ppid = 0;
  rest >> state >> ppid;
  rec.state = state;
  rec.ppid = ppid;

  const std::string status_path = "/proc/" + std::to_string(pid) + "/status";
  std::ifstream status_file(status_path);
  if (status_file.is_open()) {
    std::string line;
    while (std::getline(status_file, line)) {
      if (line.rfind("VmRSS:", 0) == 0) {
        std::istringstream ss(line.substr(6));
        ss >> rec.mem_kb;
        break;
      }
    }
  }

  // Read utime and stime from stat and form a simple cpu_usage heuristic.
  {
    std::istringstream ss(stat_line.substr(rparen + 2));
    std::string tok;
    for (int i = 1; i <= 11; ++i) {
      if (!(ss >> tok))
        break;
    }
    long utime = 0, stime = 0;
    ss >> utime >> stime;
    const long total = utime + stime;
    rec.cpu_usage = static_cast<double>(total % 10000) / 100.0;
  }

  return rec;
}

} // namespace tsh