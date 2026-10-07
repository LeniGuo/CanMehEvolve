/// @file
/// workloads/SdkDecode -- the decode CanMehBench times when CanMehHarness
/// measures a patch.
///
/// CanMehHarness init compiles a copy of this file into each build of the SDK
/// and links that build as a shared library, so every library exports the C
/// entry points below. CanMehBench loads two such libraries with RTLD_LOCAL,
/// and CanMehHarness compiles each build with -fvisibility=hidden, so the two
/// builds' fit:: symbols and static profile tables do not collide.
/// CanMehBench registers one benchmark function per library, and CanMehHarness
/// passes --benchmark_enable_random_interleaving=true, so Google Benchmark
/// interleaves the two functions' repetitions.
///
/// canmehLoadDataFiles reads the activity files into memory, and canmehDecode
/// decodes the bytes canmehLoadDataFiles read and folds every decoded value
/// into a checksum, so no timing includes reading the files from disk.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "utilities/Portability.hpp"

using utilities::Digest;
#include "fit_decode.hpp"
#include "fit_developer_field.hpp"
#include "fit_mesg.hpp"
#include "fit_mesg_listener.hpp"
#include "fit_profile.hpp"

#define WORKLOAD_EXPORT __attribute__((visibility("default")))

namespace workloads {
// Unnamed inside workloads, so each of the two libraries CanMehBench loads keeps
// its own copies of the counters below even if a build ever drops
// -fvisibility=hidden.
namespace {

class DecodedValueDigest : public fit::MesgListener {
 public:
  Digest digest;
  uint64_t messages = 0;
  uint64_t fields = 0;

  void OnMesg(fit::Mesg& message) override {
    ++messages;
    digest.foldBits(message.GetNum());
    digest.foldBits(static_cast<uint64_t>(message.GetNumFields()));

    for (auto i = FIT_UINT16{0}; i < static_cast<FIT_UINT16>(message.GetNumFields()); ++i) {
      const auto field = message.GetFieldByIndex(i);
      if (field == nullptr) {
        digest.foldByte(0xFE);
      } else {
        ++fields;
        foldField(*field);
      }
    }
    for (const fit::DeveloperField& developerField : message.GetDeveloperFields()) {
      ++fields;
      foldField(developerField);
    }
  }

 private:
  void foldField(const fit::FieldBase& field) {
    digest.foldBits(field.GetNum());
    digest.foldBits(field.GetType());
    const auto count = FIT_UINT8{field.GetNumValues()};
    digest.foldBits(count);
    if (!field.IsValid()) {
      digest.foldByte(0xFD);
    } else {
      for (auto v = FIT_UINT8{0}; v < count; ++v) {
        if (field.GetType() == FIT_BASE_TYPE_STRING) {
          const auto s = FIT_WSTRING{field.GetSTRINGValue(v)};
          for (wchar_t c : s) digest.foldBits(static_cast<uint64_t>(c));
          digest.foldByte(0x00);
        } else {
          digest.foldDouble(field.GetFLOAT64Value(v));
        }
      }
    }
  }
};

auto loadedFiles = std::vector<std::string>{};
auto loadedByteCount = uint64_t{0};
auto decodedMessageCount = uint64_t{0};
auto decodedFieldCount = uint64_t{0};

}  // namespace
}  // namespace workloads

using workloads::DecodedValueDigest;
using workloads::decodedFieldCount;
using workloads::decodedMessageCount;
using workloads::loadedByteCount;
using workloads::loadedFiles;

extern "C" {

/// Folds what Profile::GetMesg, GetFieldIndex and GetField answer for every
/// message number from 0 to 65535: every field number from 0 to 255 for a
/// message number Profile::GetMesg answers, and the nine numbers in
/// probeFieldNumbers for a message number it does not. So
/// canmehComputeProfileDigest folds answers the four activity files never
/// reach. Values, never pointers: an address in one library's static tables
/// means nothing to the other. canmehComputeProfileDigest sweeps every
/// message number because a table a patch indexed by a message number rather
/// than by a message index answers correctly for the message numbers the four
/// activity files contain and wrongly above 255.
WORKLOAD_EXPORT unsigned long long canmehComputeProfileDigest(void) {
  static constexpr auto probeFieldNumbers = std::array<FIT_UINT8, 9>{0, 1, 2, 127, 128, 200, 253, 254, 255};
  constexpr auto absentMarker = 0xFFFFFFFFULL;

  auto digest = Digest{};
  for (auto number = uint32_t{0}; number <= 0xFFFF; ++number) {
    const auto messageNumber = FIT_UINT16{static_cast<FIT_UINT16>(number)};
    const auto profileMessage = fit::Profile::GetMesg(messageNumber);
    digest.foldBits(profileMessage == FIT_NULL ? absentMarker : profileMessage->num);
    digest.foldBits(profileMessage == FIT_NULL ? absentMarker : profileMessage->numFields);

    const auto probes = static_cast<uint32_t>(
        (profileMessage == FIT_NULL) ? probeFieldNumbers.size() : 256);
    for (auto i = uint32_t{0}; i < probes; ++i) {
      const auto fieldNumber = static_cast<FIT_UINT8>(
          (profileMessage == FIT_NULL) ? probeFieldNumbers[i] : static_cast<FIT_UINT8>(i));
      digest.foldBits(fit::Profile::GetFieldIndex(messageNumber, fieldNumber));
      const auto field = fit::Profile::GetField(messageNumber, fieldNumber);
      digest.foldBits(field == FIT_NULL ? absentMarker : field->num);
      digest.foldBits(field == FIT_NULL ? absentMarker : field->type);
    }
  }
  return digest.hash;
}

/// @return how many .fit files canmehLoadDataFiles read, which is zero for a
///         directory with none, or -1 when the directory cannot be read at
///         all. An unreadable path and an empty directory are different
///         mistakes, so each one gets its own answer.
WORKLOAD_EXPORT int canmehLoadDataFiles(const char* dir) {
  loadedFiles.clear();
  loadedByteCount = 0;

  auto names = std::vector<std::filesystem::path>{};
  auto errorCode = std::error_code{};
  for (const auto& entry : std::filesystem::directory_iterator(dir, errorCode))
    if (entry.path().extension() == ".fit") names.push_back(entry.path());
  if (errorCode) return -1;
  // std::filesystem::directory_iterator yields entries in whatever order the
  // filesystem returns them, and std::sort fixes the order canmehDecode folds
  // the files in, so the checksum does not depend on that order.
  std::sort(names.begin(), names.end());

  for (const std::filesystem::path& name : names) {
    auto file = std::ifstream{name, std::ios::binary};
    auto contents = std::ostringstream{};
    contents << file.rdbuf();
    loadedFiles.push_back(contents.str());
    loadedByteCount += loadedFiles.back().size();
  }
  return static_cast<int>(loadedFiles.size());
}

WORKLOAD_EXPORT unsigned long long canmehDecode(void) {
  auto listener = DecodedValueDigest{};
  for (const std::string& bytes : loadedFiles) {
    std::istringstream stream(bytes, std::ios::binary);
    auto decode = fit::Decode{};
    if (!decode.Read(stream, listener)) return 0;
  }
  decodedMessageCount = listener.messages;
  decodedFieldCount = listener.fields;
  return listener.digest.hash;
}

WORKLOAD_EXPORT unsigned long long canmehMessageCount(void) { return decodedMessageCount; }
WORKLOAD_EXPORT unsigned long long canmehFieldCount(void) { return decodedFieldCount; }
WORKLOAD_EXPORT unsigned long long canmehByteCount(void) { return loadedByteCount; }



}  // extern "C"
