#pragma once
#include <string>
#include <vector>

namespace tsh {

struct ProcessRecord {
  int pid = 0;
  std::string name;
  double cpu_usage = 0.0;
  long mem_kb = 0;
  std::string state;
  int ppid = 0;
};

// ProcReader enumerates /proc/<pid>/stat and /proc/<pid>/status on Linux
// to build a snapshot of running processes.
class ProcReader {
public:
  static std::vector<ProcessRecord> read_all_processes();

private:
  static ProcessRecord read_process(int pid);
};

} // namespace tsh
