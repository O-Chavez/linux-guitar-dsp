# Default to Release builds for realtime DSP performance.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

option(BUILD_LEGACY_ENGINES "Build deprecated JACK/PipeWire engines" OFF)

pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(FFTW3F REQUIRED fftw3f)

# NAM core (vendored sources)
set(NAM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/NeuralAmpModelerCore")
set(NAM_SRC
  ${NAM_ROOT}/NAM/activations.cpp
  ${NAM_ROOT}/NAM/convnet.cpp
  ${NAM_ROOT}/NAM/dsp.cpp
  ${NAM_ROOT}/NAM/get_dsp.cpp
  ${NAM_ROOT}/NAM/lstm.cpp
  ${NAM_ROOT}/NAM/util.cpp
  ${NAM_ROOT}/NAM/wavenet.cpp
)

add_library(nam_core STATIC ${NAM_SRC})
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT)

# Performance flags for Release builds (CPU-optimized).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(nam_core PRIVATE -O3 -march=native -mtune=native)
endif()

target_include_directories(nam_core PUBLIC
  ${NAM_ROOT}/NAM
  ${NAM_ROOT}/Dependencies/nlohmann
)

# Eigen (header-only) - installed at /usr/include/eigen3 on Ubuntu
target_include_directories(nam_core PUBLIC /usr/include/eigen3)

# Your executables
# Legacy engines (deprecated)
if(BUILD_LEGACY_ENGINES)
  pkg_check_modules(JACK REQUIRED jack)
  pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

  # Original JACK version
  add_executable(dsp_engine_v1
    deprecated/src/main.cpp
    src/ir_loader.cpp
    src/fft_convolver.cpp
  )

  target_include_directories(dsp_engine_v1 PRIVATE
    ${NAM_ROOT}/NAM
    ${NAM_ROOT}/Dependencies/nlohmann
    /usr/include/eigen3
  )

  target_link_libraries(dsp_engine_v1 PRIVATE
    ${JACK_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    ${FFTW3F_LIBRARIES}
    -Wl,--whole-archive nam_core -Wl,--no-whole-archive
  )

  # PipeWire version
  add_executable(dsp_engine_pw
    deprecated/src/main_pipewire.cpp
    src/ir_loader.cpp
    src/fft_convolver.cpp
  )

  target_include_directories(dsp_engine_pw PRIVATE
    ${NAM_ROOT}/NAM
    ${NAM_ROOT}/Dependencies/nlohmann
    /usr/include/eigen3
    ${PIPEWIRE_INCLUDE_DIRS}
  )

  target_link_libraries(dsp_engine_pw PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    ${FFTW3F_LIBRARIES}
    -Wl,--whole-archive nam_core -Wl,--no-whole-archive
  )

  # Minimal PipeWire DSP (clean signal path / transport validation)
  add_executable(dsp_engine_min
    deprecated/src/pw_minidsp.cpp
  )

  target_include_directories(dsp_engine_min PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
  )

  target_link_libraries(dsp_engine_min PRIVATE
    ${PIPEWIRE_LIBRARIES}
    pthread
  )
endif()

# ALSA-direct engine (appliance mode)
add_executable(dsp_engine_alsa
  src/main_alsa.cpp
  src/ir_loader.cpp
  src/fft_convolver.cpp
  src/signal_chain_schema.cpp
  src/signal_chain_nodes.cpp
  src/signal_chain.cpp
  src/chain_control_server.cpp
)

# Embed the configured build type string so the runtime banner can prove what binary is running.
target_compile_definitions(dsp_engine_alsa PRIVATE DSP_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(dsp_engine_alsa PRIVATE -O3 -march=native -mtune=native)
endif()

target_include_directories(dsp_engine_alsa PRIVATE
  ${NAM_ROOT}/NAM
  ${NAM_ROOT}/Dependencies/nlohmann
  /usr/include/eigen3
  ${ALSA_INCLUDE_DIRS}
)

target_link_libraries(dsp_engine_alsa PRIVATE
  ${ALSA_LIBRARIES}
  ${SNDFILE_LIBRARIES}
  ${FFTW3F_LIBRARIES}
  -Wl,--whole-archive nam_core -Wl,--no-whole-archive
)

 # Offline NAM harness (no PipeWire/JACK): generates a synthetic input and writes WAV output.
 add_executable(nam_synth_test
   src/nam_synth_test.cpp
 )
 target_link_libraries(nam_synth_test PRIVATE
   sndfile
   -Wl,--whole-archive nam_core -Wl,--no-whole-archive
   pthread
 )
if(BUILD_LEGACY_ENGINES)
  target_compile_options(dsp_engine_v1 PRIVATE   )
  target_compile_options(dsp_engine_pw PRIVATE   )
endif()
