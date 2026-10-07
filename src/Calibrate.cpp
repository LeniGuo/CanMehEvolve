/// @file
/// CanMehCalibrate -- measures the calibration pairs CMake built, and checks
/// each one against the answer compiled into it.
///
/// A calibration pair is two shared libraries compiled from
/// src/workloads/CalibrationLoop.cpp that differ only in how many work units
/// each one performs. The quotient of those two counts is the true ratio, so a
/// measurement design can be scored against an answer known by construction,
/// which the inert patch cannot provide: the inert patch tests the one point at
/// a true ratio of 1.000 and measures neither detection nor bias.
///
/// CMake builds every pair in place under build/calibration, so this program
/// compiles and links nothing. For each pair it reads the two work counts from
/// meta.json, measures the pair by running CanMehHarness, and compares what
/// CanMehHarness reported against the quotient of those counts.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <CLI11.hpp>
#include <nlohmann/json.hpp>

#include "utilities/Process.hpp"

using utilities::fixedPoint;
using utilities::readJson;
using utilities::runForJsonReply;
using utilities::writeFile;
using nlohmann::json;

namespace {

#ifndef CANMEH_DEFAULT_HARNESS
#define CANMEH_DEFAULT_HARNESS "./CanMehHarness"
#endif
#ifndef CANMEH_DEFAULT_CALIBRATION_ROOT
#define CANMEH_DEFAULT_CALIBRATION_ROOT "calibration"
#endif

/// The probe runs one process at three repetitions, which is the fewest
/// CanMehHarness accepts. The probe establishes that CanMehBench accepts the
/// pair and times one decode, so the probe runs at a min_time far below the
/// min_time the probe is sizing.
constexpr auto probeRepetitions = int{3};
constexpr auto probeProcesses = int{1};
constexpr const auto probeMinTime = "0.001s";

struct Settings {
  std::string root = CANMEH_DEFAULT_CALIBRATION_ROOT;
  std::string harness = CANMEH_DEFAULT_HARNESS;
  int repetitions = 9;
  int repeat = 10;
  std::optional<std::string> minTime;
};

/// One pair, as the meta.json CMake wrote describes it.
struct Pair {
  std::string name;
  std::filesystem::path directory;
  long long referenceUnits = 0;
  long long candidateUnits = 0;
  bool digestBroken = false;
  bool installNameShared = false;

  /// The answer compiled into this pair.
  double trueRatio() const {
    return static_cast<double>(candidateUnits) / static_cast<double>(referenceUnits);
  }  /// The stage CanMehBench must refuse this pair at, empty when CanMehBench
  /// must reach a measurement. expectedStage reads the digest_broken and
  /// install_name_shared flags CMake wrote into meta.json from the
  /// BREAK_DIGEST and SHARE_INSTALL_NAME arguments to
  /// canmeh_calibration_pair.
  std::string expectedStage() const {
    if (digestBroken) return "correctness";
    if (installNameShared) return "load";
    return "";
  }
};

/// Every calibration pair under root, in name order. pairsUnder skips a
/// subdirectory whose meta.json it cannot read or that describes no calibration
/// pair, rather than reporting a failure, so a directory somebody else created
/// under root does not stop the run.
std::vector<Pair> pairsUnder(const std::filesystem::path& root) {
  auto pairs = std::vector<Pair>{};
  auto errorCode = std::error_code{};
  for (const auto& entry : std::filesystem::directory_iterator(root, errorCode)) {
    if (!entry.is_directory()) continue;
    // readJson, not json{readJson(...)}: for nlohmann::json the braces are an
    // initializer list, so json{x} is a one-element array holding x rather
    // than a copy of x.
    const auto described = readJson(entry.path() / "meta.json");
    if (described.is_discarded() || !described.is_object()) continue;
    if (!described.contains("calibration")) continue;
    const auto& knobs = described.at("calibration");
    auto pair = Pair{};
    pair.name = entry.path().filename().string();
    pair.directory = entry.path();
    pair.referenceUnits = knobs.value("reference_units", 0LL);
    pair.candidateUnits = knobs.value("candidate_units", 0LL);
    pair.digestBroken = knobs.value("digest_broken", false);
    pair.installNameShared = knobs.value("install_name_shared", false);
    if (pair.referenceUnits > 0 && pair.candidateUnits > 0) pairs.push_back(pair);
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const Pair& x, const Pair& y) { return x.name < y.name; });
  return pairs;
}

json measurePair(const Settings& settings, const Pair& pair, int repetitions, int repeat,
                  bool withDecision, const std::string& minTime) {
  // runProcess redirects the child's stdout and stderr into this directory,
  // and freopen fails when the directory does not exist, which kills the child
  // before CanMehHarness ever runs. CMake creates each pair's directory but
  // nothing inside it.
  auto errorCode = std::error_code{};
  std::filesystem::create_directories(pair.directory / "scratch", errorCode);
  auto args = std::vector<std::string>{"measure-pair",  "--work",
                                       pair.directory.string(), "--repetitions",
                                       std::to_string(repetitions), "--repeat",
                                       std::to_string(repeat)};
  if (!withDecision) args.push_back("--no-decision");
  if (!minTime.empty()) args.insert(args.end(), {"--min-time", minTime});
  return runForJsonReply(settings.harness, args, pair.directory / "scratch" / "measure.out",
                         pair.directory / "scratch" / "measure.err", 7200);
}

double meanOf(const std::vector<double>& values) {
  if (values.empty()) return 0.0;
  return std::accumulate(values.begin(), values.end(), 0.0) /
         static_cast<double>(values.size());
}

/// Ending long one decode of the slower library takes, in milliseconds, averaged
/// over every repetition of every process. Google Benchmark reports each
/// real_time as a mean over the decodes of one repetition, so a real_time is
/// already a per-decode time.
///
/// slowerDecodeMs pools both libraries' timings and compares nothing, so
/// CanMehCalibrate uses the result only to size --min-time.
double slowerDecodeMs(const json& measured) {
  auto candidate = std::vector<double>{};
  auto reference = std::vector<double>{};
  for (const json& process : measured.at("timings").at("process")) {
    for (double one : process.at("candidate_ns")) candidate.push_back(one);
    for (double one : process.at("reference_ns")) reference.push_back(one);
  }
  return std::max(meanOf(candidate), meanOf(reference)) / 1e6;
}

/// Google Benchmark reports each timing as a mean over however many decodes it
/// fits into --benchmark_min_time. A min_time far above one decode averages the
/// work loop's variance away, so a decision rule scored on a calibration pair
/// measured that way would report a lower false-positive rate than the same
/// rule reaches on the SDK decode. Twice one decode gives about two decodes per
/// repetition, which is what the SDK decode gets at CanMehHarness's default of
/// 0.25s.
std::string minTimeForOneDecode(double decodeMs) {
  return fixedPoint(std::max(2.0 * decodeMs / 1000.0, 0.0001), 4) + "s";
}

/// workloads/CalibrationLoop.cpp reads the work count through a volatile const,
/// so the compiler cannot specialize the kernel on that count and both
/// libraries hold the same code. Two sizes mean the compiler specialized
/// anyway, and the quotient of the two work counts is then not the true ratio.
/// Two compilations of one source should produce files of nearly equal size,
/// so a difference is evidence that the compiler specialized the work loop on
/// the work count.
std::string specializationWarning(const Pair& pair) {
  auto errorCode = std::error_code{};
  auto sizes = std::vector<std::uintmax_t>{};
  for (const char* name : {"libfit_ref", "libfit_cand"})
    for (const char* suffix : {".dylib", ".so"}) {
      const auto path = std::filesystem::path{pair.directory / (std::string{name} + suffix)};
      const auto size = std::filesystem::file_size(path, errorCode);
      if (!errorCode) sizes.push_back(size);
    }
  if (sizes.size() != 2 || sizes.front() == sizes.back()) return "";
  return pair.name + ": the two libraries differ in size (" +
         std::to_string(sizes.front()) + " against " + std::to_string(sizes.back()) +
         " bytes), so the compiler may have specialized the kernel on the work "
         "count. The quotient of the two work counts is then not the true ratio.";
}

/// What this program requires of each pair, and why the requirement differs
/// with the size of the compiled effect.
///
/// A true ratio of 1.000 means both libraries perform the same work, so the
/// measured ratio has to reach 1.000 within the two percent drift
/// CanMehHarness init allows its own null run. A true ratio at or above 2, or
/// at or below one half, has to come within three percent, and that
/// requirement is what the linearity pairs exist for: if one decode takes
/// a + b*N for N work units, the measured ratio is (a + b*M) / (a + b*N), which
/// equals M/N only when a is negligible. sweep-200 measuring 2.000 within three
/// percent is therefore evidence that a is small enough for M/N to predict the
/// measured ratio.
///
/// CanMehCalibrate reports every other pair without requiring anything of it,
/// because near the smallest ratio CanMehBench and CanMehHarness resolve, a
/// miss is evidence about CanMehBench and CanMehHarness rather than about the
/// calibration pairs. When a required pair misses, raise
/// CANMEH_CALIBRATION_REFERENCE_UNITS and rebuild rather than widening the
/// requirement.
bool ratioIsRequired(double trueRatio) {
  return trueRatio == 1.0 || trueRatio >= 2.0 || trueRatio <= 0.5;
}

bool ratioHolds(double trueRatio, double measured) {
  if (trueRatio == 1.0) return std::abs(measured - 1.0) <= 0.02;
  return std::abs(measured / trueRatio - 1.0) <= 0.03;
}

void runCalibrate(const Settings& settings) {
  const auto root = std::filesystem::path{std::filesystem::absolute(settings.root)};
  const auto pairs = pairsUnder(root);
  if (pairs.empty()) {
    std::cerr << "no calibration pair under " << root.string()
              << " -- run cmake --build first\n";
    std::exit(1);
  }

  std::cout << "measuring " << pairs.size() << " calibration pairs under " << root.string()
            << "\nat --repetitions " << settings.repetitions << " --repeat "
            << settings.repeat << "\n\n"
            << "  pair                 reference  candidate      true   measured"
            << "    error  spread  what CanMehBench did\n";

  auto rows = json::array();
  auto wrong = int{0};
  auto totalSeconds = double{0.0};

  for (const Pair& pair : pairs) {
    auto row = json{{"pair", pair.name},
                    {"reference_units", pair.referenceUnits},
                    {"candidate_units", pair.candidateUnits},
                    {"true_ratio", pair.trueRatio()},
                    {"expected_stage", pair.expectedStage()}};
    const auto warning = std::string{specializationWarning(pair)};
    if (!warning.empty()) {
      row["warning"] = warning;
      std::cerr << "warning: " << warning << "\n";
    }

    const auto printPair = [&] {
      std::cout << "  " << std::left << std::setw(20) << pair.name << std::right << "  "
                << std::setw(9) << pair.referenceUnits << "  " << std::setw(9)
                << pair.candidateUnits << "  " << std::setw(8)
                << fixedPoint(pair.trueRatio(), 4);
    };

    const auto probeStart = std::chrono::steady_clock::now();
    const auto probed = measurePair(settings, pair, probeRepetitions, probeProcesses, false,
                                     probeMinTime);
    const auto probeSeconds = double{
        std::chrono::duration<double>(std::chrono::steady_clock::now() - probeStart).count()};
    const auto stage = std::string{probed.value("ok", false)
                                       ? std::string{}
                                       : probed.value("stage", std::string{"unknown"})};

    // A pair CMake built to be refused never reaches a measurement, so the
    // probe is the whole test for it: what matters is the stage CanMehBench
    // reported, not a ratio that does not exist.
    if (!pair.expectedStage().empty() || !stage.empty()) {
      const auto asExpected = bool{stage == pair.expectedStage()};
      if (!asExpected) ++wrong;
      row["stage"] = stage;
      row["as_expected"] = asExpected;
      row["error"] = probed.value("error", std::string{});
      rows.push_back(row);
      printPair();
      std::cout << "                           "
                << (stage.empty() ? "measured" : "refused at stage " + stage);
      if (!asExpected)
        std::cout << ", but CMake built this pair to be "
                  << (pair.expectedStage().empty()
                          ? std::string{"measured"}
                          : "refused at stage " + pair.expectedStage());
      std::cout << "\n";
      continue;
    }

    const auto decodeMs = double{slowerDecodeMs(probed)};
    const auto minTime =
        std::string{settings.minTime.value_or(minTimeForOneDecode(decodeMs))};
    const auto measured =
        measurePair(settings, pair, settings.repetitions, settings.repeat, true, minTime);
    totalSeconds +=
        probeSeconds * settings.repeat +
        (settings.repetitions - probeRepetitions) * 2.0 * decodeMs / 1000.0 * settings.repeat;

    if (!measured.value("ok", false)) {
      ++wrong;
      row["stage"] = measured.value("stage", std::string{"unknown"});
      row["as_expected"] = false;
      row["error"] = measured.value("error", std::string{});
      rows.push_back(row);
      printPair();
      std::cout << "                           the measurement failed at stage "
                << measured.value("stage", std::string{"unknown"}) << "\n";
      continue;
    }

    const auto ratio = double{measured.value("ratio", 0.0)};
    const auto spread = double{measured.value("ratio_spread", 0.0)};
    const auto required = bool{ratioIsRequired(pair.trueRatio())};
    const auto holds = bool{ratioHolds(pair.trueRatio(), ratio)};
    if (required && !holds) ++wrong;
    row["measured_ratio"] = ratio;
    row["ratio_spread"] = spread;
    row["min_time"] = minTime;
    row["decode_ms"] = decodeMs;
    row["required"] = required;
    row["holds"] = holds;
    row["as_expected"] = !required || holds;
    rows.push_back(row);

    printPair();
    std::cout << "  " << std::setw(9) << fixedPoint(ratio, 4) << "  " << std::setw(7)
              << (fixedPoint(100.0 * (ratio / pair.trueRatio() - 1.0), 2) + "%") << "  "
              << std::setw(6) << fixedPoint(spread, 3) << "  "
              << (required ? (holds ? "holds" : "MISSED") : "reported") << "\n";
  }

  // The report records what CMake compiled into each pair and what
  // CanMehHarness reported for it. Dividing the two work counts is the
  // student's job, so true_ratio appears here and never in a pair's meta.json.
  const auto report = json{{"root", root.string()},
                           {"repetitions", settings.repetitions},
                           {"repeat", settings.repeat},
                           {"pairs", rows},
                           {"pairs_not_as_expected", wrong}};
  writeFile(root / "calibration.json", report.dump(2) + "\n");
  std::cout << "\nwrote " << (root / "calibration.json").string() << "\n"
            << "measuring every pair again at these settings takes about "
            << fixedPoint(totalSeconds, 1) << " s\n";
  if (wrong == 0) {
    std::cout << "CanMehBench refused every pair CMake built it to refuse, and the\n"
              << "measured ratio of every pair with a required ratio came within its\n"
              << "margin of the quotient of that pair's compiled work counts, so at\n"
              << "these settings that quotient predicts the measured ratio\n";
    return;
  }
  std::cout << wrong << " pair(s) disagreed with what CMake compiled into them.\n"
            << "A pair CanMehBench refused at a stage CMake did not build it for is a\n"
            << "defect in CanMehBench or in that pair's CMake arguments. A required\n"
            << "ratio that missed means either the machine was busy or one decode is\n"
            << "too short for a decode's fixed work to be negligible: measure again on\n"
            << "an idle machine, then raise CANMEH_CALIBRATION_REFERENCE_UNITS and\n"
            << "rebuild.\n";
  std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
  auto app = CLI::App{
      "Measures the calibration pairs CMake built and checks each against the answer "
      "compiled into it."};
  auto settings = Settings{};
  app.add_option("--root", settings.root, "directory holding the calibration pairs")
      ->capture_default_str();
  app.add_option("--harness", settings.harness, "CanMehHarness binary")
      ->capture_default_str();
  app.add_option("--repetitions", settings.repetitions, "repetitions inside one process")
      ->capture_default_str();
  app.add_option("--repeat", settings.repeat,
                 "independent processes behind one measurement")
      ->capture_default_str();
  app.add_option("--min-time", settings.minTime,
                 "seconds per repetition; left out, CanMehCalibrate times one decode of "
                 "each pair and uses twice that");
  CLI11_PARSE(app, argc, argv);
  runCalibrate(settings);
  return 0;
}
