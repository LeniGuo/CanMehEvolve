/// @file
/// utilities/portability -- what this project needs because it builds as C++17,
/// because it runs on both macOS and Linux, and because a checksum has to come
/// out identical on both.
///
/// This header declares two kinds of thing. The first is what each platform
/// calls a shared library: macOS and Linux give one different suffixes and need
/// different flags to record its name. The second is how to compute a value
/// bit-exactly: a decoded checksum and a generated data set have to come out
/// identical on every machine and under every standard library, which rules out
/// std::hash and the standard library's distributions, whose results the
/// standard leaves to the implementation.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace utilities {

/// std::bit_cast, which arrived in C++20 while this project builds as C++17.
/// std::memcpy is the one way to reinterpret an object's bytes that does not
/// break the aliasing rules.
template <typename To, typename From>
To bitCast(const From& from) {
  static_assert(sizeof(To) == sizeof(From));
  static_assert(std::is_trivially_copyable<To>::value);
  static_assert(std::is_trivially_copyable<From>::value);
  auto to = To{};
  std::memcpy(&to, &from, sizeof to);
  return to;
}

/// FNV-1a, 64-bit. hash starts at FNV-1a's published 64-bit offset basis and
/// foldByte multiplies by its published prime, so two machines that fold the
/// same bytes in the same order compute the same hash. std::hash fails here for
/// two separate reasons: the standard fixes no value for std::hash, so two standard libraries may disagree, and the
/// standard requires std::hash to hash equal values equally, so std::hash
/// would hide a decoder that began returning -0.0 where it returned 0.0.
struct Digest {
  uint64_t hash = 1469598103934665603ULL;

  void foldByte(uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  }

  void foldBits(uint64_t bitPattern) {
    for (auto i = int{0}; i < 8; ++i) foldByte(static_cast<uint8_t>(bitPattern >> (8 * i)));
  }

  /// Folds the double's bit pattern rather than the double's value, so that
  /// 0.0 and -0.0 fold to different digests even though the two compare equal.
  void foldDouble(double number) { foldBits(bitCast<uint64_t>(number)); }
};

/// xorshift32, so that a generated data set comes out byte-identical on every
/// machine and every run. The standard library's engines are specified
/// precisely enough to give that guarantee as well, but the standard library's
/// distributions are not: std::uniform_int_distribution may map one engine
/// output to different values under different standard libraries. Writing out
/// the generator is what avoids depending on a distribution.
/// state must not be zero: xorshift32 maps 0 to 0, so a DeterministicRandom
/// left at the default never advances.
struct DeterministicRandom {
  uint32_t state = 0;

  uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
};

/// The suffix this platform gives a shared library. CanMehHarness appends
/// sharedLibrarySuffix to the name of each library it links, and CanMehBench
/// passes the resulting path to dlopen.
#ifdef __APPLE__
constexpr const auto sharedLibrarySuffix = ".dylib";
#else
constexpr const auto sharedLibrarySuffix = ".so";
#endif

/// The linker flags that record claimedName in the library as the name the
/// loader matches a dlopen against.
///
/// dyld on macOS and the glibc loader on Linux both answer a dlopen from the
/// list of images already loaded, and both match that list by the name each
/// image claims: macOS by the LC_ID_DYLIB an image records, Linux by an image's
/// SONAME. Each loader registers an image under the name that image claims and
/// answers a later dlopen of that name with the registered image. CanMehBench
/// opens the --base slot first, so a reference library that claims the
/// candidate's path makes the loader answer the second dlopen with the
/// reference and the candidate's code never runs. The reverse direction loads
/// two images. That is why CanMehHarness and CMake each name a library after
/// the path they link it to, and why CMake links the shared-install-name pair's
/// reference library under the candidate library's path on purpose, so
/// CanMehCalibrate can check that CanMehBench refuses that pair.
///
/// We measured the macOS case on Darwin 27 with a three-line dlopen program:
/// the two canmehDecode pointers came back equal. Nobody has run the Linux
/// case; the same outcome follows from how the glibc loader matches a dlopen
/// against the loaded-object list.
inline std::vector<std::string> libraryNameFlags(const std::string& claimedName) {
#ifdef __APPLE__
  return {"-Wl,-install_name," + claimedName};
#else
  return {"-Wl,-soname," + claimedName};
#endif
}

}  // namespace utilities
