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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
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
  /// Both measure-patch calls reported significant.
  bool significant = false;  /// Both measure-patch calls reported significant and the kept measurement's effect
  /// reached measure-patch's --min-effect. CanMehSearch admits a candidate on this
  /// field and not on significant, because signFlipPValue reads only whether
  /// the processes agree in direction: two candidates were called significant
  /// at 0.32 and 0.34 percent, far below what this instrument can resolve.
  ///
  /// measure-patch computed this from the whole accumulated patch against the
  /// reference, which is the only pair measure-patch measured. A child inherits its
  /// parent's rewrites, so a descendant of a patch that already clears
  /// --min-effect clears it too whatever its own rewrite did: 30 of 40
  /// generations of one cluster run reported true, from one discovery and
  /// 29 inheritors. Read improvedOnParent for the one generation.
  bool significantAtMinEffect = false;
};

struct RegionStats {
  int attempts = 0;
  int accepted = 0;
  // The largest ratio improvement this region produced against the parent the
  // candidate came from, counting only a measurement measure-patch called significant.
  // A difference of two ratios, not a duration.
  double bestGain = 0.0;
};

json runHarnessCommand(const Options& options, std::vector<std::string> args,
                        double timeoutSeconds = 900);

/// The decision: did this candidate decode faster than the patch it was spliced
/// onto?
///
/// Phase 1 of README.md requires you to run measure-patch with --no-decision,
/// and CanMehHarness then sends neither `significant` nor `ratio`, so your
/// version computes the decision from reply["timings"] alone. That computation
/// is the assignment, which is why the rule is one function rather than an
/// expression inside the loop.
bool candidateIsFaster(const json& reply, double minEffect);

struct CodeBlock {
  std::string tag;
  std::string text;
};

std::vector<CodeBlock> codeBlocks(const std::string& reply);

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
void splitReply(const std::string& reply, std::string* code, std::string* includes);

std::string systemPrompt();

std::string userPrompt(const Region& region, const std::string& currentText,
                        double parentRatio, double bestRatio, double baselineMs,
                        const std::string& failure, bool hints);

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

int main(int argc, char** argv);
