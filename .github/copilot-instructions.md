# Copilot instructions for dsp-engine-v1

## Big picture
- Primary engine is ALSA-direct in [src/main_alsa.cpp](src/main_alsa.cpp), built as `dsp_engine_alsa` (appliance-style fixed routing).
- Legacy PipeWire/JACK engines live under [deprecated/src/](deprecated/src/) and only build with `-DBUILD_LEGACY_ENGINES=ON`.
- DSP chain is input trim → NAM model → optional IR convolver → stereo out. IR convolution uses partitioned FFT in [src/fft_convolver.cpp](src/fft_convolver.cpp).
- NeuralAmpModelerCore is vendored in [third_party/NeuralAmpModelerCore/](third_party/NeuralAmpModelerCore/) and built as `nam_core` from [CMakeLists.txt](CMakeLists.txt).

## Build + run workflows
- Build (all targets): `cmake -S . -B build && cmake --build build -j`.
- Manual run ALSA engine via [start_alsa.sh](start_alsa.sh); logs go to `$XDG_STATE_HOME/dsp-engine-v1` (fallback `/tmp/dsp-engine-v1`).
- Service units exist in [systemd/](systemd/) but are not auto-enabled by install scripts.
- Offline validation: `nam_synth_test` in [src/nam_synth_test.cpp](src/nam_synth_test.cpp) renders a test tone through a NAM model to WAV.

## Runtime config + control
- Active runtime config is `/opt/pedal/config/chain.json` (model + IR paths, input trim, passthrough).
- UDP control on localhost:9000 accepts `TRIM_DB <value>` to update input trim.
- ALSA environment knobs are read in [start_alsa.sh](start_alsa.sh) and [src/main_alsa.cpp](src/main_alsa.cpp); common overrides: `ALSA_DEVICE`, `ALSA_RATE`, `ALSA_PERIOD`, `ALSA_PERIODS`, `ALSA_INPUT_TRIM_DB`, `ALSA_BYPASS_NAM`, `ALSA_BYPASS_IR`, `ALSA_PASSTHROUGH`, `NAM_PRE_GAIN_DB`.

## Project-specific patterns
- Real-time callbacks avoid allocations; when needed they use resize-once or thread-local buffers.
- IR files must already match the negotiated sample rate; no resampling occurs (see [src/ir_loader.cpp](src/ir_loader.cpp)).
- NAM/IR block sizes are tied to the audio period size; reinit on rate/period change (see ALSA setup in [src/main_alsa.cpp](src/main_alsa.cpp)).
