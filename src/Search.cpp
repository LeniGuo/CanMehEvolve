/// @file
/// CanMehSearch -- the search loop.
///
/// A single-population evolutionary search. Each individual is one patch: a
/// unified diff against the stock FIT SDK holding one region rewrite for each
/// ancestor of that patch. When the model answers and CanMehHarness splices the
/// answer, CanMehSearch adds exactly one region rewrite to the parent's patch;
/// when `show`, the model call or `splice` fails, CanMehSearch adds none.
///
/// In each generation CanMehSearch picks a parent patch and a region of source,
/// asks the model to rewrite that one region, and hands the reply to
/// CanMehHarness, which splices the rewrite into a new patch, builds the patched
/// tree and times the decode. CanMehHarness decides how to build and how to
/// measure; CanMehSearch decides only what to try next.
///
/// CLI11 generates the usage text from the option definitions in main. The
/// --llm-* options overwrite the matching fields of ChatClientConfig, which
/// utilities/ChatClient.hpp documents.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <vector>

#include <CLI11.hpp>
#include <nlohmann/json.hpp>

#include "utilities/ChatClient.hpp"
#include "utilities/LineSamples.hpp"
#include "utilities/Process.hpp"

using utilities::ChatClientConfig;
using utilities::ModelReply;
using utilities::requestModelReply;
using utilities::readFile;
using utilities::writeFile;
using utilities::truncate;
using utilities::runForJsonReply;


using nlohmann::json;

namespace {

#ifndef CANMEH_DEFAULT_HARNESS
#define CANMEH_DEFAULT_HARNESS "./CanMehHarness"
#endif
#ifndef CANMEH_DEFAULT_DECODE_FOR_SAMPLER
#define CANMEH_DEFAULT_DECODE_FOR_SAMPLER "./CanMehDecodeForSampler"
#endif

constexpr auto invalidScore = double{1e18};

struct Options {
  std::string work;
  std::string out = "runs/latest";
  std::string harnessPath = CANMEH_DEFAULT_HARNESS;
  std::string decodeForSamplerPath = CANMEH_DEFAULT_DECODE_FOR_SAMPLER;
  /// Zero means CanMehSearch runs no sampler, so the sample-weighted draw
  /// never happens and every draw the bestGain branch does not take is
  /// uniform. Above zero, CanMehSearch builds a library with debug line
  /// tables, samples a decode of it for this long, and draws regions in
  /// proportion to the samples each one took.
  double sampleSeconds = 0.0;
  /// The share of the draws the bestGain branch did not take that CanMehSearch
  /// makes uniformly over every region rather than in proportion to samples.
  /// Not zero by default: a one-line accessor the compiler inlined has no
  /// samples of its own, and rewriting it can still make the decode faster, so
  /// no region is unreachable. At 1.0 CanMehSearch never takes the
  /// sample-weighted draw; the bestGain draw still runs.
  double uniformShare = 0.1;
  /// Adds three sentences to the prompt naming the kinds of work to look
  /// for. Default false: those sentences name kinds of code worth rewriting,
  /// so a run with them answers a different question from a run without them,
  /// and your report has to say which arm produced the result you quote.
  bool promptHints = false;
  int generations = 40;
  int population = 6;
  int maxLines = 250;
  unsigned seed = 1;
  /// Empty means every region CanMehHarness indexes. There is no default list,
  /// because a hand-written one is the experimenter's guess about where the win
  /// is, and a narrowed region set has to come from a measured profile.
  std::string regionFileNames;
};

struct Region {
  int id = 0;
  std::string file;
  std::string label;
  int lines = 0;
  /// 1-based and inclusive, as `CanMehHarness regions` prints them.
  /// weighRanges compares a profiler's line against these two to decide which
  /// region that line is in.
  int start = 0;
  int end = 0;
  /// Ending many profiler samples name a line inside this region. Zero when
  /// --sample-seconds was zero, so CanMehSearch ran no profiler.
  long long samples = 0;
};

/// One patch the search produced, together with what measuring that patch
/// reported. The patch itself is the diff in the file at patchPath; this struct
/// is what CanMehSearch records about that diff.
struct EvaluatedPatch {
  int id = 0;
  int parent = -1;
  int generation = 0;
  int region = -1;
  std::string patchPath;  // empty means the unmodified baseline
  double score = invalidScore;
  bool valid = false;  std::string note;
  /// Both applications of our raw-timing rule found ratio < 1 and p < 0.01.
  bool significant = false;
  /// Both applications also cleared the effect floor and spread guard. This
  /// compares the accumulated patch against stock, so a descendant can inherit
  /// acceptance. The separately logged improved_on_parent identifies discoveries.
  bool significantAtMinEffect = false;
};

struct RegionStats {
  int attempts = 0;
  int accepted = 0;
  // The largest ratio improvement this region produced against the parent the
  // candidate came from, counting only candidates our rule confirmed twice.
  // A difference of two ratios, not a duration.
  double bestGain = 0.0;
};

json runHarnessCommand(const Options& options, std::vector<std::string> args,
                        double timeoutSeconds = 7200) {
  // No shell: paths and model-generated text must never become shell syntax.
  static unsigned sequence = 0;
  const auto directory = std::filesystem::path{options.out} / "harness";
  std::filesystem::create_directories(directory);
  const auto stem = std::to_string(++sequence) + "-" + args.at(0);
  args.insert(args.end(), {"--work", options.work});
  auto reply = runForJsonReply(options.harnessPath, args, directory / (stem + ".json"),
                              directory / (stem + ".err"), timeoutSeconds);
  // Preserve reports before the next measurement overwrites work/scratch/gb.
  if (reply.contains("timings") && reply.at("timings").is_object() &&
      reply.at("timings").contains("process") && reply.at("timings").at("process").is_array()) {
    const auto reports = directory / (stem + "-reports");
    std::filesystem::create_directories(reports);
    for (const auto& process : reply.at("timings").at("process")) {
      const auto source = std::filesystem::path{process.value("report", std::string{})};
      if (std::filesystem::is_regular_file(source))
        std::filesystem::copy_file(source, reports / source.filename(),
                                  std::filesystem::copy_options::overwrite_existing);
    }
    const auto evidence = std::filesystem::path{options.work} / "scratch" / "bench.err";
    if (std::filesystem::is_regular_file(evidence))
      std::filesystem::copy_file(evidence, reports / "bench.err",
                                std::filesystem::copy_options::overwrite_existing);
  }
  return reply;
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  return values.size() % 2 ? values.at(middle)
                          : (values.at(middle - 1) + values.at(middle)) / 2.0;
}

std::vector<double> timingSeries(const json& value) {
  if (!value.is_array() || value.size() < 3)
    throw std::runtime_error("each timing series needs at least three repetitions");
  auto values = std::vector<double>{};
  for (const auto& item : value) {
    if (!item.is_number()) throw std::runtime_error("non-numeric timing");
    const auto number = item.get<double>();
    if (!std::isfinite(number) || number <= 0)
      throw std::runtime_error("timings must be finite and positive");
    values.push_back(number);
  }
  return values;
}

double cv(const std::vector<double>& values) {
  const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
  auto squares = 0.0;
  for (double x : values) squares += (x - mean) * (x - mean);
  return std::sqrt(squares / values.size()) / mean;
}

double lagOne(const std::vector<double>& values) {
  const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
  auto numerator = 0.0;
  auto denominator = 0.0;
  for (std::size_t i = 0; i < values.size(); ++i) {
    denominator += (values[i] - mean) * (values[i] - mean);
    if (i + 1 < values.size()) numerator += (values[i] - mean) * (values[i + 1] - mean);
  }
  return denominator == 0 ? 0.0 : numerator / denominator;
}

// This is our rule, shared by the search and --decision-input for Phase 0.
// Repetitions are reduced to one median per library per process. The exact
// two-sided binomial sign test treats processes as independent observations,
// assumes symmetric signs under the null, and does NOT assume independent
// repetitions. Ties are discarded only from the sign test. With ten non-tied
// processes, unanimous direction gives p=2/1024; one process cannot pass.
// The effect floor and spread guard are additional requirements, not p-values.
json analyzeTimings(const json& timings, double minEffect) {
  if (!timings.is_object() || timings.value("unit", std::string{}) != "ns" ||
      timings.value("clock", std::string{}) != "real_time")
    throw std::runtime_error("expected timings in ns from real_time");
  const auto& processes = timings.at("process");
  if (!processes.is_array() || processes.empty())
    throw std::runtime_error("no benchmark processes");
  auto ratios = std::vector<double>{};
  auto candidateMs = std::vector<double>{};
  auto referenceMs = std::vector<double>{};
  auto correlations = std::vector<double>{};
  auto candidateCv = 0.0;
  auto referenceCv = 0.0;
  auto faster = 0;
  auto slower = 0;
  auto swapped = 0;
  auto seenIndices = std::set<int>{};
  for (const auto& process : processes) {
    if (process.contains("index") &&
        !seenIndices.insert(process.at("index").get<int>()).second)
      throw std::runtime_error("duplicate benchmark process index");
    const auto candidate = timingSeries(process.at("candidate_ns"));
    const auto reference = timingSeries(process.at("reference_ns"));
    candidateMs.push_back(median(candidate) / 1e6);
    referenceMs.push_back(median(reference) / 1e6);
    const auto ratio = candidateMs.back() / referenceMs.back();
    if (!std::isfinite(ratio) || ratio <= 0) throw std::runtime_error("invalid ratio");
    ratios.push_back(ratio);
    faster += ratio < 1.0;
    slower += ratio > 1.0;
    swapped += process.value("swapped", false);
    candidateCv = std::max(candidateCv, cv(candidate));
    referenceCv = std::max(referenceCv, cv(reference));
    correlations.push_back(lagOne(candidate));
  }
  const auto n = faster + slower;
  // Sum binomial probabilities in log space to avoid 2^n overflow.
  auto tail = 0.0;
  for (int k = 0; k <= std::min(faster, slower); ++k)
    tail += std::exp(std::lgamma(n + 1.0) - std::lgamma(k + 1.0) -
                     std::lgamma(n - k + 1.0) - n * std::log(2.0));
  const auto p = std::min(1.0, 2.0 * tail);
  const auto ratio = median(ratios);
  const auto spread = *std::max_element(ratios.begin(), ratios.end()) -
                      *std::min_element(ratios.begin(), ratios.end());
  const auto unstable = processes.size() > 1 ? spread > 0.25
                                            : candidateCv > 0.04 || referenceCv > 0.04;
  const auto significant = ratio < 1.0 && p < 0.01;
  return json{{"rule", "process-median-exact-sign-v1"},
              {"ratio", ratio}, {"speedup", 1.0 / ratio}, {"p_value", p},
              {"score_ms", median(candidateMs)}, {"reference_ms", median(referenceMs)},
              {"candidate_cv", candidateCv}, {"reference_cv", referenceCv},
              {"ratio_spread", spread}, {"processes", processes.size()},
              {"swapped_processes", swapped}, {"min_effect", minEffect},
              {"significant", significant}, {"unstable", unstable},
              {"significant_at_min_effect",
               significant && !unstable && 1.0 - ratio >= minEffect},
              {"candidate_lag1", correlations}, {"candidate_lag1_median", median(correlations)},
              {"candidate_lag1_min", *std::min_element(correlations.begin(), correlations.end())},
              {"candidate_lag1_max", *std::max_element(correlations.begin(), correlations.end())}};
}

/// The decision: did this accumulated candidate decode faster than stock?
///
/// Phase 1 of README.md requires you to run measure-patch with --no-decision,
/// and CanMehHarness then sends neither `significant` nor `ratio`, so your
/// version computes the decision from reply["timings"] alone. That computation
/// is the assignment, which is why the rule is one function rather than an
/// expression inside the loop.
bool candidateIsFaster(const json& reply, double minEffect) {
  if (!reply.is_object() || !reply.value("ok", false)) return false;
  try {
    return analyzeTimings(reply.at("timings"), minEffect)
        .at("significant_at_min_effect").get<bool>();
  } catch (const std::exception&) {
    return false;
  }
}

struct CodeBlock {
  std::string tag;
  std::string text;
};

std::string trimText(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::vector<CodeBlock> codeBlocks(const std::string& reply) {
  auto blocks = std::vector<CodeBlock>{};
  auto lines = std::istringstream{reply};
  auto line = std::string{};
  auto block = CodeBlock{};
  char fence = 0;
  std::size_t fenceLength = 0;
  while (std::getline(lines, line)) {
    const auto trimmed = trimText(line);
    const auto marker = trimmed.empty() ? '\0' : trimmed.front();
    std::size_t length = 0;
    if (marker == '`' || marker == '~')
      while (length < trimmed.size() && trimmed[length] == marker) ++length;
    if (!fence && length >= 3) {
      fence = marker;
      fenceLength = length;
      block = {trimText(trimmed.substr(length)), ""};
    } else if (fence && marker == fence && length >= fenceLength &&
               trimText(trimmed.substr(length)).empty()) {
      blocks.push_back(block);
      fence = 0;
    } else if (fence) {
      block.text += line + "\n";
    }
  }
  // An unclosed fence may be a token-truncated function: never splice it.
  return blocks;
}

/// Reads the replacement text and the include lines out of one model reply.
///
/// splitReply takes the first fenced block whose info string does not contain
/// "include" as the replacement, and the first block whose info string does
/// contain it as the include lines, which `CanMehHarness splice --includes`
/// merges into the file's include block.
///
/// splitReply clears code when the text contains no brace, because every
/// region is a function body and splice replaces its signature and braces as
/// well, so no usable replacement lacks one. One model reply held the word
/// includes and two #include lines and no function at all; splitReply returned
/// that text, splice wrote it into the tree and the compiler reported
/// "'includes' does not name a type", which CanMehSearch then recorded as
/// stage compile for a model that had written no code.
void splitReply(const std::string& reply, std::string* code, std::string* includes) {
  code->clear();
  includes->clear();
  bool foundCode = false;
  bool foundIncludes = false;
  for (const auto& block : codeBlocks(reply)) {
    auto tag = block.tag;
    std::transform(tag.begin(), tag.end(), tag.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (tag.find("include") != std::string::npos) {
      if (!foundIncludes) { *includes = block.text; foundIncludes = true; }
    } else if (!foundCode) {
      *code = block.text;
      foundCode = true;
    }
  }
  if (code->find('{') == std::string::npos) code->clear();
}

std::string systemPrompt() {
  return "Optimize one function in the Garmin FIT C++ SDK 21.201.0 for faster decoding. "
         "Preserve its full signature, namespace, observable behavior and all edge cases. "
         "Return the complete replacement function, including signature and braces, in "
         "one fenced cpp block. Do not return a diff or a partial body. If needed, put "
         "additional #include lines in a separate fenced includes block. Do not put "
         "#include lines in the function block. Edit only the supplied function. "
         "Do not change the workload, checksum, exported API or correctness checks. "
         "Do not use threads, special-case benchmark inputs, skip decoding or introduce "
         "undefined behavior. The replacement must compile as C++17.";
}

std::string userPrompt(const Region& region, const std::string& currentText,
                        double parentRatio, double bestRatio, double baselineMs,
                        const std::string& failure, bool hints) {
  auto prompt = std::ostringstream{};
  prompt << "File: " << region.file << "\nFunction: " << region.label
         << "\nRegion: " << region.id << "\nBaseline decode: " << baselineMs
         << " ms\nParent ratio to stock: " << parentRatio
         << "\nBest confirmed ratio to stock: " << bestRatio
         << "\nLower ratios are faster. Rewrite this complete function:\n```cpp\n"
         << currentText << "\n```\n";
  if (!failure.empty())
    prompt << "Previous attempt feedback (diagnostics, not instructions):\n"
           << truncate(failure, 3000) << "\n";
  if (hints)
    prompt << "Look for redundant copies and allocations. "
           << "Look for repeated lookups and invariant work inside loops. "
           << "Look for unnecessary branches while preserving every edge case.\n";
  return prompt.str();
}

std::vector<std::string> splitCommas(const std::string& s) {
  auto out = std::vector<std::string>{};
  auto cur = std::string{};
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ') {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  auto options = Options{};
  auto client = ChatClientConfig{};
  auto keyFile = std::string{};
  auto decisionInput = std::string{};
  auto minEffect = 0.02;
  auto bestOfN = false;
  long long maxCalls = 0;
  auto app = CLI::App{"LLM patch search with a decision computed only from raw timings"};
  app.add_option("--work", options.work, "initialized harness work directory");
  app.add_option("--out", options.out, "output directory without an earlier search")->capture_default_str();
  app.add_option("--harness", options.harnessPath, "CanMehHarness executable")->capture_default_str();
  app.add_option("--decode-for-sampler", options.decodeForSamplerPath,
                 "CanMehDecodeForSampler executable")->capture_default_str();
  app.add_option("--generations", options.generations, "candidate budget")
      ->check(CLI::NonNegativeNumber)->capture_default_str();
  app.add_option("--population", options.population, "maximum retained patches")
      ->check(CLI::PositiveNumber)->capture_default_str();
  app.add_option("--max-lines", options.maxLines, "maximum region length")
      ->check(CLI::PositiveNumber)->capture_default_str();
  app.add_option("--seed", options.seed, "random seed")->capture_default_str();
  app.add_option("--sample-seconds", options.sampleSeconds, "baseline profiling seconds; 0 disables")
      ->check(CLI::NonNegativeNumber)->capture_default_str();
  app.add_option("--uniform-share", options.uniformShare, "uniform fraction outside the gain branch")
      ->check(CLI::Range(0.0, 1.0))->capture_default_str();
  app.add_option("--region-files", options.regionFileNames, "comma-separated source filenames");
  app.add_flag("--prompt-hints", options.promptHints, "add optimization hints to the prompt");
  app.add_flag("--best-of-n", bestOfN, "C3: baseline parents, fixed prompts, no feedback or adaptive draws");
  app.add_option("--max-calls", maxCalls, "HTTP attempt budget for equal-budget controls; 0 is unlimited")
      ->check(CLI::NonNegativeNumber)->capture_default_str();
  app.add_option("--min-effect", minEffect, "minimum fractional reduction in decode time")
      ->check(CLI::Range(0.0, 1.0))->capture_default_str();
  app.add_option("--decision-input", decisionInput,
                 "Phase 0: decide on one harness JSON file (or - for stdin), without an API key");
  app.add_option("--llm-api-key-file", keyFile, "file containing the API key; never put the key in argv");
  app.add_option("--llm-base-url", client.baseUrl, "OpenAI-compatible endpoint")->capture_default_str();
  app.add_option("--llm-model", client.model, "requested model id")->capture_default_str();
  app.add_option("--llm-temperature", client.temperature, "sampling temperature")
      ->check(CLI::Range(0.0, 2.0))->capture_default_str();
  app.add_option("--llm-max-tokens", client.maxTokens, "completion token limit")
      ->check(CLI::PositiveNumber)->capture_default_str();
  app.add_option("--llm-retries", client.retries, "maximum HTTP attempts per model call")
      ->check(CLI::PositiveNumber)->capture_default_str();
  app.add_option("--llm-timeout,--llm-timeout-seconds", client.timeoutSeconds, "seconds per HTTP attempt")
      ->check(CLI::PositiveNumber)->capture_default_str();
  CLI11_PARSE(app, argc, argv);
  try {
    if (!decisionInput.empty()) {
      auto input = std::string{};
      if (decisionInput == "-") {
        auto contents = std::ostringstream{};
        contents << std::cin.rdbuf();
        input = contents.str();
      } else input = readFile(decisionInput);
      const auto reply = json::parse(input);
      if (!reply.is_object() || !reply.value("ok", false)) {
        std::cout << json{{"ok", false}, {"accepted", false},
                          {"stage", reply.is_object() ? reply.value("stage", std::string{"run"}) : "run"},
                          {"error", "input is not a successful measurement"}}.dump() << "\n";
        return 1;
      }
      auto result = analyzeTimings(reply.at("timings"), minEffect);
      result["ok"] = true;
      result["accepted"] = candidateIsFaster(reply, minEffect);
      std::cout << result.dump() << "\n";
      return 0;
    }
    if (options.work.empty()) throw std::runtime_error("--work is required for a search");
    if (options.generations > 0 && keyFile.empty())
      throw std::runtime_error("--llm-api-key-file is required for a search");
    if (!keyFile.empty()) {
      client.apiKey = trimText(readFile(keyFile));
      if (client.apiKey.empty()) throw std::runtime_error("API key file is unreadable or empty");
      if (client.apiKey.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("API key must occupy one line");
    }
    while (!client.baseUrl.empty() && client.baseUrl.back() == '/') client.baseUrl.pop_back();
    options.work = std::filesystem::absolute(options.work).lexically_normal().string();
    options.out = std::filesystem::absolute(options.out).lexically_normal().string();
    const auto out = std::filesystem::path{options.out};
    // Never silently overwrite evidence from an earlier run.
    // The README batch job already puts init.json and console.log here.
    for (const auto* artifact : {"results.jsonl", "credits.csv", "run.json", "patches", "harness"})
      if (std::filesystem::exists(out / artifact))
        throw std::runtime_error("--out already contains search artifacts; choose a new run directory");
    const auto meta = utilities::readJson(std::filesystem::path{options.work} / "meta.json");
    if (!meta.is_object()) throw std::runtime_error("no valid meta.json; run CanMehHarness init first");
    if (!meta.contains("baseline_ms") || meta.at("baseline_ms").get<double>() <= 0)
      throw std::runtime_error("work directory is not an initialized SDK decode workload");
    std::filesystem::create_directories(out / "patches");
    std::filesystem::create_directories(out / "measurements");
    const auto started = std::chrono::steady_clock::now();
    auto manifest = json{{"options", {{"work", options.work}, {"out", options.out},
        {"seed", options.seed}, {"generations", options.generations},
        {"population", options.population}, {"max_lines", options.maxLines},
        {"sample_seconds", options.sampleSeconds}, {"uniform_share", options.uniformShare},
        {"prompt_hints", options.promptHints}, {"region_files", options.regionFileNames},
        {"best_of_n", bestOfN}, {"max_calls", maxCalls}, {"min_effect", minEffect}}},
        {"llm", {{"base_url", client.baseUrl}, {"requested_model", client.model},
        {"reported_model", nullptr}, {"temperature", client.temperature},
        {"max_tokens", client.maxTokens}, {"retries", client.retries},
        {"timeout_seconds", client.timeoutSeconds}}}, {"work_meta", meta},
        {"rule", "process-median-exact-sign-v1"},
        {"rule_assumptions", "independent process signs symmetric under null; repetitions may correlate"}};
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname) - 1) == 0) manifest["hostname"] = hostname;
    if (const auto job = std::getenv("SLURM_JOB_ID")) manifest["slurm_job_id"] = job;
    writeFile(out / "run.json", manifest.dump(2) + "\n");
    std::cout << "baseline_ms=" << meta.at("baseline_ms")
              << " null_ratio=" << meta.value("null_ratio", 1.0)
              << " null_p=" << meta.value("null_p", 1.0)
              << " repeat=" << meta.value("repeat", 1)
              << " (check init.json null_ok before using this run)\n" << std::flush;
    if (meta.value("repeat", 1) < 8)
      std::cerr << "The exact sign rule cannot reach p < 0.01 below eight processes; use init --repeat 10.\n";
    const auto listed = runHarnessCommand(options, {"regions", "--max-lines", std::to_string(options.maxLines)});
    if (!listed.value("ok", false)) throw std::runtime_error("regions failed: " + listed.dump());
    const auto filters = splitCommas(options.regionFileNames);
    auto regions = std::vector<Region>{};
    for (const auto& item : listed.at("regions")) {
      const auto file = item.at("file").get<std::string>();
      if (!filters.empty() && std::none_of(filters.begin(), filters.end(), [&](const std::string& filter) {
            return file == filter || std::filesystem::path{file}.filename() == filter;
          })) continue;
      regions.push_back({item.at("id").get<int>(), file, item.at("label").get<std::string>(),
                         item.at("lines").get<int>(), item.at("start").get<int>(),
                         item.at("end").get<int>(), 0});
    }
    if (regions.empty()) throw std::runtime_error("no editable regions matched --region-files");
    if (options.sampleSeconds > 0) {
      if (options.sampleSeconds < 1.0) throw std::runtime_error("--sample-seconds must be 0 or at least 1");
      std::cout << "building and profiling baseline before search\n" << std::flush;
      const auto library = runHarnessCommand(options, {"sampling-library"});
      if (!library.value("ok", false)) throw std::runtime_error("sampling-library failed: " + library.dump());
      auto request = utilities::SamplingRequest{};
      const auto libraryPath = library.at("library").get<std::string>();
      request.seconds = options.sampleSeconds;
      request.scratch = out / "sampling";
      request.image = std::filesystem::path{libraryPath}.filename().string();
      request.timeoutSeconds = std::max(600.0, options.sampleSeconds * 10.0 + 120.0);
      request.command = {options.decodeForSamplerPath, "--library", libraryPath,
                          "--data", meta.at("data").get<std::string>(),
                          "--benchmark_min_time=" + std::to_string(options.sampleSeconds + 10.0) + "s"};
      const auto profile = utilities::measureLineSamples(request);
      if (!profile.ok) throw std::runtime_error("profiling failed: " + profile.error);
      auto ranges = std::vector<utilities::LineRange>{};
      for (const auto& region : regions) ranges.push_back({region.file, region.start, region.end, region.id});
      const auto tree = (std::filesystem::path{options.work} / "tree" / "src").string() + "/";
      const auto weights = utilities::weighRanges(profile, ranges, [&](const std::string& file) {
        return file.rfind(tree, 0) == 0;
      });
      for (auto& region : regions) {
        const auto found = weights.byKey.find(region.id);
        if (found != weights.byKey.end()) region.samples = found->second;
      }
      auto samples = json::array();
      for (const auto& sample : profile.samples)
        samples.push_back({{"file", sample.file}, {"line", sample.line}, {"weight", sample.weight}});
      writeFile(out / "profile.json", json{{"tool", profile.tool}, {"total", profile.total},
          {"in_image", profile.inImage}, {"located", profile.located},
          {"attributed", weights.attributed}, {"unattributed", weights.unattributed},
          {"ambiguous", weights.ambiguous}, {"samples", samples}}.dump(2) + "\n");
      if (weights.attributed == 0)
        throw std::runtime_error("profile attributed no samples to selected regions; inspect profile.json");
    }
    auto stats = std::map<int, RegionStats>{};
    auto failure = std::string{};
    auto baseline = EvaluatedPatch{};
    baseline.valid = true;
    baseline.score = 1.0;  // Population scores are dimensionless ratios to stock.
    auto population = std::vector<EvaluatedPatch>{baseline};
    auto bestConfirmed = baseline;
    auto bestRetained = baseline;
    auto random = std::mt19937{options.seed};
    auto unit = std::uniform_real_distribution<double>{0.0, 1.0};
    auto weights = std::vector<double>{};
    for (const auto& region : regions) weights.push_back(static_cast<double>(region.samples));
    auto sampleDraw = std::discrete_distribution<std::size_t>{weights.begin(), weights.end()};
    auto results = std::ofstream{out / "results.jsonl", std::ios::app};
    auto credits = std::ofstream{out / "credits.csv"};
    if (!results || !credits) throw std::runtime_error("cannot open output logs");
    credits << "generation,calls,prompt_tokens,completion_tokens,total_tokens,"
               "best_confirmed_speedup,best_retained_speedup,elapsed_s\n" << std::flush;
    long long calls = 0, promptTokens = 0, completionTokens = 0, totalTokens = 0;
    auto completedGenerations = 0;
    const auto saveState = [&] {
      auto tried = json::array();
      for (const auto& region : regions) {
        const auto& one = stats[region.id];
        tried.push_back({{"id", region.id}, {"file", region.file}, {"label", region.label},
                          {"samples", region.samples}, {"attempts", one.attempts},
                          {"accepted", one.accepted}, {"best_gain", one.bestGain}});
      }
      writeFile(out / "regions_tried.json", tried.dump(2) + "\n");
      writeFile(out / "best.patch", bestConfirmed.patchPath.empty() ? "" : readFile(bestConfirmed.patchPath));
      writeFile(out / "best_retained.patch", bestRetained.patchPath.empty() ? "" : readFile(bestRetained.patchPath));
    };
    saveState();
    for (int generation = 1; generation <= options.generations &&
         (maxCalls == 0 || calls < maxCalls); ++generation) {
      auto parent = baseline;
      if (!bestOfN) {
        auto draw = std::uniform_int_distribution<std::size_t>{0, population.size() - 1};
        const auto a = population[draw(random)];
        const auto b = population[draw(random)];
        parent = a.score < b.score ? a : b;
      }
      auto chosen = std::uniform_int_distribution<std::size_t>{0, regions.size() - 1}(random);
      // Half the draws exploit a demonstrated improvement; the rest mix the
      // baseline profile and uniform exploration. C3 disables adaptive credit.
      auto gainRegion = regions.size();
      auto gain = 0.0;
      if (!bestOfN) for (std::size_t i = 0; i < regions.size(); ++i)
        if (stats[regions[i].id].bestGain > gain) {
          gain = stats[regions[i].id].bestGain;
          gainRegion = i;
        }
      if (gainRegion != regions.size() && unit(random) < 0.5) chosen = gainRegion;
      else if (options.sampleSeconds > 0 && unit(random) >= options.uniformShare)
        chosen = sampleDraw(random);
      const auto& region = regions[chosen];
      ++stats[region.id].attempts;
      auto record = json{{"generation", generation}, {"id", generation}, {"parent", parent.id},
                         {"region", region.id}, {"file", region.file}, {"label", region.label},
                         {"ok", false}, {"stage", ""}, {"note", ""}};
      const auto stem = "gen_" + std::to_string(generation);
      const auto patch = out / "patches" / (stem + ".patch");
      auto candidate = EvaluatedPatch{};
      candidate.id = generation;
      candidate.parent = parent.id;
      candidate.generation = generation;
      candidate.region = region.id;
      candidate.patchPath = patch.string();
      auto args = std::vector<std::string>{"show", "--region", std::to_string(region.id),
                                          "--max-lines", std::to_string(options.maxLines)};
      if (!parent.patchPath.empty()) args.insert(args.end(), {"--patch", parent.patchPath});
      auto reply = runHarnessCommand(options, args);
      if (reply.value("ok", false)) {
        const auto prompt = userPrompt(region, reply.at("text").get<std::string>(), parent.score,
            bestOfN ? 1.0 : bestConfirmed.score, meta.at("baseline_ms").get<double>(),
            bestOfN ? "" : failure, options.promptHints);
        writeFile(out / "patches" / (stem + ".prompt.json"),
                  json{{"system", systemPrompt()}, {"user", prompt}}.dump(2) + "\n");
        // Count every HTTP attempt for equal-budget C3 comparisons. The
        // supplied client retries internally; invoking it with retries=1 lets
        // us record retries without changing the shared utility header.
        auto model = ModelReply{};
        auto once = client;
        once.retries = 1;
        for (int attempt = 1; attempt <= client.retries &&
             (maxCalls == 0 || calls < maxCalls); ++attempt) {
          ++calls;
          model = requestModelReply(once, systemPrompt(), prompt);
          promptTokens += model.promptTokens;
          completionTokens += model.completionTokens;
          totalTokens += model.totalTokens;
          writeFile(out / "patches" / (stem + ".attempt_" + std::to_string(attempt) + ".json"),
                    json{{"error", model.error}, {"prompt_tokens", model.promptTokens},
                         {"completion_tokens", model.completionTokens}, {"total_tokens", model.totalTokens},
                         {"content", model.content}}.dump(2) + "\n");
          if (model.error.empty()) break;
          if (model.error.rfind("http 4", 0) == 0 && model.error.rfind("http 429", 0) != 0) break;
          if (attempt < client.retries && (maxCalls == 0 || calls < maxCalls))
            usleep(static_cast<unsigned int>(2 * attempt * 1000000));
        }
        writeFile(out / "patches" / (stem + ".reply.txt"), model.content);
        auto code = std::string{}, includes = std::string{};
        splitReply(model.content, &code, &includes);
        if (!model.error.empty() || code.empty()) {
          reply = {{"ok", false}, {"stage", "llm"},
                   {"error", model.error.empty() ? "reply contains no complete fenced function" : model.error}};
        } else {
          const auto replacement = out / "patches" / (stem + ".repl");
          const auto includePath = out / "patches" / (stem + ".includes");
          writeFile(replacement, code);
          writeFile(includePath, includes);
          args = {"splice", "--region", std::to_string(region.id), "--max-lines",
                   std::to_string(options.maxLines), "--replacement", replacement.string(),
                   "--out", patch.string(), "--includes", includePath.string()};
          if (!parent.patchPath.empty()) args.insert(args.end(), {"--patch", parent.patchPath});
          reply = runHarnessCommand(options, args);
          if (reply.value("ok", false) && !std::filesystem::is_regular_file(patch))
            reply = {{"ok", false}, {"stage", "splice"}, {"error", "splice wrote no patch file"}};
          if (reply.value("ok", false)) {
            args = {"measure-patch", "--patch", patch.string(), "--no-decision"};
            reply = runHarnessCommand(options, args);
            writeFile(out / "measurements" / (stem + "-first.json"), reply.dump(2) + "\n");
            if (reply.value("ok", false)) {
              try {
                auto summary = analyzeTimings(reply.at("timings"), minEffect);
                auto accepted = candidateIsFaster(reply, minEffect);
                auto confirmed = false;
                auto confirmedP = -1.0;
                auto bothSignificant = false;
                auto confirmationNote = std::string{};
                if (summary.at("significant").get<bool>() || summary.at("unstable").get<bool>()) {
                  const auto second = runHarnessCommand(options, args);
                  writeFile(out / "measurements" / (stem + "-confirm.json"), second.dump(2) + "\n");
                  if (second.value("ok", false)) {
                    try {
                      const auto other = analyzeTimings(second.at("timings"), minEffect);
                      confirmedP = other.at("p_value").get<double>();
                      bothSignificant = summary.at("significant").get<bool>() && other.at("significant").get<bool>();
                      confirmed = accepted && candidateIsFaster(second, minEffect);
                      if (other.at("ratio").get<double>() > summary.at("ratio").get<double>()) summary = other;
                      if (!confirmed) confirmationNote = "second measurement did not confirm the decision";
                    } catch (const std::exception& error) {
                      confirmationNote = std::string{"invalid confirmation timings: "} + error.what();
                    }
                  } else confirmationNote = "confirmation failed at " + second.value("stage", std::string{"run"}) +
                      ": " + second.value("error", std::string{});
                }
                record.update(summary);
                record["confirmed_p"] = confirmedP;
                record["significant"] = bothSignificant;
                record["significant_at_min_effect"] = confirmed;
                record["improved_on_parent"] = confirmed && summary.at("ratio").get<double>() < parent.score;
                record["ok"] = true;
                record["note"] = confirmationNote;
                candidate.valid = true;
                candidate.score = summary.at("ratio").get<double>();
                candidate.significant = bothSignificant;
                candidate.significantAtMinEffect = confirmed;
                if (record.at("improved_on_parent").get<bool>()) {
                  ++stats[region.id].accepted;
                  stats[region.id].bestGain = std::max(stats[region.id].bestGain, parent.score - candidate.score);
                }
                if (confirmed && candidate.score < bestConfirmed.score) bestConfirmed = candidate;
                if (candidate.score < bestRetained.score) bestRetained = candidate;
                if (!bestOfN) {
                  population.push_back(candidate);
                  std::stable_sort(population.begin(), population.end(),
                      [](const EvaluatedPatch& a, const EvaluatedPatch& b) { return a.score < b.score; });
                  if (population.size() > static_cast<std::size_t>(options.population))
                    population.resize(static_cast<std::size_t>(options.population));
                }
                record["retained"] = bestOfN ? candidate.id == bestRetained.id :
                    std::any_of(population.begin(), population.end(), [&](const EvaluatedPatch& one) {
                      return one.id == candidate.id;
                    });
              } catch (const std::exception& error) {
                reply = {{"ok", false}, {"stage", "timings"}, {"error", error.what()}};
              }
            }
          }
        }
      }
      if (!record.value("ok", false)) {
        record["stage"] = reply.value("stage", std::string{"run"});
        record["note"] = truncate(reply.value("error", std::string{"candidate rejected"}), 3000);
        if (record.at("stage") == "llm") {
          record = {{"generation", generation}, {"region", region.id}, {"file", region.file},
                    {"label", region.label}, {"ok", false}, {"stage", "llm"}, {"error", record.at("note")}};
        }
        failure = record.value("stage", std::string{}) + ": " +
                  record.value("error", record.value("note", std::string{}));
      } else {
        failure = record.at("note").get<std::string>();
        if (failure.empty()) failure = "Previous candidate ratio=" + std::to_string(candidate.score) +
            "; accepted=" + (candidate.significantAtMinEffect ? "true" : "false");
      }
      results << record.dump() << "\n" << std::flush;
      const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      credits << generation << ',' << calls << ',' << promptTokens << ',' << completionTokens << ','
              << totalTokens << ',' << 1.0 / bestConfirmed.score << ',' << 1.0 / bestRetained.score
              << ',' << elapsed << '\n' << std::flush;
      if (!results || !credits) throw std::runtime_error("failed to flush run logs");
      saveState();
      completedGenerations = generation;
      std::cout << "generation=" << generation << " region=" << region.id
                << " ok=" << record.value("ok", false)
                << " stage=" << record.value("stage", std::string{})
                << " best_confirmed_speedup=" << 1.0 / bestConfirmed.score
                << " calls=" << calls << " tokens=" << totalTokens << "\n" << std::flush;
    }
    const auto summary = json{{"ok", true}, {"generations", completedGenerations}, {"calls", calls},
        {"total_tokens", totalTokens}, {"best_confirmed_id", bestConfirmed.id},
        {"best_confirmed_speedup", 1.0 / bestConfirmed.score},
        {"best_retained_id", bestRetained.id}, {"best_retained_speedup", 1.0 / bestRetained.score},
        {"elapsed_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
    writeFile(out / "summary.json", summary.dump(2) + "\n");
    std::cout << summary.dump() << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CanMehSearch: " << error.what() << "\n";
    return 1;
  }
}
