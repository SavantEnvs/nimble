# mayhem/CMakeLists.txt - the Mayhem integration's wrapper around Nimble's own CMake project.
#
# Nimble's top-level CMakeLists.txt is added unchanged as a subdirectory, so everything it defines
# (the nimble_* libraries, the Velox and OpenZL submodule builds, the gtest suites) is built by the
# upstream build system with the upstream options. This file only adds what the integration needs
# on top: the libFuzzer harnesses, their -standalone reproducers, and the seed generator.
# Driven by mayhem/build.sh, which configures it twice (fuzz tree and test tree).
cmake_minimum_required(VERSION 3.14)
project(NimbleMayhem C CXX)

# Collect the tests Nimble registers (add_test) in this tree, so `ctest` sees them from the top.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

get_filename_component(NIMBLE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_subdirectory("${NIMBLE_ROOT}" nimble)

# Nimble's top-level CMakeLists.txt appends Velox's get_cxx_flags (the CPU_TARGET SIMD flags) to
# CMAKE_CXX_FLAGS in its own directory scope only. The targets below include the same folly/Velox
# headers, so they need the same flags: folly's F14 tables pick their SIMD mode from them, and a
# mismatch fails at link time (undefined F14LinkCheck<...>::check()).
execute_process(
  COMMAND
    bash -c
    "( source ${NIMBLE_ROOT}/velox/scripts/setup-helper-functions.sh && echo -n $(get_cxx_flags $ENV{CPU_TARGET}))"
  OUTPUT_VARIABLE MAYHEM_SCRIPT_CXX_FLAGS
  RESULT_VARIABLE MAYHEM_SCRIPT_STATUS)
if(NOT MAYHEM_SCRIPT_STATUS EQUAL 0)
  message(FATAL_ERROR "Unable to determine Velox's compiler flags")
endif()
string(APPEND CMAKE_CXX_FLAGS " ${MAYHEM_SCRIPT_CXX_FLAGS}")

option(MAYHEM_BUILD_FUZZERS "Build the fuzz harnesses and their standalone reproducers" OFF)
option(MAYHEM_BUILD_SEEDGEN "Build mayhem_gen_seeds (regenerates the committed seed corpora)" OFF)

# Same include roots Nimble's own targets get from its top-level include_directories().
set(MAYHEM_INCLUDES "${NIMBLE_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}")
set(MAYHEM_SYSTEM_INCLUDES "${NIMBLE_ROOT}/velox" "${NIMBLE_ROOT}/velox/velox/external/xxhash")

if(MAYHEM_BUILD_FUZZERS)
  # MAYHEM_COVERAGE_FLAGS (-fsanitize=fuzzer-no-link) goes on Nimble's own libraries only - every
  # library target defined under dwio/ - so the fuzzer's coverage feedback is about Nimble, not
  # about Velox, OpenZL or fsst. The sanitizers themselves come in through CMAKE_<LANG>_FLAGS and
  # cover everything built from the checkout.
  set(MAYHEM_COVERAGE_FLAGS "" CACHE STRING "Coverage instrumentation for Nimble's libraries")
  set(MAYHEM_FUZZ_ENGINE "" CACHE STRING "Link flags selecting the fuzzing engine")
  set(MAYHEM_STANDALONE_MAIN "" CACHE FILEPATH "C source of the run-once driver")
  set(MAYHEM_LSAN_OFF "${CMAKE_CURRENT_SOURCE_DIR}/lsan_off.cc" CACHE FILEPATH
    "Source defining __lsan_is_turned_off(), linked into every fuzz binary")
  separate_arguments(_cov NATIVE_COMMAND "${MAYHEM_COVERAGE_FLAGS}")
  separate_arguments(_engine NATIVE_COMMAND "${MAYHEM_FUZZ_ENGINE}")

  function(mayhem_instrument_dir dir)
    string(FIND "${dir}/" "${NIMBLE_ROOT}/dwio/" _pos)
    if(_pos EQUAL 0)
      get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
      foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY")
          target_compile_options(${_t} PRIVATE ${_cov})
        endif()
      endforeach()
    endif()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_s IN LISTS _subdirs)
      mayhem_instrument_dir("${_s}")
    endforeach()
  endfunction()
  if(_cov)
    mayhem_instrument_dir("${NIMBLE_ROOT}")
  endif()

  # The run-once driver is C; built as its own object so its LLVMFuzzerTestOneInput reference
  # keeps C linkage.
  add_library(mayhem_standalone_main OBJECT "${MAYHEM_STANDALONE_MAIN}")
  set_source_files_properties("${MAYHEM_STANDALONE_MAIN}" PROPERTIES LANGUAGE C)

  # mayhem_fuzzer(<name> <libs...>): <name> (libFuzzer) and <name>-standalone (run-once), both in
  # the top of the build tree; build.sh copies them to /mayhem.
  function(mayhem_fuzzer name)
    add_library(${name}_obj OBJECT ${name}.cpp "${MAYHEM_LSAN_OFF}")
    target_include_directories(${name}_obj PRIVATE ${MAYHEM_INCLUDES})
    target_include_directories(${name}_obj SYSTEM PRIVATE ${MAYHEM_SYSTEM_INCLUDES})
    target_compile_options(${name}_obj PRIVATE ${_cov})
    target_link_libraries(${name}_obj PRIVATE ${ARGN})

    add_executable(${name} $<TARGET_OBJECTS:${name}_obj>)
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_link_options(${name} PRIVATE ${_engine})

    add_executable(${name}-standalone
      $<TARGET_OBJECTS:${name}_obj> $<TARGET_OBJECTS:mayhem_standalone_main>)
    target_link_libraries(${name}-standalone PRIVATE ${ARGN})

    set_target_properties(${name} ${name}-standalone
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  endfunction()

  # nimble_index (which every tablet reader links) calls into the writer libraries
  # (ChunkedStreamWriter, the buffer growth policies) without listing them, and the writer in turn
  # links nimble_index. Upstream's own executables all link the writer explicitly; here the three
  # archives are put in one rescanned link group so the cycle resolves whatever the order.
  set(MAYHEM_READER_LIBS
    "$<LINK_GROUP:RESCAN,nimble_index,nimble_velox_writer,nimble_velox_field_writer>")

  mayhem_fuzzer(fuzz_velox_reader
    nimble_velox_reader ${MAYHEM_READER_LIBS} velox_file velox_memory)
  mayhem_fuzzer(fuzz_selective_reader
    nimble_velox_selective ${MAYHEM_READER_LIBS} velox_dwio_common velox_file velox_memory)
  mayhem_fuzzer(fuzz_encoding nimble_encodings velox_memory)
  mayhem_fuzzer(fuzz_encoding_legacy nimble_encodings_legacy nimble_encodings velox_memory)
  mayhem_fuzzer(fuzz_deserializer
    nimble_deserializer nimble_velox_common velox_vector velox_memory)
endif()

if(MAYHEM_BUILD_SEEDGEN)
  add_executable(mayhem_gen_seeds gen_seeds.cpp)
  target_include_directories(mayhem_gen_seeds PRIVATE ${MAYHEM_INCLUDES})
  target_include_directories(mayhem_gen_seeds SYSTEM PRIVATE ${MAYHEM_SYSTEM_INCLUDES})
  target_link_libraries(mayhem_gen_seeds PRIVATE
    nimble_common_file_writer nimble_serializer nimble_encodings nimble_encodings_tests_utils
    velox_vector_test_lib velox_vector velox_memory)
  set_target_properties(mayhem_gen_seeds
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
