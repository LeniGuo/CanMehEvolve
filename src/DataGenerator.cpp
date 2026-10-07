/// @file
/// CanMehDataGenerator writes a fixed set of synthetic FIT files through the
/// encoder the FIT SDK ships.
///
/// CanMehDataGenerator writes the files into the directory named as its first
/// argument, or into ./data when the command line names none.
///
/// CanMehDataGenerator writes each file with a file id message, two timer event
/// messages, one record message per second, a lap message every lapLength
/// records, a session message and an activity message. long_run, intervals and
/// ride also get one developer field description and one developer field per
/// record; treadmill sets developerFields false and gets neither.
#include <cmath>
#include <array>
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <string>

#include "utilities/Portability.hpp"

using utilities::DeterministicRandom;
#include "fit_activity_mesg.hpp"
#include "fit_date_time.hpp"
#include "fit_developer_data_id_mesg.hpp"
#include "fit_developer_field.hpp"
#include "fit_encode.hpp"
#include "fit_event_mesg.hpp"
#include "fit_field_description_mesg.hpp"
#include "fit_file_id_mesg.hpp"
#include "fit_lap_mesg.hpp"
#include "fit_record_mesg.hpp"
#include "fit_session_mesg.hpp"

namespace {

constexpr auto twoPi = double{6.283185307179586};
constexpr auto semicirclesPerMeter = double{107.173};

struct ActivityParameters {
  const char* name;  int recordedSeconds;
  int laps;
  bool writesDeveloperFields;
  bool writesPosition;
};

constexpr auto activitySeed = uint32_t{0x5eed1234u};

void writeActivityFile(const ActivityParameters& parameters, const std::string& path,
               DeterministicRandom& random) {
  auto file = std::fstream{};
  file.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    std::cerr << "cannot open " << path << "\n";
    std::exit(1);
  }

  fit::Encode encode(fit::ProtocolVersion::V20);
  encode.Open(file);  const auto fixedStartTime = FIT_DATE_TIME{1000000000};
  fit::DateTime startTime(fixedStartTime);

  auto fileId = fit::FileIdMesg{};
  fileId.SetType(FIT_FILE_ACTIVITY);
  fileId.SetManufacturer(FIT_MANUFACTURER_DEVELOPMENT);
  fileId.SetProduct(1);
  fileId.SetTimeCreated(startTime.GetTimeStamp());
  fileId.SetSerialNumber(4242);
  encode.Write(fileId);

  auto developerDataId = fit::DeveloperDataIdMesg{};
  const auto applicationId = std::array<FIT_UINT8, 16>{0x03, 0x02, 0x01, 0x00, 0x05, 0x04,
                                                       0x07, 0x06, 0x08, 0x09, 0x0a, 0x0b,
                                                       0x0c, 0x0d, 0x0e, 0x0f};
  for (auto i = FIT_UINT8{0}; i < applicationId.size(); ++i)
    developerDataId.SetApplicationId(i, applicationId[i]);
  developerDataId.SetDeveloperDataIndex(0);
  developerDataId.SetApplicationVersion(110);
  encode.Write(developerDataId);

  auto formPowerDescription = fit::FieldDescriptionMesg{};
  formPowerDescription.SetDeveloperDataIndex(0);
  formPowerDescription.SetFieldDefinitionNumber(0);
  formPowerDescription.SetFitBaseTypeId(FIT_BASE_TYPE_UINT16);
  formPowerDescription.SetFieldName(0, L"Form Power");
  formPowerDescription.SetUnits(0, L"W");
  formPowerDescription.SetNativeMesgNum(FIT_MESG_NUM_RECORD);
  if (parameters.writesDeveloperFields) encode.Write(formPowerDescription);

  auto timerStart = fit::EventMesg{};
  timerStart.SetTimestamp(startTime.GetTimeStamp());
  timerStart.SetEvent(FIT_EVENT_TIMER);
  timerStart.SetEventType(FIT_EVENT_TYPE_START);
  encode.Write(timerStart);

  fit::DateTime timestamp(startTime);
  const auto lapLength = int{parameters.recordedSeconds / (parameters.laps > 0 ? parameters.laps : 1)};
  auto distance = double{0.0};

  for (auto i = int{0}; i < parameters.recordedSeconds; ++i) {
    // speed drifts rather than staying flat, so the encoder writes values of
    // varying byte width.
    //
    // Every cast of a sine-derived double below rounds rather than truncates,
    // and CMake compiles this file with -ffp-contract=off. Truncating made one
    // byte of every file depend on whether the compiler fused a multiply-add:
    // at i = 1300 the exact value of 140 + 20 * sin is 150 - 65/2^52, so one
    // rounding lands just past half an ulp below 150 and truncates to 149,
    // while two roundings tie at half an ulp and round to even, giving 150.
    // Apple clang fused and wrote 149; the cluster's gcc did not and wrote
    // 150, so one data set decoded to two different checksums depending on
    // which compiler wrote the files.
    const auto speed = double{3.6 + 0.4 * std::sin(twoPi * i / 900.0) +
                         0.0004 * static_cast<double>(random.next() % 1000)};
    distance += speed;

    auto record = fit::RecordMesg{};
    record.SetTimestamp(timestamp.GetTimeStamp());
    record.SetDistance(static_cast<FIT_FLOAT32>(distance));
    record.SetSpeed(static_cast<FIT_FLOAT32>(speed));
    record.SetHeartRate(
        static_cast<FIT_UINT8>(std::lround(140 + 20 * std::sin(twoPi * i / 1200.0))));
    record.SetCadence(static_cast<FIT_UINT8>(86 + (i % 5)));
    record.SetPower(
        static_cast<FIT_UINT16>(std::lround(260 + 30 * std::sin(twoPi * i / 600.0))));
    record.SetAltitude(static_cast<FIT_FLOAT32>(30.0 + 12.0 * std::sin(twoPi * i / 1800.0)));
    record.SetTemperature(static_cast<FIT_SINT8>(28));

    if (parameters.writesPosition) {
      record.SetPositionLat(static_cast<FIT_SINT32>(1.29 * 11930465.0));
      record.SetPositionLong(
          static_cast<FIT_SINT32>(
              std::lround(103.85 * 11930465.0 + distance * semicirclesPerMeter)));
    }

    if (parameters.writesDeveloperFields) {
      fit::DeveloperField formPower(formPowerDescription, developerDataId);
      formPower.AddValue(static_cast<FIT_UINT16>(55 + (i % 11)));
      record.AddDeveloperField(formPower);
    }

    encode.Write(record);

    if (lapLength > 0 && i > 0 && i % lapLength == 0) {
      auto lap = fit::LapMesg{};
      lap.SetTimestamp(timestamp.GetTimeStamp());
      lap.SetStartTime(static_cast<FIT_DATE_TIME>(fixedStartTime + i - lapLength));
      lap.SetTotalElapsedTime(static_cast<FIT_FLOAT32>(lapLength));
      lap.SetTotalTimerTime(static_cast<FIT_FLOAT32>(lapLength));
      lap.SetTotalDistance(static_cast<FIT_FLOAT32>(distance));
      lap.SetAvgHeartRate(150);
      lap.SetMaxHeartRate(172);
      lap.SetAvgCadence(87);
      encode.Write(lap);
    }

    timestamp.add(fit::DateTime(static_cast<FIT_DATE_TIME>(1)));
  }

  auto timerStop = fit::EventMesg{};
  timerStop.SetTimestamp(timestamp.GetTimeStamp());
  timerStop.SetEvent(FIT_EVENT_TIMER);
  timerStop.SetEventType(FIT_EVENT_TYPE_STOP);
  encode.Write(timerStop);

  auto session = fit::SessionMesg{};
  session.SetTimestamp(timestamp.GetTimeStamp());
  session.SetStartTime(startTime.GetTimeStamp());
  session.SetTotalElapsedTime(static_cast<FIT_FLOAT32>(parameters.recordedSeconds));
  session.SetTotalTimerTime(static_cast<FIT_FLOAT32>(parameters.recordedSeconds));
  session.SetTotalDistance(static_cast<FIT_FLOAT32>(distance));
  session.SetSport(FIT_SPORT_RUNNING);
  session.SetSubSport(FIT_SUB_SPORT_GENERIC);
  session.SetFirstLapIndex(0);
  session.SetNumLaps(static_cast<FIT_UINT16>(parameters.laps));
  encode.Write(session);

  auto activity = fit::ActivityMesg{};
  activity.SetTimestamp(timestamp.GetTimeStamp());
  activity.SetNumSessions(1);
  activity.SetLocalTimestamp(
      static_cast<FIT_LOCAL_DATE_TIME>(timestamp.GetTimeStamp() + 8 * 3600));
  encode.Write(activity);

  if (!encode.Close()) {
    std::cerr << "encode.Close failed for " << path << "\n";
    std::exit(1);
  }
  file.close();
}

}  // namespace

int main(int argc, char** argv) {
  const auto outputDirectory = std::string{argc > 1 ? argv[1] : "data"};

  const auto activities = std::array<ActivityParameters, 4>{{
      {"long_run", 14400, 4, true, true},    // 4 hours with GPS
      {"intervals", 3600, 20, true, true},   // 20 laps of 180 records
      {"treadmill", 5400, 1, false, false},  // no GPS, no developer fields
      {"ride", 10800, 3, true, true},        // 3 hours with GPS
  }};

  for (const ActivityParameters& parameters : activities) {
    // CanMehDataGenerator constructs a new DeterministicRandom from activitySeed
    // before each file, so one file's bytes do not depend on which files
    // CanMehDataGenerator wrote earlier.
    auto random = DeterministicRandom{activitySeed};
    const auto path = std::string{outputDirectory + "/" + parameters.name + ".fit"};
    writeActivityFile(parameters, path, random);
    auto written = std::ifstream{path, std::ios::binary | std::ios::ate};
    std::cout << std::left << std::setw(12) << parameters.name << std::right << std::setw(9)
              << static_cast<long long>(written.tellg()) << " bytes " << std::setw(7)
              << parameters.recordedSeconds << " records\n";
  }
  return 0;
}
