/// @file
/// utilities/process -- running a child program and reading what the child
/// wrote.
///
/// CanMehHarness, CanMehSearch and CanMehCalibrate all run child processes and
/// read the files those children wrote, so all three need one process runner
/// that reports how a child ended rather than one integer, and one reader for
/// the single JSON object a CanMehHarness subcommand prints.
#pragma once

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace utilities {

inline std::string readFile(const std::filesystem::path& path) {
  auto file = std::ifstream{path, std::ios::binary};
  auto contents = std::ostringstream{};
  contents << file.rdbuf();
  return contents.str();
}

inline void writeFile(const std::filesystem::path& path, const std::string& text) {
  auto file = std::ofstream{path, std::ios::binary};
  file << text;
}

struct ProcessResult {
  enum class Ending { Exited, Signaled, TimedOut, SpawnFailed, ChildSetupFailed };

  Ending ending = Ending::Exited;

  // Exit status when Exited, signal number when Signaled, 125 or 126 when
  // ChildSetupFailed, and 0 when TimedOut or SpawnFailed.
  int code = 0;

  bool ok() const { return ending == Ending::Exited && code == 0; }

  std::string describe() const {
    switch (ending) {
      case Ending::Exited:
        return "exited with status " + std::to_string(code);
      case Ending::Signaled: {
        auto what = std::string{"died on signal " + std::to_string(code)};
        if (code == SIGSEGV) what += " (invalid memory access)";
        if (code == SIGBUS) what += " (misaligned or unmapped access)";
        if (code == SIGABRT)
          what += " (abort, which std::terminate raises for an uncaught exception)";
        return what;
      }
      case Ending::TimedOut:
        return "timed out";
      case Ending::SpawnFailed:
        return "could not be started";
      case Ending::ChildSetupFailed:        return code == 126 ? "could not open its output files"
                           : "could not change to its working directory";
    }
    return "ended in an unknown way";
  }
};

/// Forks, redirects the child's stdout and stderr, changes the child's
/// directory when cwd is not empty, and execs argv.
///
/// A caller that must watch the child while the child runs needs the child's
/// process id before the child finishes.
/// @return the child's process id, or -1 when fork failed.
inline pid_t spawnProcess(const std::vector<std::string>& argv,
                           const std::filesystem::path& outPath,
                           const std::filesystem::path& errPath,
                           const std::filesystem::path& cwd = "") {
  // Flush before forking. The child inherits whatever the parent's stdout
  // buffer holds, and the child's freopen closes that stream first, which
  // writes the inherited bytes to the parent's original stdout. Every line the
  // parent printed and did not flush then appears a second time.
  std::cout.flush();
  std::fflush(nullptr);
  const auto pid = pid_t{fork()};
  if (pid < 0) return -1;
  if (pid == 0) {
    // Redirect before changing directory. The output paths may be relative to
    // the directory the parent process ran in, and chdir first would leave those paths
    // unresolvable: the child then dies at freopen with no diagnostics, and
    // the caller sees a command that failed for no visible reason.
    if (!freopen(outPath.c_str(), "w", stdout)) _exit(126);
    if (!freopen(errPath.c_str(), "w", stderr)) _exit(126);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(125);
    auto arguments = std::vector<char*>{};
    arguments.reserve(argv.size() + 1);
    for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);
    execvp(arguments[0], arguments.data());
    _exit(127);
  }
  return pid;
}

/// waitForProcess polls for the child spawnProcess started and reports how that
/// child ended.
inline ProcessResult waitForProcess(pid_t pid, double timeoutSeconds) {
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    auto status = int{0};
    const auto reaped = pid_t{waitpid(pid, &status, WNOHANG)};
    if (reaped == pid) {
      if (WIFSIGNALED(status)) return {ProcessResult::Ending::Signaled, WTERMSIG(status)};
      const auto code = int{WIFEXITED(status) ? WEXITSTATUS(status) : 0};
      // spawnProcess exits the child 126 when freopen failed and 125 when chdir
      // failed, so either status means the parent failed to set the child up
      // rather than the command choosing that status.
      if (code == 125 || code == 126) return {ProcessResult::Ending::ChildSetupFailed, code};
      return {ProcessResult::Ending::Exited, code};
    }
    const auto elapsed =
        double{std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
    if (elapsed > timeoutSeconds) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return {ProcessResult::Ending::TimedOut, 0};
    }    // waitForProcess polls every 3 ms for the first second and every 100 ms
    // afterwards. The parent of a benchmark process is itself the child of
    // another parent polling the same way, so a fixed 3 ms interval would wake
    // each of the two processes 333 times a second while Google Benchmark is
    // timing a decode. A caller that passes a timeout of a second or less sees
    // waitForProcess report that timeout up to 100 ms late.
    usleep(elapsed < 1.0 ? 3000 : 100000);
  }
}

inline ProcessResult runProcess(const std::vector<std::string>& argv,
                                 const std::filesystem::path& outPath,
                                 const std::filesystem::path& errPath, double timeoutSeconds,
                                 const std::filesystem::path& cwd = "") {
  const auto pid = spawnProcess(argv, outPath, errPath, cwd);
  if (pid < 0) return {ProcessResult::Ending::SpawnFailed, 0};
  return waitForProcess(pid, timeoutSeconds);
}

/// readJson passes false for nlohmann::json::parse's allow_exceptions, so an
/// unreadable or malformed file comes back as a discarded value a caller can
/// name in its own failure JSON rather than catching an exception.
inline nlohmann::json readJson(const std::filesystem::path& path) {
  return nlohmann::json::parse(readFile(path), nullptr, false);
}

/// Shortens text to at most limit bytes, cutting only at a UTF-8 character
/// boundary.
///
/// The boundary matters because nlohmann::json::dump validates UTF-8 and throws,
/// and the callers truncate compiler output and model replies, both of which
/// carry multi-byte characters: g++ quotes an identifier with U+2018 and U+2019,
/// three bytes each. A 40-generation cluster run died at generation 12 with
/// `type_error.316 invalid UTF-8 byte at index 400: 0x0A` because bytes 398 to 400
/// of that generation's compile error were the e2 80 98 of a quote, substr kept
/// e2 80 and the appended marker put 0x0A where the third byte belonged. Four
/// earlier generations in the same run carried multi-byte quotes and survived only
/// because the cut happened to fall between characters.
inline std::string truncate(const std::string& text, std::size_t limit) {
  if (text.size() <= limit) return text;
  // 10xxxxxx is a continuation byte, so while the cut lands on one it is inside a
  // character and has to move back to that character's first byte.
  auto at = std::size_t{limit};
  while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
  return text.substr(0, at) + "\n... [truncated]";
}

inline std::string fixedPoint(double value, int places) {
  auto text = std::ostringstream{};
  text << std::fixed << std::setprecision(places) << value;
  return text.str();
}

/// Runs program with args and returns the one JSON object program printed on
/// stdout. runForJsonReply writes the no-JSON failure once: a child that
/// printed no JSON object comes back as a rejection naming how that child
/// ended.
inline nlohmann::json runForJsonReply(const std::string& program,
                                       std::vector<std::string> args,
                                       const std::filesystem::path& outPath,
                                       const std::filesystem::path& errPath,
                                       double timeoutSeconds) {
  args.insert(args.begin(), program);
  const auto ran = ProcessResult{runProcess(args, outPath, errPath, timeoutSeconds)};
  auto printed = readJson(outPath);
  if (printed.is_discarded() || !printed.is_object())
    return nlohmann::json{
        {"ok", false},
        {"stage", std::filesystem::path{program}.filename().string()},
        {"error", std::filesystem::path{program}.filename().string() + " " + ran.describe() +
                      " and printed no JSON object: " + truncate(readFile(errPath), 400)}};
  return printed;
}

}  // namespace utilities
