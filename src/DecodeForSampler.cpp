/// @file
/// CanMehDecodeForSampler -- decodes repeatedly out of one shared library, so
/// that a sampling profiler can record which source line this process is
/// executing.
///
/// A sampling profiler interrupts a process at intervals and records the
/// instruction address each thread stopped at. Debug information turns those
/// addresses into source lines, and the line ranges CanMehHarness regions
/// prints turn those lines into region ids, which is how CanMehSearch draws a
/// region in proportion to the samples it took rather than at random.
///
/// This program exists so that the only code a profiler can interrupt is the
/// decoder. It opens one shared library and calls canmehDecode, and the
/// process contains no second library, no oracle and no comparison.
///
/// CanMehBench cannot do that. CanMehBench opens two libraries and, before it
/// times anything, can reject the pair at stage load, baseline, correctness,
/// profile or threading, so a profiler interrupting CanMehBench would record
/// addresses in all of those checks as well as in the decoder.
///
/// Google Benchmark decides how long the decoding lasts, through
/// --benchmark_min_time. A hand-written loop over a wall clock would be a worse
/// copy of what that flag already does, and Google Benchmark also reports how
/// many decodes it ran, which is what says whether a sample count means
/// anything. Size that flag well above startup: a four second window spent most
/// of itself inside benchmark::CPUInfo::CPUInfo() and left few decoder samples.
#include <dlfcn.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <CLI11.hpp>
#include <benchmark/benchmark.h>
#include <nlohmann/json.hpp>

#include "utilities/Portability.hpp"

using utilities::bitCast;

namespace {

using LoadFunction = int (*)(const char*);
using ValueFunction = unsigned long long (*)();

/// Prints one JSON object on stdout and exits zero, matching how CanMehBench
/// reports a refusal, so whatever started this program reads one shape either
/// way. nlohmann does the escaping, because a dlerror string containing a
/// quote would otherwise produce JSON the reader cannot parse.
[[noreturn]] void reportAndExit(const nlohmann::json& what) {
  std::cout << what.dump() << "\n" << std::flush;
  std::exit(0);
}

template <typename Function>
Function entryPointOrExit(void* handle, const char* name) {
  const auto symbol = dlsym(handle, name);
  if (symbol == nullptr)
    reportAndExit({{"ok", false}, {"error", std::string{"the library is missing "} + name}});
  return bitCast<Function>(symbol);
}

std::string hexDigits(unsigned long long value) {
  auto text = std::ostringstream{};
  text << std::hex << std::setw(16) << std::setfill('0') << value;
  return text.str();
}

void timeOneDecode(benchmark::State& state, ValueFunction decode,
                    unsigned long long bytesPerDecode) {
  for (auto _ : state) benchmark::DoNotOptimize(decode());
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(bytesPerDecode));
}

}  // namespace

int main(int argc, char** argv) {
  auto app = CLI::App{
      "Decodes repeatedly out of one shared library, for a profiler to interrupt."};
  // Google Benchmark takes the rest of the command line, so unrecognized
  // options reach benchmark::Initialize rather than ending the run. A typo is
  // then reported as a typo.
  app.allow_extras();

  auto library = std::string{};
  auto data = std::string{};
  app.add_option("--library", library, "the shared library to decode out of")->required();
  app.add_option("--data", data, "directory of .fit files")->required();
  CLI11_PARSE(app, argc, argv);

  // app.remaining() returns the vector by value, so it has to be held in a
  // named variable: pointers into a temporary's strings dangle the moment the
  // loop ends, and Google Benchmark then reports garbage as unrecognized
  // flags.
  const auto extras = app.remaining();
  auto leftovers = std::vector<char*>{argv[0]};
  for (const std::string& extra : extras)
    leftovers.push_back(const_cast<char*>(extra.c_str()));
  auto leftoverCount = int{static_cast<int>(leftovers.size())};
  benchmark::Initialize(&leftoverCount, leftovers.data());
  if (benchmark::ReportUnrecognizedArguments(leftoverCount, leftovers.data())) return 1;

  // RTLD_LOCAL, so this program loads the library the way CanMehBench loads
  // it, and a profiler records addresses within the same code layout.
  auto* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) reportAndExit({{"ok", false}, {"error", std::string{dlerror()}}});

  const auto loadDataFiles = entryPointOrExit<LoadFunction>(handle, "canmehLoadDataFiles");
  const auto decode = entryPointOrExit<ValueFunction>(handle, "canmehDecode");
  const auto byteCount = entryPointOrExit<ValueFunction>(handle, "canmehByteCount");

  const auto fileCount = int{loadDataFiles(data.c_str())};
  if (fileCount < 0)
    reportAndExit({{"ok", false}, {"error", "cannot read the data directory " + data}});
  if (fileCount == 0)
    reportAndExit({{"ok", false}, {"error", "no .fit files in " + data}});

  // One decode before timing, so the checksum this program prints is the one a
  // reader compares against what CanMehBench reports. If this library decoded
  // something else, the samples would be real and the source lines they name
  // would describe work no measurement ever timed.
  const auto checksum = decode();
  const auto bytesPerDecode = byteCount();

  benchmark::RegisterBenchmark("decode", timeOneDecode, decode, bytesPerDecode)
      ->UseRealTime();
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  std::cout << nlohmann::json{{"ok", true},
                              {"library", library},
                              {"files", fileCount},
                              {"bytes", bytesPerDecode},
                              {"checksum", hexDigits(checksum)}}
                   .dump()
            << "\n";
  return 0;
}
