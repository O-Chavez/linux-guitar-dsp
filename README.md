# dsp-engine-v1 (appliance mode)

## ALSA-direct backend (primary)

This project now ships an ALSA-direct engine intended for appliance-style, fixed‑routing setups.
It avoids PipeWire policy/graph scheduling and owns the hardware directly.

### Build
- Configure/build with CMake as usual.
- The ALSA target is `dsp_engine_alsa`.

Important: for realtime performance, build in Release mode.

Quick build example:
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Run (manual)
- Launcher: `start_alsa.sh`.
- Logs: `$XDG_STATE_HOME/dsp-engine-v1/dsp_engine_alsa.log` (falls back to `/tmp/dsp-engine-v1`).

Quick run example:
```
./start_alsa.sh start
```

Baseline profiles (recommended):
```
./start_alsa.sh start-safe
./start_alsa.sh start-lowlat
```

Other commands:
```
./start_alsa.sh stop
./start_alsa.sh restart
./start_alsa.sh status
```

### Recommended settings (stable ALSA)
Use these for a pedal-feel baseline on N100-class hardware:
```
export ALSA_PERIOD=128
export ALSA_PERIODS=3
export ALSA_DISABLE_LINK=1
```
Conservative troubleshooting mode (adds latency):
```
export ALSA_SAFE_DEFAULTS=1
```
Optional troubleshooting:
```
export ALSA_LOG_STATS=1
export ALSA_LOG_TIMING=1
export ALSA_NODE_TIMING=1
```

Baseline enforcement / checks:
- `ALSA_ENFORCE_RELEASE=1` (default): refuses to run non-Release binaries.
- `ALSA_BASELINE=1`: prints an `ALSA: baseline_check ...` PASS/FAIL line on each stats report.
- `ALSA_BASELINE_CHAIN_US_MAX=2000`: threshold for baseline PASS.
- `ALSA_CAPTURE_SANITY_SECS=2`: duration of input sanity check window.
- `ALSA_CAPTURE_SILENT_PEAK=1e-5`: peak threshold for “capture appears silent” warning.

## Stable baseline (project policy)

This project has a known-good “stable baseline” saved and tagged in git.
If stability regresses, revert to the most recent baseline tag/commit and re-test.

### Future development checklist (copy/paste)

Keep these in mind whenever working on this project in the future:

- Never modify `third_party/NeuralAmpModelerCore` directly. Any changes must go to the fork and then update the submodule pointer.
- Never commit build artifacts. `build/` must stay ignored.

Before changing anything: run
```
git status && git submodule status
```
and paste outputs.

Any performance work must include:

- build type = Release
- log line showing `chain_us_max`, `deadline_us`, and xrun counts
-
```
ps -Leo pid,cls,rtprio,pri,ni,cmd | grep dsp_engine_alsa
```

If a change worsens stability: revert immediately to the last “baseline” commit and re-test.

### Environment knobs
- `ALSA_DEVICE` (default `hw:0,0`)
- `ALSA_CAPTURE_DEVICE` / `ALSA_PLAYBACK_DEVICE` (override capture/playback separately; fall back to `ALSA_DEVICE`)
- `ALSA_RATE` (default `48000`)
- `ALSA_CHANNELS` (default `2`)
- `ALSA_CAPTURE_CHANNELS` (default `1`)
- `ALSA_PLAYBACK_CHANNELS` (default `2`)
- `ALSA_PERIOD` (default `128`)
- `ALSA_PERIODS` (default `3`)
- `ALSA_SAFE_DEFAULTS=1` (force conservative `256/4`)
- `ALSA_DISABLE_LINK=1` (disable `snd_pcm_link`, default in `start_alsa.sh`)
- `ALSA_PASSTHROUGH=1` (bypass DSP, raw DI to output)
- `ALSA_BYPASS_NAM=1` (skip NAM stage)
- `ALSA_BYPASS_IR=1` (skip IR stage)
- `ALSA_INPUT_TRIM_DB` (override input trim in dB)
- `ALSA_OUTPUT_GAIN_DB` (master output gain in dB)
- `ALSA_IR_GAIN_DB` (scale IR at load time)
- `ALSA_IR_TARGET_DB` (normalize IR peak to target dBFS)
- `ALSA_IR_MAX_SAMPLES` (trim IR at load time; reduces CPU for very long IRs)
- `ALSA_LOG_IR_INIT=1` (print IR init diagnostics: len/bins/parts)
- `ALSA_SANITIZE_OUTPUT=1` (zero NaN/Inf samples)
- `ALSA_VERBOSE_XRUN=1` (log capture/playback xruns)
- `ALSA_LOG_STATS=1` (periodic peak/xrun stats)
- `ALSA_LOG_TIMING=1` (include chain processing timing in stats)
- `ALSA_NODE_TIMING=1` (break down timing by node type)
- `ALSA_CPU_AFFINITY=0` (pin DSP process to specific CPU core(s), e.g. `0` or `0,1`)
- `ALSA_DISABLE_SOFTCLIP=1` (disable pre-NAM soft clip)
- `ALSA_SOFTCLIP_TANH=1` (use tanh soft clip; default is fast cubic)
- `ALSA_DENORMALS_OFF=0` (disable denormal flush; default is ON)
- `ALSA_NAM_USE_INPUT_LEVEL=0` (disable NAM model input-level normalization)
- `ALSA_NAM_PRE_GAIN_DB` (pre-gain before NAM, dB)
- `NAM_PRE_GAIN_DB` (legacy alias for pre-gain)
- `ALSA_NAM_POST_GAIN_DB` (post-gain after NAM, dB)
- `ALSA_NAM_IN_LIMIT` (input limiter for NAM, default 0.90)
- `ALSA_ENABLE_RT=0` (disable realtime scheduling + mlockall)
- `ALSA_RT_PRIORITY` (SCHED_FIFO priority, default 80)
- `DUMP_NAM_IN_WAV=/tmp/nam_in.wav` (dump NAM input to WAV on shutdown)
- `DUMP_NAM_OUT_WAV=/tmp/nam_out.wav` (dump NAM output to WAV on shutdown)
- `DUMP_NAM_SECONDS=10` (length in seconds for NAM dump buffers)
- `DUMP_IR_OUT_WAV=/tmp/ir_out.wav` (dump IR output to WAV on shutdown)
- `DUMP_IR_SECONDS=10` (length in seconds for IR dump buffer)

### Runtime control

#### Control socket (recommended)

The ALSA engine exposes a simple Unix-domain socket for orchestration (Node backend integration).

- Socket: `/tmp/pedal-dsp.sock`
- Override: set `DSP_CONTROL_SOCK=/path/to.sock`
- Protocol: one JSON request per line, one JSON response per line

Commands:
- `{"cmd":"get_chain"}`
- `{"cmd":"set_chain","chain":{...}}` (accepts canonical v1 chain JSON, and the legacy chain.json format)
- `{"cmd":"list_types"}` (returns node drawer/parameter metadata)

Example (using socat):
```
echo '{"cmd":"list_types"}' | socat - UNIX-CONNECT:/tmp/pedal-dsp.sock
echo '{"cmd":"get_chain"}'  | socat - UNIX-CONNECT:/tmp/pedal-dsp.sock
```

#### Legacy trim (debug)

- UDP localhost:9000, message `TRIM_DB <value>`.

### Chain swap behavior

- Chain changes are compiled off-thread and swapped at the ALSA period boundary.
- Optional click-reduction ramp around swaps: set `ALSA_CHAIN_XFADE=1`.
	- Control ramp length with `ALSA_SWAP_RAMP_SAMPLES` (default 32 when enabled).

## PipeWire backend (deprecated)

PipeWire/JACK engines and routing scripts are kept for reference only.
They are not part of the supported day-to-day path.

Legacy sources live in deprecated/src and are built only when:
`-DBUILD_LEGACY_ENGINES=ON`

## Services (no auto-start)

The install script copies service units but does **not** enable them by default.
This prevents legacy auto-start triggers from interfering with manual runs.

- `systemd/dsp-engine-alsa.service`

To install user units:
```
./scripts/install_user_service.sh
```

## Primary folders (project + runtime)

Project tree:
- [src/](src/) — current engine sources (ALSA direct).
- [scripts/](scripts/) — wiring helpers + install tooling.
- [systemd/](systemd/) — service unit files.
- [third_party/](third_party/) — vendored dependencies (NAM core).
- [deprecated/](deprecated/) — legacy PipeWire/JACK sources (optional build).
- [build/](build/) — CMake output (local, generated).
- [app/](app/) — Node/React UI + control app (monorepo; add your app here).

Runtime (default paths used by the appliance setup):
- /opt/pedal/config/chain.json — active config (model + IR paths).

## UI / control app (monorepo)

This repo is set up to host a Node/React control UI in [app/](app/).
Keeping the DSP engine at repo root avoids breaking existing CMake paths, `start_alsa.sh`, and the systemd unit.

Suggested flow:
- Copy your existing Node/React project files into `app/`.
- Use `app/`’s own package manager scripts to build/run the UI.
- Orchestrate the engine via the control socket (`/tmp/pedal-dsp.sock` by default).
- /opt/pedal/assets/ — optional bundled assets (e.g., default models/IRs/configs).
- /opt/pedal/models/ — NAM model files (typical convention).
- /opt/pedal/irs/ — IR WAVs (typical convention).

You can keep reference configs/models in the repo and point chain.json at them,
but the shipped runtime expects system-wide assets under /opt/pedal by default.
