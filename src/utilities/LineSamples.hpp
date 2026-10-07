/// @file
/// utilities/LineSamples -- header-only, like the other three headers in this
/// directory. measureLineSamples runs one command under the
/// platform's profiler and counts the samples the profiler recorded against
/// each source line of one shared library. weighRanges sums those counts over
/// line ranges a caller supplies.
#pragma once

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "utilities/Process.hpp"

namespace utilities {

/// How many samples the profiler attributed to one source line. `file` is the
/// path the compiler recorded in the debug information, which is absolute for
/// a build the harness made.
struct LineSample {
  std::string file;
  int line = 0;
  long long weight = 0;
};

struct SamplingRequest {
  /// The process to sample, argv[0] first.
  std::vector<std::string> command;
  /// Samples whose frame names a different image are dropped. Matched as a
  /// suffix, so "libfit_sampling.dylib" is enough.
  std::string image;
  /// Where the sampler's report and the command's output are written.
  std::filesystem::path scratch;
  /// How long the sampler watches.
  double seconds = 10.0;
  /// How long to let the command run before sampling starts. A benchmark
  /// harness spends its first seconds measuring the machine rather than the
  /// workload, and samples taken then describe the startup.
  double settleSeconds = 3.0;
  /// Ends the command if it outlives this, so a decode that hangs cannot hang
  /// the caller.
  double timeoutSeconds = 600.0;
};

struct LineProfile {
  bool ok = false;
  std::string error;
  /// Which sampler produced this, for a caller that reports provenance.
  std::string tool;
  /// Samples the profiler took, over every image and every thread.
  long long total = 0;
  /// Of those, the ones whose frame named the requested image.
  long long inImage = 0;
  /// Of those, the ones the profiler could name a source line for.
  long long located = 0;
  std::vector<LineSample> samples;
};

/// What only measureLineSamples needs. These live in a named namespace rather
/// than an anonymous one, because an anonymous namespace in a header gives
/// every translation unit its own copies and the inline functions below would
/// then name different entities in each.
namespace lineSamplesDetail {

/// One row of the sampler's call graph.
struct Node {
  std::size_t depth = 0;
  long long inclusive = 0;
  long long self = 0;
  std::string image;
  std::string file;
  int line = 0;
};

/// Splits a call-graph row into its indentation, its count and the rest.
///
/// The sampler draws the tree with spaces and the guide characters + ! : |,
/// then the count, then the frame. The distance from the left margin to the
/// count is the only thing that says how deep the frame is.
inline bool splitRow(const std::string& row, Node* into, std::string* rest) {
  auto at = std::size_t{0};
  while (at < row.size() && (row[at] == ' ' || row[at] == '+' || row[at] == '!' ||
                             row[at] == ':' || row[at] == '|'))
    ++at;
  if (at >= row.size() || std::isdigit(static_cast<unsigned char>(row[at])) == 0) return false;
  into->depth = at;
  auto after = at;
  while (after < row.size() && std::isdigit(static_cast<unsigned char>(row[after])) != 0) ++after;
  into->inclusive = std::stoll(row.substr(at, after - at));
  into->self = into->inclusive;
  while (after < row.size() && row[after] == ' ') ++after;
  *rest = row.substr(after);
  return true;
}

/// The image a frame names, from its `(in NAME)` field.
inline std::string imageOf(const std::string& rest) {
  const auto opens = rest.find("(in ");
  if (opens == std::string::npos) return "";
  const auto closes = rest.find(')', opens);
  if (closes == std::string::npos) return "";
  return rest.substr(opens + 4, closes - opens - 4);
}

/// The source location a frame names, which the sampler prints after the
/// address in brackets and only when it found debug information.
inline void locationOf(const std::string& rest, std::string* file, int* line) {
  const auto afterAddress = rest.rfind("]  ");
  if (afterAddress == std::string::npos) return;
  const auto tail = rest.substr(afterAddress + 3);
  const auto colon = tail.rfind(':');
  if (colon == std::string::npos || colon + 1 >= tail.size()) return;
  const auto digits = tail.substr(colon + 1);
  if (!std::all_of(digits.begin(), digits.end(),
                   [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
    return;
  *file = tail.substr(0, colon);
  *line = std::stoi(digits);
}

/// Turns the sampler's call graph into per-line self counts.
///
/// The counts the sampler prints are inclusive: a frame's number counts every
/// sample taken anywhere below it. The samples a line took by itself are that
/// number minus the numbers of the frames it called, so nodesOf subtracts each
/// row from its parent. The sampler's other view, sorted by top of stack, does
/// report self counts, but it names only a symbol and an image with no source
/// line at all, which is why the tree is the view nodesOf reads.
inline std::vector<Node> nodesOf(const std::string& report) {
  auto nodes = std::vector<Node>{};
  auto parents = std::vector<std::pair<std::size_t, std::size_t>>{};
  auto lines = std::istringstream{report};
  auto row = std::string{};
  auto inGraph = false;

  while (std::getline(lines, row)) {
    if (row.rfind("Call graph:", 0) == 0) {
      inGraph = true;
      continue;
    }
    // The graph ends where the sampler's next section begins. Those headings
    // start at column zero, which no call-graph row does.
    if (inGraph && !row.empty() && row[0] != ' ') break;
    if (!inGraph) continue;

    auto node = Node{};
    auto rest = std::string{};
    if (!splitRow(row, &node, &rest)) continue;
    node.image = imageOf(rest);
    locationOf(rest, &node.file, &node.line);

    while (!parents.empty() && parents.back().first >= node.depth) parents.pop_back();
    if (!parents.empty()) nodes[parents.back().second].self -= node.inclusive;
    parents.emplace_back(node.depth, nodes.size());
    nodes.push_back(node);
  }
  return nodes;
}

#ifndef __APPLE__
/// Reads the flat report `perf report --no-children --sort srcline` prints,
/// whose rows already hold self counts, so no parent subtraction is needed:
///
///      5.43%           628  /abs/path/fit_profile.cpp:3060
///
/// Each row holds a percentage, a sample count and one source location.
/// --sort srcline prints no image name, so measureLineSamples cannot filter
/// these rows by image the way it does on macOS; the absolute path tells a
/// caller's interesting predicate what it needs instead.
inline std::vector<LineSample> samplesFromPerfReport(const std::string& report,
                                                     long long* total,
                                                     long long* located) {
  auto samples = std::vector<LineSample>{};
  auto lines = std::istringstream{report};
  auto row = std::string{};
  while (std::getline(lines, row)) {
    if (row.empty() || row[0] == '#') continue;
    auto fields = std::vector<std::string>{};
    auto reader = std::istringstream{row};
    auto field = std::string{};
    while (reader >> field) fields.push_back(field);
    if (fields.size() < 3 || fields.front().back() != '%') continue;

    auto count = 0LL;
    try {
      count = std::stoll(fields[1]);
    } catch (const std::exception&) {
      continue;
    }
    if (count <= 0) continue;
    *total += count;

    const auto where = fields.back();
    const auto colon = where.rfind(':');
    if (colon == std::string::npos || colon + 1 >= where.size()) continue;
    const auto digits = where.substr(colon + 1);
    if (!std::all_of(digits.begin(), digits.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
      continue;
    *located += count;
    samples.push_back({where.substr(0, colon), std::stoi(digits), count});
  }
  return samples;
}
#endif  // __APPLE__

inline bool endsWith(const std::string& text, const std::string& tail) {
  return text.size() >= tail.size() &&
         text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

}  // namespace lineSamplesDetail

/// Runs request.command under the platform's sampler and returns the samples
/// it attributed to source lines of request.image.
///
/// Never throws and never exits. A sampler that is absent, refused or silent
/// comes back with ok false and error naming the step that failed, so a caller
/// can carry on without a profile rather than losing a run.
inline LineProfile measureLineSamples(const SamplingRequest& request) {
  auto profile = LineProfile{};
#ifdef __APPLE__
  profile.tool = "sample";
#else
  profile.tool = "perf";
#endif
  if (request.command.empty()) {
    profile.error = "no command to sample";
    return profile;
  }

  auto errorCode = std::error_code{};
  std::filesystem::create_directories(request.scratch, errorCode);
  const auto reportPath = std::filesystem::path{request.scratch / "sample.txt"};
  const auto commandOut = std::filesystem::path{request.scratch / "sampled.out"};
  const auto commandErr = std::filesystem::path{request.scratch / "sampled.err"};
  const auto samplerErr = std::filesystem::path{request.scratch / "sampler.err"};

  // Both samplers attach to a process that is already running, so
  // measureLineSamples starts the command, waits settleSeconds and then
  // watches for seconds. A benchmark harness spends its first seconds reading
  // the machine's configuration, and samples taken then describe that rather
  // than the workload: in one four second window the largest single entry was
  // benchmark::CPUInfo::CPUInfo().
  const auto sampled = spawnProcess(request.command, commandOut, commandErr);
  if (sampled < 0) {
    profile.error = "cannot start " + request.command.front();
    return profile;
  }
  usleep(static_cast<unsigned int>(request.settleSeconds * 1e6));

#ifdef __APPLE__
  // -fullPaths, so a frame names the file the compiler recorded rather than a
  // basename a caller would have to guess the directory of.
  const auto watched = runProcess({"sample", std::to_string(sampled),
                                   std::to_string(static_cast<int>(request.seconds)),
                                   "-fullPaths", "-f", reportPath.string()},
                                  request.scratch / "sampler.out", samplerErr,
                                  request.seconds + 120.0);
#else
  // perf does not exist on the machine this was written on, so a person
  // established the two invocations below on a cluster node instead.
  //
  // `perf record -- <command>` samples the command's whole run, so it ignores
  // both request.seconds and the startup settleSeconds exists to skip:
  // measured on a cluster node, that form took 166,249 samples over about 167
  // seconds of processor time where --sample-seconds asked for 20. `-p` with a
  // `sleep` records the running process for exactly that long.
  const auto watched =
      runProcess({"perf", "record", "-F", "997", "-g", "-p", std::to_string(sampled),
                  "--output", (request.scratch / "perf.data").string(), "--", "sleep",
                  std::to_string(static_cast<int>(request.seconds))},
                 request.scratch / "sampler.out", samplerErr, request.seconds + 120.0);
#endif
  const auto ran = waitForProcess(sampled, request.timeoutSeconds);
  if (!watched.ok()) {
    profile.error = std::string{profile.tool} + " " + watched.describe() + ": " +
                    truncate(readFile(samplerErr), 300);
    return profile;
  }
  if (!ran.ok()) {
    profile.error = request.command.front() + " " + ran.describe() + ": " +
                    truncate(readFile(commandErr), 300);
    return profile;
  }

#ifndef __APPLE__
  // --no-children, or perf adds a caller's descendants into that caller's row
  // and the numbers stop being self counts: on a cluster node without it, one
  // row read 129.51 percent and another reported 0 samples for 17.17 percent.
  // --full-source-path, or perf prints a basename and a caller's interesting
  // predicate cannot tell a tree source from a standard library header of the
  // same name. -g none stops perf from printing a call-graph subtree under each
  // row. `perf script -F srcline` was the first attempt and perf refused it:
  // "Display of source line number requested but sample IP is not selected."
  const auto reported = runProcess(
      {"perf", "report", "--stdio", "--no-children", "--sort", "srcline",
       "--show-nr-samples", "-g", "none", "--full-source-path", "--input",
       (request.scratch / "perf.data").string()},
      reportPath, samplerErr, 300.0);
  if (!reported.ok()) {
    profile.error = "perf report " + reported.describe() + ": " +
                    truncate(readFile(samplerErr), 300);
    return profile;
  }
#endif

  const auto report = std::string{readFile(reportPath)};
  if (report.empty()) {
    profile.error = "the sampler wrote nothing to " + reportPath.string();
    return profile;
  }

#ifdef __APPLE__
  auto byLine = std::map<std::pair<std::string, int>, long long>{};
  for (const lineSamplesDetail::Node& node : lineSamplesDetail::nodesOf(report)) {
    if (node.self <= 0) continue;
    profile.total += node.self;
    if (!lineSamplesDetail::endsWith(node.image, request.image)) continue;
    profile.inImage += node.self;
    if (node.file.empty() || node.line <= 0) continue;
    profile.located += node.self;
    byLine[{node.file, node.line}] += node.self;
  }

  for (const auto& [where, weight] : byLine)
    profile.samples.push_back({where.first, where.second, weight});
#else
  // perf prints no image name on a row, so inImage counts the rows perf
  // printed a source line for rather than the rows naming request.image. A
  // caller's interesting predicate reads the absolute path instead.
  profile.samples = lineSamplesDetail::samplesFromPerfReport(report, &profile.total,
                                                             &profile.located);
  profile.inImage = profile.located;
#endif
  std::sort(profile.samples.begin(), profile.samples.end(),
            [](const LineSample& x, const LineSample& y) { return x.weight > y.weight; });

  if (profile.total == 0) {
    profile.error = "the sampler took no samples";
    return profile;
  }
  if (profile.located == 0) {
    profile.error = "no sample in " + request.image +
                    " named a source line; the library needs debug information";
    return profile;
  }
  profile.ok = true;
  return profile;
}

/// A line range, both ends inclusive, with whatever the caller calls it.
struct LineRange {
  std::string file;
  int start = 0;
  int end = 0;
  int key = 0;
};

struct RangeWeights {
  std::map<int, long long> byKey;
  /// Samples whose line is inside a range.
  long long attributed = 0;
  /// Samples in a file the caller called interesting, inside no range. A large
  /// share here means the ranges cover a minority of what ran, and a ranking
  /// over them says less than it appears to.
  long long unattributed = 0;
  /// Samples whose line is inside more than one range. The smallest range took
  /// them. A non-zero count means the caller's ranges overlap.
  long long ambiguous = 0;
};

/// Sums each sample's weight into the range holding its line. The smallest
/// range holding a line takes the sample, and the lowest key breaks a tie.
///
/// interesting decides which files count toward the denominator at all, so
/// time in a standard library header, which no caller's range covers, does not
/// make every share look small.
inline RangeWeights weighRanges(const LineProfile& profile,
                                const std::vector<LineRange>& ranges,
                                const std::function<bool(const std::string&)>& interesting) {
  auto weights = RangeWeights{};
  for (const LineSample& sample : profile.samples) {
    if (!interesting(sample.file)) continue;

    // The sampler reports the path the compiler recorded, which is absolute,
    // while a caller's range names a path relative to a tree root. A suffix
    // match is what relates the two without either side knowing the other's
    // root.
    const auto* smallest = static_cast<const LineRange*>(nullptr);
    auto containing = int{0};
    for (const LineRange& range : ranges) {
      if (sample.line < range.start || sample.line > range.end) continue;
      if (!lineSamplesDetail::endsWith(sample.file, range.file)) continue;
      ++containing;
      const auto span = range.end - range.start;
      if (smallest == nullptr || span < smallest->end - smallest->start ||
          (span == smallest->end - smallest->start && range.key < smallest->key))
        smallest = &range;
    }

    if (smallest == nullptr) {
      weights.unattributed += sample.weight;
      continue;
    }
    if (containing > 1) weights.ambiguous += sample.weight;
    weights.byKey[smallest->key] += sample.weight;
    weights.attributed += sample.weight;
  }
  return weights;
}

}  // namespace utilities
