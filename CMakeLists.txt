cmake_minimum_required(VERSION 3.20)
project(CanMehEvolve CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(CANMEH_PIPE -pipe)
endif()
add_compile_options(${CANMEH_PIPE})
set(CANMEH_DEP_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CANMEH_PIPE}")

include(ExternalProject)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
option(CANMEH_FETCH_SDK "Fetch the FIT SDK at configure time" ON)
set(CANMEH_DEPS ${CMAKE_BINARY_DIR}/deps)
set(CANMEH_DOWNLOADS $ENV{HOME}/.cmake-downloads/${CMAKE_PROJECT_NAME})
set(BENCHMARK_LIBRARY ${CANMEH_DEPS}/lib/libbenchmark.a)
set(JSON_HEADER ${CANMEH_DEPS}/include/nlohmann/json.hpp)
set(CLI11_HEADER ${CANMEH_DEPS}/include/CLI11.hpp)

ExternalProject_Add(googlebenchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
  URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
  DOWNLOAD_DIR ${CANMEH_DOWNLOADS}
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CANMEH_DEPS} -DCMAKE_INSTALL_LIBDIR=lib
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DBENCHMARK_ENABLE_TESTING=OFF
             -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER} -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
             -DCMAKE_CXX_FLAGS=${CANMEH_DEP_CXX_FLAGS}
  BUILD_BYPRODUCTS ${BENCHMARK_LIBRARY})

ExternalProject_Add(nlohmann-json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
  URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
  DOWNLOAD_DIR ${CANMEH_DOWNLOADS}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ${CMAKE_COMMAND} -E copy
    <SOURCE_DIR>/single_include/nlohmann/json.hpp ${JSON_HEADER}
  LOG_INSTALL ON
  LOG_OUTPUT_ON_FAILURE ON
  BUILD_BYPRODUCTS ${JSON_HEADER})

ExternalProject_Add(cli11
  URL https://github.com/CLIUtils/CLI11/releases/download/v2.4.2/CLI11.hpp
  URL_HASH SHA256=3935283e4c9db7089ed304476dbab2805c9311c41b125a55e92da4659df0d660
  DOWNLOAD_DIR ${CANMEH_DOWNLOADS}
  DOWNLOAD_NO_EXTRACT TRUE
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <DOWNLOADED_FILE> ${CLI11_HEADER}
  LOG_INSTALL ON
  LOG_OUTPUT_ON_FAILURE ON
  BUILD_BYPRODUCTS ${CLI11_HEADER})

add_library(CanMehToolDefaults INTERFACE)
target_include_directories(CanMehToolDefaults SYSTEM INTERFACE ${CANMEH_DEPS}/include)
target_include_directories(CanMehToolDefaults INTERFACE src)
target_compile_definitions(CanMehToolDefaults INTERFACE
  "CANMEH_DEFAULT_WORKLOAD=\"${CMAKE_CURRENT_SOURCE_DIR}/src/workloads/SdkDecode.cpp\""
  "CANMEH_DEFAULT_CALIBRATION_ROOT=\"${CMAKE_BINARY_DIR}/calibration\""
  "CANMEH_DEFAULT_BENCH=\"${CMAKE_BINARY_DIR}/CanMehBench\""
  "CANMEH_DEFAULT_HARNESS=\"${CMAKE_BINARY_DIR}/CanMehHarness\""
  "CANMEH_DEFAULT_DECODE_FOR_SAMPLER=\"${CMAKE_BINARY_DIR}/CanMehDecodeForSampler\""
  "CANMEH_DEFAULT_DATA=\"${CMAKE_BINARY_DIR}/data\""
  "CANMEH_DEFAULT_CXX=\"${CMAKE_CXX_COMPILER}\"")

add_executable(CanMehHarness src/Harness.cpp)
add_dependencies(CanMehHarness nlohmann-json cli11)
target_link_libraries(CanMehHarness PRIVATE CanMehToolDefaults)

add_executable(CanMehCalibrate src/Calibrate.cpp)
add_dependencies(CanMehCalibrate nlohmann-json cli11)
target_link_libraries(CanMehCalibrate PRIVATE CanMehToolDefaults)

add_executable(CanMehSearch src/Search.cpp)
add_dependencies(CanMehSearch nlohmann-json cli11)
target_link_libraries(CanMehSearch PRIVATE CanMehToolDefaults CURL::libcurl)

add_executable(CanMehDecodeForSampler src/DecodeForSampler.cpp)
add_dependencies(CanMehDecodeForSampler googlebenchmark cli11 nlohmann-json)
target_link_libraries(CanMehDecodeForSampler PRIVATE CanMehToolDefaults
  ${BENCHMARK_LIBRARY} ${CMAKE_DL_LIBS} Threads::Threads)

add_executable(CanMehBench src/BenchmarkProcess.cpp)
add_dependencies(CanMehBench googlebenchmark cli11 nlohmann-json)
target_include_directories(CanMehBench SYSTEM PRIVATE ${CANMEH_DEPS}/include)
target_link_libraries(CanMehBench PRIVATE ${BENCHMARK_LIBRARY} ${CMAKE_DL_LIBS} Threads::Threads)

set(CANMEH_CALIBRATION_REFERENCE_UNITS 20000 CACHE STRING
    "work units libfit_ref performs in every calibration pair")
set(CANMEH_CALIBRATION_BUFFER_MIB 16 CACHE STRING
    "MiB each calibration library scatters its accesses over")
set(CANMEH_CALIBRATION_DIGEST 0x43616c69627231ULL)
set(CANMEH_CALIBRATION_MISMATCHED_DIGEST 0x43616c69627232ULL)
if(APPLE)
  set(CANMEH_LIBRARY_SUFFIX .dylib)
  set(CANMEH_NAME_FLAG -Wl,-install_name,)
else()
  set(CANMEH_LIBRARY_SUFFIX .so)
  set(CANMEH_NAME_FLAG -Wl,-soname,)
endif()

function(canmeh_calibration_library target source units digest claimed_name directory
         output_name)
  add_library(${target} SHARED ${source})
  add_dependencies(${target} CanMehBench)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(${target} PRIVATE
    CANMEH_CALIBRATION_WORK_UNITS=${units}
    CANMEH_CALIBRATION_DIGEST=${digest}
    CANMEH_CALIBRATION_BUFFER_MIB=${CANMEH_CALIBRATION_BUFFER_MIB})
  set_target_properties(${target} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${directory}
    OUTPUT_NAME ${output_name}
    SUFFIX ${CANMEH_LIBRARY_SUFFIX}
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    NO_SONAME ON)
  target_link_options(${target} PRIVATE ${CANMEH_NAME_FLAG}${claimed_name})
endfunction()

function(canmeh_calibration_pair name candidate_units)
  cmake_parse_arguments(PAIR "BREAK_DIGEST;SHARE_INSTALL_NAME" "" "" ${ARGN})
  set(directory ${CMAKE_BINARY_DIR}/calibration/${name})
  set(reference_path ${directory}/libfit_ref${CANMEH_LIBRARY_SUFFIX})
  set(candidate_path ${directory}/libfit_cand${CANMEH_LIBRARY_SUFFIX})

  set(candidate_digest ${CANMEH_CALIBRATION_DIGEST})
  set(PAIR_DIGEST_BROKEN false)
  if(PAIR_BREAK_DIGEST)
    set(candidate_digest ${CANMEH_CALIBRATION_MISMATCHED_DIGEST})
    set(PAIR_DIGEST_BROKEN true)
  endif()

  set(reference_claims ${reference_path})
  set(PAIR_INSTALL_NAME_SHARED false)
  if(PAIR_SHARE_INSTALL_NAME)
    set(reference_claims ${candidate_path})
    set(PAIR_INSTALL_NAME_SHARED true)
  endif()

  canmeh_calibration_library(calibration_${name}_reference
    ${CMAKE_CURRENT_SOURCE_DIR}/src/workloads/CalibrationLoop.cpp
    ${CANMEH_CALIBRATION_REFERENCE_UNITS} ${CANMEH_CALIBRATION_DIGEST}
    ${reference_claims} ${directory} fit_ref)
  canmeh_calibration_library(calibration_${name}_candidate
    ${CMAKE_CURRENT_SOURCE_DIR}/src/workloads/CalibrationLoop.cpp
    ${candidate_units} ${candidate_digest}
    ${candidate_path} ${directory} fit_cand)

  set(PAIR_REFERENCE_UNITS ${CANMEH_CALIBRATION_REFERENCE_UNITS})
  set(PAIR_CANDIDATE_UNITS ${candidate_units})
  configure_file(cmake/calibration-meta.json.in ${directory}/meta.json @ONLY)
endfunction()

# The eight shipped calibration cases. Each candidate count is the reference
# count divided by the true speedup that case is built to have, so the quotient
# of the two counts compiled into a pair is the answer a measurement rule is
# scored against. CMake builds every pair in place, and each library claims as
# its own name the path CMake writes it to, so CanMehBench opens the same two
# paths the linker named and dlopen returns two images.
math(EXPR CANMEH_UNITS_1_PERCENT "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 100 / 101")
math(EXPR CANMEH_UNITS_2_PERCENT "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 100 / 102")
math(EXPR CANMEH_UNITS_5_PERCENT "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 100 / 105")
math(EXPR CANMEH_UNITS_10_PERCENT "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 100 / 110")
math(EXPR CANMEH_UNITS_10_PERCENT_SLOWER "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 110 / 100")

canmeh_calibration_pair(null ${CANMEH_CALIBRATION_REFERENCE_UNITS})
canmeh_calibration_pair(faster-1-percent ${CANMEH_UNITS_1_PERCENT})
canmeh_calibration_pair(faster-2-percent ${CANMEH_UNITS_2_PERCENT})
canmeh_calibration_pair(faster-5-percent ${CANMEH_UNITS_5_PERCENT})
canmeh_calibration_pair(faster-10-percent ${CANMEH_UNITS_10_PERCENT})
canmeh_calibration_pair(slower-10-percent ${CANMEH_UNITS_10_PERCENT_SLOWER})
canmeh_calibration_pair(broken-digest ${CANMEH_UNITS_10_PERCENT} BREAK_DIGEST)
canmeh_calibration_pair(shared-install-name ${CANMEH_UNITS_10_PERCENT} SHARE_INSTALL_NAME)

# The linearity pairs. Each candidate performs a known multiple of the
# reference's work, so the measured ratio has to reproduce that multiple. The
# directory name is the multiple times one hundred, because a CMake target name
# may not contain a dot.
math(EXPR CANMEH_SWEEP_105 "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 105 / 100")
math(EXPR CANMEH_SWEEP_110 "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 110 / 100")
math(EXPR CANMEH_SWEEP_200 "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 2")
math(EXPR CANMEH_SWEEP_400 "${CANMEH_CALIBRATION_REFERENCE_UNITS} * 4")

canmeh_calibration_pair(sweep-105 ${CANMEH_SWEEP_105})
canmeh_calibration_pair(sweep-110 ${CANMEH_SWEEP_110})
canmeh_calibration_pair(sweep-200 ${CANMEH_SWEEP_200})
canmeh_calibration_pair(sweep-400 ${CANMEH_SWEEP_400})

if(CANMEH_FETCH_SDK)
  ExternalProject_Add(FITSDK
    GIT_REPOSITORY https://github.com/garmin/fit-cpp-sdk.git
    GIT_TAG 21.201.0
    GIT_SHALLOW TRUE
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND "")
  ExternalProject_Get_Property(FITSDK SOURCE_DIR)
  set(FITSDK_SOURCE_DIR ${SOURCE_DIR})

  foreach(unit fit fit_accumulated_field fit_accumulator fit_buffer_encode fit_crc
               fit_buffered_mesg_broadcaster fit_buffered_record_mesg_broadcaster
               fit_date_time fit_decode fit_developer_field fit_encode fit_factory
               fit_developer_field_definition fit_developer_field_description
               fit_field fit_field_base fit_field_definition fit_mesg fit_profile
               fit_mesg_broadcaster fit_mesg_definition fit_protocol_validator
               fit_mesg_with_event_broadcaster fit_unicode)
    list(APPEND FITSDK_SOURCES ${FITSDK_SOURCE_DIR}/src/${unit}.cpp)
  endforeach()
  set_source_files_properties(${FITSDK_SOURCES} PROPERTIES GENERATED TRUE)

  add_executable(CanMehDataGenerator src/DataGenerator.cpp ${FITSDK_SOURCES})
  target_compile_options(CanMehDataGenerator PRIVATE -ffp-contract=off)
  add_dependencies(CanMehDataGenerator FITSDK)
  target_include_directories(CanMehDataGenerator PRIVATE ${FITSDK_SOURCE_DIR}/src)

  add_library(CanMehWorkload OBJECT src/workloads/SdkDecode.cpp)
  add_dependencies(CanMehWorkload FITSDK)
  target_include_directories(CanMehWorkload PRIVATE src ${FITSDK_SOURCE_DIR}/src)
  set_target_properties(CanMehWorkload PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden)

  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/data/long_run.fit
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/data
    COMMAND CanMehDataGenerator ${CMAKE_BINARY_DIR}/data
    DEPENDS CanMehDataGenerator)
  add_custom_target(CanMehData ALL DEPENDS ${CMAKE_BINARY_DIR}/data/long_run.fit)

  target_compile_definitions(CanMehHarness PRIVATE
    "CANMEH_DEFAULT_SDK=\"${FITSDK_SOURCE_DIR}\"")
endif()
