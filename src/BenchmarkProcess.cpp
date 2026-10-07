/// @file
/// CanMehBench -- the benchmark process.
///
/// CanMehBench loads two builds of the FIT SDK into one process, each build as
/// a separate shared library, and registers one benchmark function per build.
/// Google Benchmark then interleaves the repetitions of those two functions.
/// Both decoders read the same bytes through the same page cache, and one
/// clock times both decoders, so dividing the candidate's median time by the
/// reference's removes the drift between two separately timed processes. Load
/// order still biases one slot, which is why CanMehBench takes --swap and
/// CanMehHarness loads the candidate first in half of its processes.
///
/// Before timing anything, CanMehBench checks that both libraries fold the
/// decoded values to the same 64-bit digest, that neither library's digest
/// changes on a second decode, that the two libraries answer every profile
/// lookup identically, and that neither library ran work on a second thread.
///
/// CLI11 generates the usage text from the option definitions in main.
/// benchmark::Initialize parses every flag CLI11 did not declare, and main
/// returns 1 when benchmark::ReportUnrecognizedArguments finds one Google
/// Benchmark does not recognize.
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
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

/// Sixteen lowercase hex digits, which is how every checksum and digest in
/// this project is quoted.
std::string hexDigits(unsigned long long value) {
  auto text = std::ostringstream{};
  text << std::hex << std::setw(16) << std::setfill('0') << value;
  return text.str();
}

/// The entry points workloads/SdkDecode.cpp exports, one set per loaded build. These
/// are plain function pointers rather than std::function, because
/// timeDecodes calls decode() inside the timed loop and must not add an
/// extra indirection to the thing timeDecodes measures.
struct DecoderLibrary {
  using LoadFunction = int (*)(const char*);
  using ValueFunction = unsigned long long (*)();

  void* handle = nullptr;
  LoadFunction loadDataFiles = nullptr;
  ValueFunction decode = nullptr;
  ValueFunction messageCount = nullptr;
  ValueFunction fieldCount = nullptr;
  ValueFunction byteCount = nullptr;
  ValueFunction computeProfileDigest = nullptr;
};

[[noreturn]] void reportRejectionAndExit(const std::string& stage, const std::string& detail) {
  // A rejected candidate is a normal outcome rather than a crash, so this
  // exits zero and leaves the reason on stdout for CanMehHarness to read.
  // nlohmann::json does the escaping, because a compiler message or a
  // dlerror string containing a quote would otherwise produce JSON that
  // CanMehHarness cannot parse.
  std::cout << nlohmann::json{{"ok", false}, {"stage", stage}, {"error", detail}}.dump()
            << "\n"
            << std::flush;
  std::exit(0);
}

template <typename Function>
Function entryPointOrFail(void* handle, const char* name, const std::string& what) {
  const auto symbol = dlsym(handle, name);
  if (symbol == nullptr) reportRejectionAndExit("load", what + " is missing " + name);
  return bitCast<Function>(symbol);
}

/// RTLD_LOCAL is what lets two builds of one library coexist in a process:
/// each library keeps a private copy of the SDK's symbols and a private set of
/// static profile tables.
DecoderLibrary openDecoderLibrary(const std::string& path, const std::string& role) {
  auto* const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) reportRejectionAndExit("load", role + ": " + dlerror());

  auto library = DecoderLibrary{};
  library.handle = handle;
  library.loadDataFiles =
      entryPointOrFail<DecoderLibrary::LoadFunction>(handle, "canmehLoadDataFiles", role);
  library.decode =
      entryPointOrFail<DecoderLibrary::ValueFunction>(handle, "canmehDecode", role);
  library.messageCount =
      entryPointOrFail<DecoderLibrary::ValueFunction>(handle, "canmehMessageCount", role);
  library.fieldCount =
      entryPointOrFail<DecoderLibrary::ValueFunction>(handle, "canmehFieldCount", role);
  library.byteCount =
      entryPointOrFail<DecoderLibrary::ValueFunction>(handle, "canmehByteCount", role);
  library.computeProfileDigest =
      entryPointOrFail<DecoderLibrary::ValueFunction>(handle, "canmehComputeProfileDigest", role);
  return library;
}

/// The ratio of processor time to wall-clock time over one decode.
///
/// cpuToWallRatioOfOneDecode times one decode from outside the library.
/// workloads/SdkDecode.cpp used to export this ratio and CanMehBench used the
/// number the library returned. A candidate may rewrite workloads/SdkDecode.cpp,
/// and CanMehBench compared that returned number against nothing, so a
/// candidate that returned 1.0 while running threads passed. std::clock
/// counts the processor time of every thread in this process, so
/// cpuToWallRatioOfOneDecode returns a value above one for a decoder that
/// runs work on a second thread.
double cpuToWallRatioOfOneDecode(DecoderLibrary::ValueFunction decode) {
  const auto cpuStart = std::clock();
  const auto wallStart = std::chrono::steady_clock::now();
  decode();
  const auto wallEnd = std::chrono::steady_clock::now();
  const auto cpuEnd = std::clock();

  const auto wall = double{std::chrono::duration<double>(wallEnd - wallStart).count()};
  const auto cpu = double{static_cast<double>(cpuEnd - cpuStart) / CLOCKS_PER_SEC};
  return wall > 0.001 ? cpu / wall : 1.0;
}

auto baseSlot = DecoderLibrary{};
auto candidateSlot = DecoderLibrary{};

void timeDecodes(benchmark::State& state, DecoderLibrary::ValueFunction decode,
                    unsigned long long bytesPerDecode) {
  for (auto _ : state) benchmark::DoNotOptimize(decode());
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(bytesPerDecode));
}

}  // namespace

int main(int argc, char** argv) {
  auto app = CLI::App{"Times two builds of the FIT SDK against each other in one process."};
  app.allow_extras();

  auto data = std::string{"data"};
  auto baseLibraryPath = std::string{};
  auto candidateLibraryPath = std::string{};
  auto swapped = bool{false};
  app.add_option("--data", data, "directory each loaded library reads .fit files from")->capture_default_str();
  app.add_option("--base", baseLibraryPath, "library CanMehBench loads first")->required();
  app.add_option("--cand", candidateLibraryPath, "library CanMehBench loads second")->required();
  // The slot (--base, --cand) fixes load order, and load order fixes address
  // placement and which image's static data the allocator hands out first. The
  // role fixes which name CanMehBench gives each timing series and which
  // library the checksum, second-decode, profile-digest and processor-time
  // checks read. Because CanMehBench labels each series by role in either load
  // order, CanMehHarness can put the candidate in the --base slot on alternate
  // processes, so load order is not a constant difference between the
  // reference's timings and the candidate's.
  app.add_flag("--swap", swapped,
               "CanMehBench treats the library in the --base slot as the candidate");
  CLI11_PARSE(app, argc, argv);
  // benchmark::Initialize parses every argument CLI11 did not declare, and
  // parses them before any check below runs, so
  // benchmark::ReportUnrecognizedArguments names a misspelled option and main
  // returns 1, instead of one of the four checks failing later on a default
  // nobody asked for.
  auto passThrough = std::vector<std::string>{argv[0]};
  for (const auto& extra : app.remaining(true)) passThrough.push_back(extra);
  auto passThroughArgv = std::vector<char*>{};
  for (auto& entry : passThrough) passThroughArgv.push_back(entry.data());
  passThroughArgv.push_back(nullptr);
  auto passThroughArgc = static_cast<int>(passThrough.size());
  benchmark::Initialize(&passThroughArgc, passThroughArgv.data());
  if (benchmark::ReportUnrecognizedArguments(passThroughArgc, passThroughArgv.data())) return 1;

  baseSlot = openDecoderLibrary(baseLibraryPath,
                                swapped ? "the candidate library" : "the reference library");
  candidateSlot = openDecoderLibrary(candidateLibraryPath,
                                     swapped ? "the reference library" : "the candidate library");

  // dlopen hands back an already-loaded image when the path dlopen is given
  // matches the install name an earlier library claimed, which leaves the
  // candidate slot holding the reference library: identical timings, and a
  // correctness check that accepts anything. Neither CanMehHarness nor
  // CanMehSearch can tell one image loaded twice from an honest null result, so
  // CanMehBench compares the two decode pointers here.
  if (baseSlot.decode == candidateSlot.decode)
    reportRejectionAndExit("load",                           "both libraries resolved to one image, so CanMehBench would measure "
                           "one build twice; check that the two libraries do not claim one "
                           "name (LC_ID_DYLIB on macOS, SONAME on Linux)");

  DecoderLibrary* candidate = swapped ? &baseSlot : &candidateSlot;
  DecoderLibrary* reference = swapped ? &candidateSlot : &baseSlot;

  const auto referenceFileCount = reference->loadDataFiles(data.c_str());
  const auto candidateFileCount = candidate->loadDataFiles(data.c_str());
  if (referenceFileCount < 0 || candidateFileCount < 0)
    reportRejectionAndExit("data", "cannot read the data directory " + data);
  if (referenceFileCount == 0 || candidateFileCount == 0)
    reportRejectionAndExit("data", "no .fit files in " + data);

  const auto expectedChecksum = reference->decode();
  const auto actualChecksum = candidate->decode();
  if (expectedChecksum == 0)
    reportRejectionAndExit("baseline", "the reference library failed to decode the data");
  if (actualChecksum != expectedChecksum)
    reportRejectionAndExit("correctness", "checksum " + hexDigits(actualChecksum) +
                                              " does not match the reference " +
                                              hexDigits(expectedChecksum) +
                                              "; the decoder produced different values");
  // Each library decodes a second time so that both have decoded the same
  // number of times before Google Benchmark starts the clock: a library that
  // decoded more often than the other would reach the timed loop with the
  // warmer allocator and page cache of the two, which is a difference between
  // the two measurements that no patch caused. Note what the equal digest does
  // not establish: a candidate that returned a cached digest instead of
  // decoding would also pass.
  if (candidate->decode() != expectedChecksum)
    reportRejectionAndExit("correctness", "the candidate checksum changed between runs");
  if (reference->decode() != expectedChecksum)
    reportRejectionAndExit("correctness", "the reference checksum changed between runs");
  // A candidate whose lookup tables are too small reads out of bounds inside
  // its own canmehComputeProfileDigest and this process dies on a signal, so
  // CanMehBench writes a line to stderr before calling
  // canmehComputeProfileDigest.
  std::cerr << "CanMehBench is folding every Profile::GetMesg, GetFieldIndex and "
               "GetField answer; a candidate with undersized tables dies here\n"
            << std::flush;
  const auto expectedProfileDigest = reference->computeProfileDigest();
  const auto actualProfileDigest = candidate->computeProfileDigest();
  if (actualProfileDigest != expectedProfileDigest)
    reportRejectionAndExit("profile", "profile digest " + hexDigits(actualProfileDigest) +
                                         " does not match the reference " +
                                         hexDigits(expectedProfileDigest) +
                                         "; a lookup returns a different answer for some "
                                         "message or field number");
  // Each library decodes once more here, so that both libraries have decoded
  // the same number of times once Google Benchmark starts the clock.
  // CanMehBench rejects the pair whichever of the two libraries ran the
  // threads.
  const auto candidateCpuToWallRatio = cpuToWallRatioOfOneDecode(candidate->decode);
  const auto referenceCpuToWallRatio = cpuToWallRatioOfOneDecode(reference->decode);
  const auto cpuToWallRatio = std::max(candidateCpuToWallRatio, referenceCpuToWallRatio);
  if (cpuToWallRatio > 1.15) {
    auto ratio = std::ostringstream{};
    ratio << std::fixed << std::setprecision(2) << cpuToWallRatio;
    reportRejectionAndExit(        "threading", "processor time over wall-clock time reached " + ratio.str() +
                         " for one of the two libraries, above the 1.15 CanMehBench "
                         "accepts, so one library ran work on more than one thread");
  }  std::cerr << "the reference library read " << reference->byteCount()
            << " bytes and decoded " << reference->messageCount() << " messages, "
            << reference->fieldCount() << " fields, checksum "
            << hexDigits(expectedChecksum) << "\n"
            << std::flush;

  const auto bytesPerDecode = reference->byteCount();
  benchmark::RegisterBenchmark("reference", timeDecodes, reference->decode, bytesPerDecode)
      ->UseRealTime();
  benchmark::RegisterBenchmark("candidate", timeDecodes, candidate->decode, bytesPerDecode)
      ->UseRealTime();

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
