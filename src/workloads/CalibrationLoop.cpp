/// @file
/// workloads/CalibrationLoop -- the source CMake compiles into pairs of
/// libraries whose true time ratio is the quotient of two compiled work counts.
///
/// CanMehBench reaches the six entry points below through dlsym and includes no
/// SDK header, so this file compiles into a pair of libraries CanMehBench
/// accepts in place of two SDK builds. CMake compiles this file twice per pair,
/// once at each work count, and links each compilation into a shared library
/// under build/calibration, so a student can score a measurement design
/// against an answer known by construction.
///
/// What a calibration pair cannot establish: nobody has compared this work
/// loop's noise against the SDK decode's, this work loop allocates nothing
/// inside the timed region, and a work count scales every part of a decode
/// rather than making one part of many faster. A student who scores a decision
/// rule on these pairs still has to run that rule against an inert patch on
/// the SDK decode.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "utilities/Portability.hpp"

using utilities::Digest;
using utilities::DeterministicRandom;

#define WORKLOAD_EXPORT __attribute__((visibility("default")))

/// CMake passes these three on the compile command line of every calibration
/// pair library, so no target reaches a fallback below. The fallback values
/// let a person compile this file by hand without CMake, and the fallback work
/// count repeats CANMEH_CALIBRATION_REFERENCE_UNITS, so a hand compilation
/// performs the same work as the reference library of a pair.
#ifndef CANMEH_CALIBRATION_WORK_UNITS
#define CANMEH_CALIBRATION_WORK_UNITS 20000
#endif
#ifndef CANMEH_CALIBRATION_BUFFER_MIB
#define CANMEH_CALIBRATION_BUFFER_MIB 16
#endif
#ifndef CANMEH_CALIBRATION_DIGEST
#define CANMEH_CALIBRATION_DIGEST 0x43616c69627231ULL
#endif

namespace workloads {
// Unnamed inside workloads, so each of the two libraries CanMehBench loads keeps
// its own buffer and counters even if a build ever drops -fvisibility=hidden.
namespace {

/// The work count is read through a volatile, never used as a literal. A
/// literal lets the compiler specialize the work loop to that count: two
/// counts can unroll and schedule differently, and the quotient of the two
/// counts then stops being the true ratio by an amount nobody has measured.
/// Reading the count through a volatile keeps it out of the compiler's hands
/// at compile time. Nothing in the language guarantees the two compilations
/// emit identical code, so CanMehCalibrate compares the two libraries' file
/// sizes afterwards.
volatile const auto calibrationWorkUnits = uint64_t{CANMEH_CALIBRATION_WORK_UNITS};

constexpr auto stepsPerWorkUnit = int{256};
constexpr auto bufferWords =
    size_t{(size_t{CANMEH_CALIBRATION_BUFFER_MIB} << 20) / sizeof(uint32_t)};
static_assert((bufferWords & (bufferWords - 1)) == 0,
              "bufferWords must be a power of two, because oneWorkUnit masks the "
              "index with bufferWords - 1");

auto buffer = std::vector<uint32_t>{};
auto loadedByteCount = uint64_t{0};

/// canmehDecode assigns the accumulator here once per decode. calibrationSink
/// is volatile, so the compiler cannot delete a work loop whose result nothing
/// else reads.
volatile auto calibrationSink = uint64_t{0};

/// One unit of work: scattered read-modify-writes over a buffer of
/// CANMEH_CALIBRATION_BUFFER_MIB, 16 MiB by default against the 6 MiB L2 of
/// the machine we measured on. The dependence through memory is what makes the
/// loop unremovable without a volatile inside it, and the scattered index -- a
/// fixed function of the unit number, so both libraries of a pair touch the
/// same addresses -- is what puts the time in the memory hierarchy rather than
/// in the arithmetic units, which is where the SDK decode spends a large part
/// of its own time.
///
/// DeterministicRandom never advances from state 0, so oneWorkUnit sets the
/// low bit of the unit number.
uint64_t oneWorkUnit(uint32_t unitNumber) {
  auto random = DeterministicRandom{unitNumber | 1u};
  auto accumulator = uint64_t{0};
  for (auto step = int{0}; step < stepsPerWorkUnit; ++step) {
    const auto at = size_t{random.next() & (bufferWords - 1)};
    accumulator += buffer[at];
    buffer[at] = static_cast<uint32_t>(accumulator >> 7);
  }
  return accumulator;
}

}  // namespace
}  // namespace workloads

using workloads::buffer;
using workloads::bufferWords;
using workloads::calibrationSink;
using workloads::calibrationWorkUnits;
using workloads::loadedByteCount;
using workloads::oneWorkUnit;
using workloads::stepsPerWorkUnit;

extern "C" {

/// canmehLoadDataFiles counts the same .fit files workloads/SdkDecode.cpp reads, so
/// CanMehBench's data check accepts a calibration pair, then sizes and fills
/// the buffer from a fixed seed. The fill does not read the files, so both
/// libraries of a pair start from an identical buffer and the data has no part
/// in either library's timing.
///
/// No calibration timing reads a byte of those files, so canmehLoadDataFiles
/// asks the filesystem for each size rather than opening any file:
/// workloads/SdkDecode.cpp keeps the bytes because workloads/SdkDecode.cpp decodes them,
/// and copying the 900 kB data set into each of a pair's two libraries would do
/// work that no calibration timing reads.
/// @return how many .fit files canmehLoadDataFiles found under dir, zero for a
///         directory with none, or -1 when the directory cannot be read.
WORKLOAD_EXPORT int canmehLoadDataFiles(const char* dir) {
  loadedByteCount = 0;
  auto names = std::vector<std::filesystem::path>{};
  auto errorCode = std::error_code{};
  for (const auto& entry : std::filesystem::directory_iterator(dir, errorCode))
    if (entry.path().extension() == ".fit") names.push_back(entry.path());
  if (errorCode) return -1;
  std::sort(names.begin(), names.end());

  for (const std::filesystem::path& name : names)
    loadedByteCount += std::filesystem::file_size(name, errorCode);
  // resize zero-fills on the first call and does nothing on a later one, and
  // the loop below then writes every word, so a second call does not re-zero
  // CANMEH_CALIBRATION_BUFFER_MIB of memory. CanMehBench calls
  // canmehLoadDataFiles once per library, so only the first case arises today.
  buffer.resize(bufferWords);
  auto random = DeterministicRandom{0x9E3779B9u};
  for (auto& word : buffer) word = random.next();
  return static_cast<int>(names.size());
}

/// Runs the compiled number of work units and returns a constant. The digest
/// has to be constant: CanMehBench requires both libraries of a pair to decode
/// to the same checksum, and CMake compiles a pair's two libraries at
/// different work counts in every case but null.
WORKLOAD_EXPORT unsigned long long canmehDecode(void) {
  auto accumulator = uint64_t{0};
  const auto units = uint64_t{calibrationWorkUnits};
  for (auto unit = uint64_t{0}; unit < units; ++unit)
    accumulator += oneWorkUnit(static_cast<uint32_t>(unit));
  calibrationSink = accumulator;
  return CANMEH_CALIBRATION_DIGEST;
}

/// The work count CMake compiled in. CanMehBench prints the reference
/// library's count on stderr, so a reader can tell which pair CanMehBench
/// opened.
WORKLOAD_EXPORT unsigned long long canmehMessageCount(void) {
  return calibrationWorkUnits;
}

WORKLOAD_EXPORT unsigned long long canmehFieldCount(void) {
  return calibrationWorkUnits * static_cast<uint64_t>(stepsPerWorkUnit);
}

WORKLOAD_EXPORT unsigned long long canmehByteCount(void) { return loadedByteCount; }

/// canmehComputeProfileDigest folds a fixed sequence that does not read the
/// work count, so both libraries of a pair return the same value.
/// workloads/SdkDecode.cpp sweeps every message number instead, which would add
/// seconds to each of the twelve pairs CanMehCalibrate measures.
WORKLOAD_EXPORT unsigned long long canmehComputeProfileDigest(void) {
  auto digest = Digest{};
  auto random = DeterministicRandom{0x01000193u};
  for (auto step = int{0}; step < 4096; ++step) digest.foldBits(random.next());
  return digest.hash;
}

}  // extern "C"
