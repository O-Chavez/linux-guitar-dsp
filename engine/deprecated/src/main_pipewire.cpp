#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>

#include <sndfile.h>

#include "json.hpp"
#include "get_dsp.h"
#include "ir_loader.h"
#include "fft_convolver.h"

// UDP
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// PipeWire globals
// PipeWire globals
static struct pw_thread_loop *tloop = nullptr;
// Duplex mode uses a single PipeWire stream for both capture+playback.
static struct pw_stream *duplex_stream = nullptr;
static struct pw_stream *capture_stream = nullptr;
static struct pw_stream *playback_stream = nullptr;
static struct pw_context *context = nullptr;
static struct pw_core *core = nullptr;
static struct pw_registry *registry = nullptr;
static struct spa_hook registry_listener;
static struct spa_hook capture_listener;
static struct spa_hook playback_listener;
static struct spa_hook core_listener;

// Optional explicit target node IDs (resolved from PW_TARGET_CAPTURE/PW_TARGET_PLAYBACK).
static std::atomic<uint32_t> gTargetCaptureNodeId{PW_ID_ANY};
static std::atomic<uint32_t> gTargetPlaybackNodeId{PW_ID_ANY};
static std::string gTargetCaptureName;
static std::string gTargetPlaybackName;

static void registry_global(void *data, uint32_t id, uint32_t permissions,
                            const char *type, uint32_t version,
                            const struct spa_dict *props)
{
  (void)data;
  (void)permissions;
  (void)version;
  if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;
  if (!props)
    return;

  const char *nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (!nodeName)
    return;

  // Match by full node.name (what pw-cli ls Node prints).
  if (!gTargetCaptureName.empty() && gTargetCaptureNodeId.load(std::memory_order_relaxed) == PW_ID_ANY &&
      std::strcmp(nodeName, gTargetCaptureName.c_str()) == 0)
  {
    gTargetCaptureNodeId.store(id, std::memory_order_relaxed);
    std::fprintf(stderr, "[PWREG] resolved capture target '%s' -> node id %u\n", nodeName, id);
    std::fflush(stderr);
  }
  if (!gTargetPlaybackName.empty() && gTargetPlaybackNodeId.load(std::memory_order_relaxed) == PW_ID_ANY &&
      std::strcmp(nodeName, gTargetPlaybackName.c_str()) == 0)
  {
    gTargetPlaybackNodeId.store(id, std::memory_order_relaxed);
    std::fprintf(stderr, "[PWREG] resolved playback target '%s' -> node id %u\n", nodeName, id);
    std::fflush(stderr);
  }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
};

static inline bool waitForTargetIds(uint32_t &capTargetId, uint32_t &pbTargetId,
                                    uint32_t timeoutMs)
{
  // The PipeWire registry delivers globals asynchronously. If we connect streams
  // before those globals arrive, we'll end up using PW_ID_ANY even when the user
  // requested explicit targets.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline)
  {
    capTargetId = gTargetCaptureNodeId.load(std::memory_order_relaxed);
    pbTargetId = gTargetPlaybackNodeId.load(std::memory_order_relaxed);

    const bool capOk = gTargetCaptureName.empty() || capTargetId != PW_ID_ANY;
    const bool pbOk = gTargetPlaybackName.empty() || pbTargetId != PW_ID_ANY;
    if (capOk && pbOk)
      return true;

    // We must pump PipeWire events for registry_global() to run. Use a thread loop
    // if available (preferred), otherwise fall back to a short sleep.
    if (tloop)
    {
      pw_thread_loop_lock(tloop);
      pw_thread_loop_timed_wait(tloop, 10 /*ms*/);
      pw_thread_loop_unlock(tloop);
    }
    else
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  capTargetId = gTargetCaptureNodeId.load(std::memory_order_relaxed);
  pbTargetId = gTargetPlaybackNodeId.load(std::memory_order_relaxed);
  return false;
}

static std::atomic<bool> printedStreamState{false};
static std::atomic<bool> printedFormat{false};

// Some PipeWire graph configurations expose playback ports as "monitor_*" instead of
// "output_*" for duplex nodes. We'll treat whatever buffer PipeWire gives us as
// the playback destination.

static std::atomic<bool> running{true};
static uint32_t sampleRate = 48000;
static uint32_t bufferSize = 128;

static inline void threadLoopSignal()
{
  if (!tloop)
    return;
  pw_thread_loop_lock(tloop);
  pw_thread_loop_signal(tloop, false);
  pw_thread_loop_unlock(tloop);
}

// Track NAM block size separately from the IR/convolver so we can re-prewarm
// the model if PipeWire's quantum changes at runtime.
static uint32_t gNamBlockSize = 0;
static uint32_t requestedQuantum = 128;

// Reference mode: mimic common LV2/host usage (48k only, no resampling/OS, minimal chain).
// Enable with NAM_REFERENCE_MODE=1.
static std::atomic<bool> namReferenceMode{false};

// Peak metering (updated in RT callback, read by meter thread)
static std::atomic<float> peakInput{0.0f};
static std::atomic<float> peakNamIn{0.0f};
static std::atomic<float> peakNamOut{0.0f};
static std::atomic<float> peakIrOut{0.0f};
static std::atomic<float> peakFinalOut{0.0f};

// Optional realtime dump of the signal going into/out of NAM (for offline analysis).
// Env:
//   DUMP_NAM_IN_WAV=/tmp/nam_in.wav
//   DUMP_NAM_OUT_WAV=/tmp/nam_out.wav
//   DUMP_NAM_SECONDS=10
// The dump is collected in the RT callback into a ring buffer and flushed on shutdown.
static std::string dumpNamInPath;
static std::string dumpNamOutPath;
static uint32_t dumpNamMaxFrames = 0;
static uint32_t dumpNamInitSr = 0;
static uint32_t dumpNamInitSecs = 0;
static std::atomic<uint32_t> dumpNamInWritePos{0};
static std::atomic<uint32_t> dumpNamOutWritePos{0};
static std::atomic<uint32_t> dumpNamInTotalWritten{0};
static std::atomic<uint32_t> dumpNamOutTotalWritten{0};
static std::vector<float> dumpNamIn;
static std::vector<float> dumpNamOut;
static std::mutex dumpNamFlushMutex;

static inline uint32_t readEnvU32(const char *k, uint32_t def)
{
  if (const char *e = std::getenv(k))
  {
    long v = std::strtol(e, nullptr, 10);
    if (v > 0)
      return (uint32_t)v;
  }
  return def;
}

static void dumpNamInit(uint32_t sr)
{
  const char *inP = std::getenv("DUMP_NAM_IN_WAV");
  const char *outP = std::getenv("DUMP_NAM_OUT_WAV");
  if (inP)
    dumpNamInPath = inP;
  if (outP)
    dumpNamOutPath = outP;
  if (dumpNamInPath.empty() && dumpNamOutPath.empty())
    return;

  const uint32_t secs = readEnvU32("DUMP_NAM_SECONDS", 10);

  // If we already initialized, only re-init when the sample rate changes (likely due to
  // PipeWire format negotiation). This avoids locking in the placeholder startup rate.
  if (dumpNamMaxFrames != 0)
  {
    if (sr == dumpNamInitSr && secs == dumpNamInitSecs)
      return;

    std::printf("Dump: re-init due to SR/secs change (old sr=%u secs=%u -> new sr=%u secs=%u)\n",
                dumpNamInitSr, dumpNamInitSecs, sr, secs);
    std::fflush(stdout);
  }

  dumpNamInitSr = sr;
  dumpNamInitSecs = secs;
  dumpNamMaxFrames = sr * secs;
  dumpNamInWritePos.store(0, std::memory_order_relaxed);
  dumpNamOutWritePos.store(0, std::memory_order_relaxed);
  dumpNamInTotalWritten.store(0, std::memory_order_relaxed);
  dumpNamOutTotalWritten.store(0, std::memory_order_relaxed);
  if (!dumpNamInPath.empty())
    dumpNamIn.assign(dumpNamMaxFrames, 0.0f);
  if (!dumpNamOutPath.empty())
    dumpNamOut.assign(dumpNamMaxFrames, 0.0f);

  std::printf("Dump: enabled NAM dump for %u seconds (%u frames).\n", secs, dumpNamMaxFrames);
  if (!dumpNamInPath.empty())
    std::printf("Dump: DUMP_NAM_IN_WAV=%s\n", dumpNamInPath.c_str());
  if (!dumpNamOutPath.empty())
    std::printf("Dump: DUMP_NAM_OUT_WAV=%s\n", dumpNamOutPath.c_str());
  std::fflush(stdout);
}

static inline void dumpNamPushIn(const float *in, uint32_t nframes)
{
  if (dumpNamMaxFrames == 0)
    return;
  if (dumpNamIn.empty())
    return;
  uint32_t wp = dumpNamInWritePos.load(std::memory_order_relaxed);
  if (wp >= dumpNamMaxFrames)
    return;
  const uint32_t toWrite = std::min(nframes, dumpNamMaxFrames - wp);
  if (toWrite == 0)
    return;
  if (in)
    std::memcpy(&dumpNamIn[wp], in, sizeof(float) * toWrite);
  else
    std::memset(&dumpNamIn[wp], 0, sizeof(float) * toWrite);
  dumpNamInWritePos.store(wp + toWrite, std::memory_order_relaxed);
  dumpNamInTotalWritten.store(wp + toWrite, std::memory_order_relaxed);

  // Rate-limited progress print (helps detect if audio processing stalls).
  static std::atomic<uint32_t> lastPrinted{0};
  const uint32_t now = wp + toWrite;
  uint32_t prev = lastPrinted.load(std::memory_order_relaxed);
  if (now >= prev + 48000) // ~1 second at 48k
  {
    if (lastPrinted.compare_exchange_strong(prev, now, std::memory_order_relaxed))
    {
      const double denom = (sampleRate != 0) ? (double)sampleRate : 48000.0;
      std::printf("Dump: progress in=%u/%u (%.2fs)\n", now, dumpNamMaxFrames, (double)now / denom);
      std::fflush(stdout);
    }
  }
}

static inline void dumpNamPushOut(const float *out, uint32_t nframes)
{
  if (dumpNamMaxFrames == 0)
    return;
  if (dumpNamOut.empty())
    return;
  uint32_t wp = dumpNamOutWritePos.load(std::memory_order_relaxed);
  if (wp >= dumpNamMaxFrames)
    return;
  const uint32_t toWrite = std::min(nframes, dumpNamMaxFrames - wp);
  if (toWrite == 0)
    return;
  if (out)
    std::memcpy(&dumpNamOut[wp], out, sizeof(float) * toWrite);
  else
    std::memset(&dumpNamOut[wp], 0, sizeof(float) * toWrite);
  dumpNamOutWritePos.store(wp + toWrite, std::memory_order_relaxed);
  dumpNamOutTotalWritten.store(wp + toWrite, std::memory_order_relaxed);
}

static bool writeWavF32Mono(const std::string &path, const std::vector<float> &y, uint32_t nframes, uint32_t sr)
{
  if (path.empty() || nframes == 0)
    return false;
  SF_INFO info{};
  info.samplerate = (int)sr;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  SNDFILE *sf = sf_open(path.c_str(), SFM_WRITE, &info);
  if (!sf)
  {
    std::fprintf(stderr, "Dump: failed to open %s: %s\n", path.c_str(), sf_strerror(nullptr));
    return false;
  }
  std::vector<short> tmp(nframes);
  for (uint32_t i = 0; i < nframes; i++)
  {
    float s = y[i];
    if (s > 1.0f)
      s = 1.0f;
    if (s < -1.0f)
      s = -1.0f;
    tmp[i] = (short)std::lround(s * 32767.0f);
  }
  const sf_count_t wrote = sf_write_short(sf, tmp.data(), (sf_count_t)tmp.size());
  sf_close(sf);
  return wrote == (sf_count_t)tmp.size();
}

static void dumpNamFlush(uint32_t sr)
{
  std::lock_guard<std::mutex> lock(dumpNamFlushMutex);
  if (dumpNamMaxFrames == 0)
    return;

  const uint32_t inTotal = dumpNamInTotalWritten.load(std::memory_order_relaxed);
  const uint32_t outTotal = dumpNamOutTotalWritten.load(std::memory_order_relaxed);
  const uint32_t inWp = dumpNamInWritePos.load(std::memory_order_relaxed);
  const uint32_t outWp = dumpNamOutWritePos.load(std::memory_order_relaxed);

  std::printf("Dump: flush sr=%u maxFrames=%u in_wp=%u in_total=%u out_wp=%u out_total=%u\n",
              sr, dumpNamMaxFrames, inWp, inTotal, outWp, outTotal);
  std::fflush(stdout);

  auto slice = [&](const std::vector<float> &buf, uint32_t total) -> std::vector<float>
  {
    total = std::min(total, dumpNamMaxFrames);
    return std::vector<float>(buf.begin(), buf.begin() + total);
  };

  if (!dumpNamInPath.empty() && !dumpNamIn.empty())
  {
    const uint32_t total = inTotal;
    if (total > 0)
    {
      auto y = slice(dumpNamIn, total);
      if (writeWavF32Mono(dumpNamInPath, y, (uint32_t)y.size(), sr))
        std::printf("Dump: wrote %u frames (%.2fs) to %s\n",
                    (unsigned)y.size(),
                    (double)y.size() / (double)sr,
                    dumpNamInPath.c_str());
    }
  }
  if (!dumpNamOutPath.empty() && !dumpNamOut.empty())
  {
    const uint32_t total = outTotal;
    if (total > 0)
    {
      auto y = slice(dumpNamOut, total);
      if (writeWavF32Mono(dumpNamOutPath, y, (uint32_t)y.size(), sr))
        std::printf("Dump: wrote %u frames (%.2fs) to %s\n",
                    (unsigned)y.size(),
                    (double)y.size() / (double)sr,
                    dumpNamOutPath.c_str());
    }
  }
  std::fflush(stdout);
}

// Debug/passthrough mode: bypass NAM/IR/gate and copy capture to playback.
static std::atomic<bool> passthroughMode{false};
// DI-only mode: absolute minimum DSP path for debugging (raw capture -> output).
// This bypasses: noise gate, NAM, IR, softclip, etc. It still applies safety
// DC block + limiter to prevent blasts.
static std::atomic<bool> diOnlyMode{false};
// Debug: bypass the NAM model but keep the rest of the chain running.
// This is useful to isolate "buzzy" artifacts to NAM vs IR/other processing.
static std::atomic<bool> bypassNam{false};
// Debug: bypass only the IR convolver (keep NAM active).
static std::atomic<bool> bypassIr{false};
// Debug: run NAM at 2x sample rate via simple oversampling to reduce aliasing.
// This is a pragmatic fix for "buzzy" distortion that is actually aliasing.
static std::atomic<bool> namOversample2x{false};

// Input level normalization for NAM.
// Some NAM models embed an expected input level (dBu corresponding to 0 dBFS sine).
// If present, we can scale our input so we hit the model at its intended level.
// This can reduce harsh/buzzy behavior caused by over/under-driving the model.
static std::atomic<bool> namUseInputLevel{true};
static std::atomic<float> namInputLevelDbu{0.0f};
static std::atomic<bool> namHasInputLevel{false};

// Pre-model gain trim (helps avoid overdriving NAM models that were trained on lower DI levels).
// Env override: NAM_PRE_GAIN_DB (range clamped).
static std::atomic<float> namPreGainDb{-12.0f};
static std::atomic<float> namPreGainLin{1.0f};

// Process counters for quick diagnostics
static std::atomic<uint64_t> processCalls{0};
static std::atomic<uint64_t> captureProcessCalls{0};
// Wall-clock callback heartbeat (monotonic time in ms).
// These let us distinguish "engine is alive but PW stopped scheduling" from normal operation.
static std::atomic<uint64_t> lastPlaybackCbMs{0};
static std::atomic<uint64_t> lastCaptureCbMs{0};

// Stream scheduling diagnostics (best-effort).
static std::atomic<uint32_t> lastPlaybackNFrames{0};
static std::atomic<uint32_t> lastCaptureNFrames{0};
static std::atomic<uint64_t> playbackCbTotal{0};
static std::atomic<uint64_t> captureCbTotal{0};
static std::atomic<int64_t> lastPbTimeNow{0};
static std::atomic<int64_t> lastPbTimeRate{0};
static std::atomic<int64_t> lastPbTimeDelay{0};
static std::atomic<int64_t> lastCapTimeNow{0};
static std::atomic<int64_t> lastCapTimeRate{0};
static std::atomic<int64_t> lastCapTimeDelay{0};
static std::atomic<uint64_t> playbackFramesTotal{0};
static std::atomic<uint64_t> captureFramesTotal{0};
static std::atomic<uint64_t> nonZeroInCalls{0};
static std::atomic<float> peakCaptureRaw{0.0f};
static std::atomic<bool> captureDebugPrint{true};
static std::atomic<bool> printedMeta{false};
static std::atomic<uint64_t> debugCalls{0};
static std::atomic<uint64_t> earlyNoBuf{0};
static std::atomic<uint64_t> earlyNoCaptureBuf{0};
static std::atomic<uint64_t> earlyBadSpa{0};
static std::atomic<uint64_t> earlyMissingPtrs{0};
static std::atomic<uint64_t> earlyBadStride{0};
static std::atomic<uint64_t> earlyBadFrames{0};

// Capture-specific early-return counters (why capture didn't produce samples)
static std::atomic<uint64_t> capEarlyNoStream{0};
static std::atomic<uint64_t> capEarlyNoSpa{0};
static std::atomic<uint64_t> capEarlyNoDataOrChunk{0};
static std::atomic<uint64_t> capEarlyBadStride{0};
static std::atomic<uint64_t> capEarlyBadFrames{0};

// Env-driven debug toggles (read at startup in loadConfig()).
// These are meant to isolate PipeWire format/stride issues from DSP/model issues.
static std::atomic<bool> forceCapturePlanar{false};
static std::atomic<bool> forceCaptureInterleaved{false};

// Debug: force NAM output to zero (but keep the engine/graph running).
// If static persists with this enabled, it's not coming from NAM processing itself.
static std::atomic<bool> namForceBypassOutput{false};

// Debug: DI monitor modes (env-based).
// NAM_DI_MONITOR=1: mix a little DI into output (helps compare "clean" vs NAM noise).
// NAM_WET_MUTE=1: output DI only (mutes NAM/IR), useful to confirm static is exclusively in wet path.
static std::atomic<bool> namDiMonitor{false};
static std::atomic<bool> namWetMute{false};

// Absolute debug: force output to zero at the very end.
// If static is still audible with this enabled, the noise is external to this process (wiring/mixer/device).
static std::atomic<bool> forceOutputZero{false};

// Post-NAM smoothing: simple 1-pole lowpass to knock down aliasing-like fizz.
// Env:
//   NAM_POST_LPF_ENABLE=1
//   NAM_POST_LPF_HZ=<cutoff Hz> (default 8000)
static std::atomic<bool> namPostLpfEnable{false};
static std::atomic<float> namPostLpfHz{8000.0f};
static float namPostLpf_y1 = 0.0f;

// Oversampling anti-alias filter (used only in NAM_OS_2X path).
// This is the *correct* missing piece: lowpass before decimation.
// Env: NAM_OS_2X_LPF_HZ (default 12000)
static std::atomic<float> namOs2xLpfHz{12000.0f};

// Debug: disable runtime ResetAndPrewarm() calls triggered by quantum changes.
// If frequent resets are causing audible buzz/zipper, this will greatly reduce it.
// Env: NAM_DISABLE_RUNTIME_RESET=1
static std::atomic<bool> namDisableRuntimeReset{false};
static std::atomic<uint64_t> namRuntimeResets{0};

// -------------------- Capture->Playback handoff --------------------
// With separate PipeWire streams, it's unreliable to dequeue capture buffers
// from inside the playback stream's process callback. Instead, we capture in a
// capture-specific process callback and hand off the latest mono block to the
// playback callback via a tiny lock-free buffer.
static constexpr uint32_t kMaxQuantum = 8192;
static std::atomic<uint32_t> capFramesAvail{0};
static std::vector<float> capMono;
static std::atomic<uint32_t> gCaptureChannels{2};

static inline void ensureCapBuffer(uint32_t nframes)
{
  if (nframes == 0 || nframes > kMaxQuantum)
    nframes = requestedQuantum;
  if (nframes == 0 || nframes > kMaxQuantum)
    nframes = 128;
  if (capMono.size() < nframes)
    capMono.resize(nframes);
}

// Simple noise gate to kill constant DSP hiss at idle.
// We gate *before* NAM+IR to avoid amplifying model noise/denormals.
static std::atomic<bool> gateEnabled{true};
static float gateEnv = 0.0f;
static float gateGain = 0.0f;
static bool gateOpen = false;

// Gate helper for DI/passthrough paths. This is intentionally simple and RT-safe.
// It uses the already-computed peakCaptureRaw as an envelope input.
static inline float gateForBypass()
{
  if (!gateEnabled.load(std::memory_order_relaxed))
    return 1.0f;

  // Open/close thresholds in linear amplitude.
  // These keep idle hiss down but open quickly on guitar transients.
  constexpr float openTh = 0.0010f;  // ~ -60 dBFS
  constexpr float closeTh = 0.0003f; // ~ -70 dBFS

  // Envelope smoothing.
  constexpr float envAtk = 0.04f;
  constexpr float envRel = 0.002f;

  // Gain smoothing.
  constexpr float gainAtk = 0.25f;
  constexpr float gainRel = 0.02f;

  float inPk = peakCaptureRaw.load(std::memory_order_relaxed);
  if (inPk > gateEnv)
    gateEnv = gateEnv + (inPk - gateEnv) * envAtk;
  else
    gateEnv = gateEnv + (inPk - gateEnv) * envRel;

  if (!gateOpen)
  {
    if (gateEnv >= openTh)
      gateOpen = true;
  }
  else
  {
    if (gateEnv <= closeTh)
      gateOpen = false;
  }

  const float target = gateOpen ? 1.0f : 0.0f;
  const float k = gateOpen ? gainAtk : gainRel;
  gateGain = gateGain + (target - gateGain) * k;
  return gateGain;
}

// Safety processing (RT-safe): DC blocker + hard limiter.
// This is primarily to prevent runaway full-scale noise events.
static std::atomic<float> safetyLimiterAbs{0.2f}; // ~ -14 dBFS
static float dc_x1 = 0.0f;
static float dc_y1 = 0.0f;

static inline float dcBlock(float x)
{
  // 1-pole high-pass around a few Hz. Standard DC-blocker.
  // y[n] = x[n] - x[n-1] + R*y[n-1]
  constexpr float R = 0.995f;
  float y = x - dc_x1 + R * dc_y1;
  dc_x1 = x;
  dc_y1 = y;
  return y;
}

static void updatePeak(std::atomic<float> &dst, float v)
{
  float cur = dst.load(std::memory_order_relaxed);
  if (v > cur)
    dst.store(v, std::memory_order_relaxed);
}

// Config
static std::atomic<float> inputTrimDb{0.0f};
static std::atomic<float> inputTrimLin{1.0f};
static std::string namModelPath;
static std::string irPath;

static inline void zeroPlanarStereo(uint8_t *base, uint32_t stride, uint32_t nframes)
{
  if (!base || stride < sizeof(float) || nframes == 0)
    return;
  for (uint32_t i = 0; i < nframes; i++)
  {
    *(float *)(base + (size_t)i * stride) = 0.0f;
    if (stride >= 2 * sizeof(float))
      *(float *)(base + (size_t)i * stride + sizeof(float)) = 0.0f;
  }
}

static inline void zeroMono(float *dst, uint32_t nframes)
{
  if (!dst || nframes == 0)
    return;
  for (uint32_t i = 0; i < nframes; i++)
    dst[i] = 0.0f;
}

// NAM
static std::unique_ptr<nam::DSP> gModel;
static std::atomic<bool> modelReady{false};

static void requestQuit()
{
  running.store(false, std::memory_order_relaxed);
  threadLoopSignal();
}
static std::vector<float> namIn;
static std::vector<float> namOut;

// IR
static FFTConvolverPartitioned gIR;
static std::atomic<bool> irReady{false};
static std::vector<float> gIrCached;
static std::vector<float> irBlockIn;
static std::vector<float> irBlockOut;

static inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
static inline float clampf(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi
                                                                                          : v; }

static void onSignal(int) { requestQuit(); }

static void meterThread()
{
  while (running.load(std::memory_order_relaxed))
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();

    uint64_t p = processCalls.exchange(0, std::memory_order_relaxed);
    uint64_t cp = captureProcessCalls.exchange(0, std::memory_order_relaxed);
    const uint64_t pbCb = playbackCbTotal.exchange(0, std::memory_order_relaxed);
    const uint64_t capCb = captureCbTotal.exchange(0, std::memory_order_relaxed);
    const uint32_t pbNf = lastPlaybackNFrames.load(std::memory_order_relaxed);
    const uint32_t capNf = lastCaptureNFrames.load(std::memory_order_relaxed);
    const int64_t pbNow = lastPbTimeNow.load(std::memory_order_relaxed);
    const int64_t pbRate = lastPbTimeRate.load(std::memory_order_relaxed);
    const int64_t pbDelay = lastPbTimeDelay.load(std::memory_order_relaxed);
    const int64_t capNow = lastCapTimeNow.load(std::memory_order_relaxed);
    const int64_t capRate = lastCapTimeRate.load(std::memory_order_relaxed);
    const int64_t capDelay = lastCapTimeDelay.load(std::memory_order_relaxed);

    const uint64_t pbLast = lastPlaybackCbMs.load(std::memory_order_relaxed);
    const uint64_t capLast = lastCaptureCbMs.load(std::memory_order_relaxed);
    const uint64_t pbIdle = (pbLast == 0) ? 0 : (nowMs - pbLast);
    const uint64_t capIdle = (capLast == 0) ? 0 : (nowMs - capLast);
    const uint64_t pbFrames = playbackFramesTotal.load(std::memory_order_relaxed);
    const uint64_t capFrames = captureFramesTotal.load(std::memory_order_relaxed);

    static uint64_t lastPbFrames = 0;
    static uint64_t lastCapFrames = 0;
    static uint64_t lastNowMs = 0;
    const uint64_t dtMs = (lastNowMs == 0) ? 1000 : (nowMs - lastNowMs);
    const uint64_t dPbFrames = (lastPbFrames == 0) ? 0 : (pbFrames - lastPbFrames);
    const uint64_t dCapFrames = (lastCapFrames == 0) ? 0 : (capFrames - lastCapFrames);
    lastPbFrames = pbFrames;
    lastCapFrames = capFrames;
    lastNowMs = nowMs;

    const double dtSec = (dtMs > 0) ? ((double)dtMs / 1000.0) : 1.0;
    const double pbFps = (dtSec > 0.0) ? ((double)dPbFrames / dtSec) : 0.0;
    const double capFps = (dtSec > 0.0) ? ((double)dCapFrames / dtSec) : 0.0;
    const double sr = (sampleRate != 0) ? (double)sampleRate : 48000.0;
    const double pbRealtime = (sr > 0.0) ? (pbFps / sr) : 0.0;
    const double capRealtime = (sr > 0.0) ? (capFps / sr) : 0.0;

    float pkIn = peakInput.exchange(0.0f, std::memory_order_relaxed);
    float pkCap = peakCaptureRaw.exchange(0.0f, std::memory_order_relaxed);
    float pkNamIn = peakNamIn.exchange(0.0f, std::memory_order_relaxed);
    float pkNam = peakNamOut.exchange(0.0f, std::memory_order_relaxed);
    float pkIr = peakIrOut.exchange(0.0f, std::memory_order_relaxed);
    float pkOut = peakFinalOut.exchange(0.0f, std::memory_order_relaxed);

    // Gate state telemetry (best-effort; gate variables are written from RT thread).
    const float gEnv = gateEnv;
    const float gGain = gateGain;

    auto toDb = [](float peak) -> float
    {
      if (peak < 0.000001f)
        return -120.0f;
      return 20.0f * std::log10(peak);
    };

    std::printf("[METER] In: %6.1f dBFS | NAMin: %6.1f dBFS | NAM: %6.1f dBFS | IR: %6.1f dBFS | Out: %6.1f dBFS\n",
                toDb(pkIn), toDb(pkNamIn), toDb(pkNam), toDb(pkIr), toDb(pkOut));
    std::printf("[RT] process=%llu/s\n", (unsigned long long)p);
    std::printf("[RT] capture_process=%llu/s\n", (unsigned long long)cp);
    const uint32_t q = (pbNf != 0) ? pbNf : ((requestedQuantum != 0) ? requestedQuantum : 128u);
    const double expCbps = (sampleRate != 0 && q != 0) ? ((double)sampleRate / (double)q) : 0.0;
    std::printf("[SCHED] exp_cbps=%.1f pb_cb=%llu cap_cb=%llu pb_nf=%u cap_nf=%u\n",
                expCbps,
                (unsigned long long)pbCb,
                (unsigned long long)capCb,
                (unsigned)pbNf,
                (unsigned)capNf);
    std::printf("[TIME] pb_now=%lld pb_rate=%lld pb_delay=%lld | cap_now=%lld cap_rate=%lld cap_delay=%lld\n",
                (long long)pbNow,
                (long long)pbRate,
                (long long)pbDelay,
                (long long)capNow,
                (long long)capRate,
                (long long)capDelay);
    std::printf("[HB] pb_idle=%llums cap_idle=%llums pb_frames=%llu cap_frames=%llu\n",
                (unsigned long long)pbIdle,
                (unsigned long long)capIdle,
                (unsigned long long)pbFrames,
                (unsigned long long)capFrames);
    std::printf("[RATE] dt=%.3fs sr=%u pb_fps=%.1f cap_fps=%.1f pb_x=%.3f cap_x=%.3f\n",
                dtSec,
                (unsigned)sampleRate,
                pbFps,
                capFps,
                pbRealtime,
                capRealtime);

    // Effective quantum diagnostics: if PipeWire gives us a different quantum than we think,
    // callback cadence will mismatch. pb_fps is authoritative over a 1s window.
    const double effQ = (pbCb > 0) ? (pbFps / (double)pbCb) : 0.0;
    const double expCbpsFromFrames = (effQ > 0.0) ? (sr / effQ) : 0.0;
    std::printf("[Q] pb_cbps=%.1f eff_q=%.1f pb_nf=%u req_q=%u exp_cbps(eff)=%.1f\n",
                (double)pbCb / dtSec,
                effQ,
                (unsigned)pbNf,
                (unsigned)requestedQuantum,
                expCbpsFromFrames);
    std::printf("[NAM] runtime_resets=%llu/s\n",
                (unsigned long long)namRuntimeResets.exchange(0, std::memory_order_relaxed));
    std::printf("[IN] raw=%6.1f dBFS nonzero=%llu/s\n",
                toDb(pkCap),
                (unsigned long long)nonZeroInCalls.exchange(0, std::memory_order_relaxed));

    std::printf("[GATE] env=%6.1f dBFS gain=%5.2f\n", toDb(gEnv), (double)gGain);
    std::printf("[PWRET] nobuf=%llu/s badspa=%llu/s missptr=%llu/s badstride=%llu/s badframes=%llu/s\n",
                (unsigned long long)earlyNoBuf.exchange(0, std::memory_order_relaxed),
                (unsigned long long)earlyBadSpa.exchange(0, std::memory_order_relaxed),
                (unsigned long long)earlyMissingPtrs.exchange(0, std::memory_order_relaxed),
                (unsigned long long)earlyBadStride.exchange(0, std::memory_order_relaxed),
                (unsigned long long)earlyBadFrames.exchange(0, std::memory_order_relaxed));
    std::printf("[PWRET2] nocap=%llu/s\n",
                (unsigned long long)earlyNoCaptureBuf.exchange(0, std::memory_order_relaxed));

    std::printf("[CAPRET] nostream=%llu/s nospa=%llu/s nodata=%llu/s badstride=%llu/s badframes=%llu/s\n",
                (unsigned long long)capEarlyNoStream.exchange(0, std::memory_order_relaxed),
                (unsigned long long)capEarlyNoSpa.exchange(0, std::memory_order_relaxed),
                (unsigned long long)capEarlyNoDataOrChunk.exchange(0, std::memory_order_relaxed),
                (unsigned long long)capEarlyBadStride.exchange(0, std::memory_order_relaxed),
                (unsigned long long)capEarlyBadFrames.exchange(0, std::memory_order_relaxed));
    std::fflush(stdout);
  }
}

// -------------------- Config --------------------
static void loadConfig()
{
  const char *path = "/opt/pedal/config/chain.json";
  try
  {
    // Enable optional realtime dump of NAM input/output.
    // Needs sample rate; use current global sampleRate (updated after format negotiation if needed).
    dumpNamInit(sampleRate);

    // Environment overrides (fast iteration without editing JSON).
    // NAM_PRE_GAIN_DB: pre-model trim in dB (default -12).
    if (const char *e = std::getenv("NAM_PRE_GAIN_DB"))
    {
      float db = std::strtof(e, nullptr);
      if (std::isfinite(db))
      {
        db = clampf(db, -60.0f, 24.0f);
        namPreGainDb.store(db, std::memory_order_relaxed);
        namPreGainLin.store(dbToLin(db), std::memory_order_relaxed);
      }
    }

    // Quick debug toggles (env-based):
    // NAM_OS_2X=1 -> enable 2x oversampling
    // NAM_DISABLE_GATE=1 -> disable noise gate
    // CAPTURE_FORCE_PLANAR=1 -> treat capture as planar (use datas[0]/datas[1])
    // CAPTURE_FORCE_INTERLEAVED=1 -> treat capture as interleaved (LR in datas[0])
    if (const char *e = std::getenv("NAM_OS_2X"))
    {
      if (std::atoi(e) != 0)
        namOversample2x.store(true, std::memory_order_relaxed);
    }
    if (const char *e = std::getenv("NAM_DISABLE_GATE"))
    {
      if (std::atoi(e) != 0)
        gateEnabled.store(false, std::memory_order_relaxed);
    }

    if (const char *e = std::getenv("NAM_REFERENCE_MODE"))
    {
      if (std::atoi(e) != 0)
      {
        namReferenceMode.store(true, std::memory_order_relaxed);
        // Force a minimal reference chain.
        namOversample2x.store(false, std::memory_order_relaxed);
        gateEnabled.store(false, std::memory_order_relaxed);
        std::printf("NAM_REFERENCE_MODE=1: forcing minimal chain (no OS2x, gate disabled, IR disabled) and requiring 48kHz.\n");
        std::fflush(stdout);
      }
    }
    if (const char *e = std::getenv("CAPTURE_FORCE_PLANAR"))
    {
      if (std::atoi(e) != 0)
        forceCapturePlanar.store(true, std::memory_order_relaxed);
    }
    if (const char *e = std::getenv("CAPTURE_FORCE_INTERLEAVED"))
    {
      if (std::atoi(e) != 0)
        forceCaptureInterleaved.store(true, std::memory_order_relaxed);
    }

    if (const char *e = std::getenv("NAM_FORCE_BYPASS_OUTPUT"))
    {
      if (std::atoi(e) != 0)
        namForceBypassOutput.store(true, std::memory_order_relaxed);
    }

    if (const char *e = std::getenv("NAM_DI_MONITOR"))
    {
      if (std::atoi(e) != 0)
        namDiMonitor.store(true, std::memory_order_relaxed);
    }
    if (const char *e = std::getenv("NAM_WET_MUTE"))
    {
      if (std::atoi(e) != 0)
        namWetMute.store(true, std::memory_order_relaxed);
    }

    if (const char *e = std::getenv("FORCE_OUTPUT_ZERO"))
    {
      if (std::atoi(e) != 0)
        forceOutputZero.store(true, std::memory_order_relaxed);
    }

    if (const char *e = std::getenv("NAM_POST_LPF_ENABLE"))
    {
      if (std::atoi(e) != 0)
        namPostLpfEnable.store(true, std::memory_order_relaxed);
    }
    if (const char *e = std::getenv("NAM_POST_LPF_HZ"))
    {
      float hz = std::strtof(e, nullptr);
      if (std::isfinite(hz))
        namPostLpfHz.store(clampf(hz, 800.0f, 20000.0f), std::memory_order_relaxed);
    }

    if (const char *e = std::getenv("NAM_OS_2X_LPF_HZ"))
    {
      float hz = std::strtof(e, nullptr);
      if (std::isfinite(hz))
        namOs2xLpfHz.store(clampf(hz, 1000.0f, 20000.0f), std::memory_order_relaxed);
    }

    if (const char *e = std::getenv("NAM_DISABLE_RUNTIME_RESET"))
    {
      if (std::atoi(e) != 0)
        namDisableRuntimeReset.store(true, std::memory_order_relaxed);
    }

    std::ifstream f(path);
    if (!f.is_open())
    {
      std::printf("Config: could not open %s (using defaults)\n", path);
      std::printf("Config: nam_pre_gain_db=%.1f dB (env NAM_PRE_GAIN_DB)\n",
                  (double)namPreGainDb.load(std::memory_order_relaxed));
      return;
    }

    nlohmann::json j;
    f >> j;

    if (j.contains("audio") && j["audio"].contains("inputTrimDb"))
    {
      float db = j["audio"]["inputTrimDb"].get<float>();
      db = clampf(db, -24.0f, 24.0f);
      inputTrimDb.store(db, std::memory_order_relaxed);
      inputTrimLin.store(dbToLin(db), std::memory_order_relaxed);
    }

    if (j.contains("chain") && j["chain"].contains("namModelPath"))
    {
      namModelPath = j["chain"]["namModelPath"].get<std::string>();
    }

    if (j.contains("chain") && j["chain"].contains("irPath"))
    {
      irPath = j["chain"]["irPath"].get<std::string>();
    }

    if (j.contains("debug") && j["debug"].contains("passthrough"))
    {
      passthroughMode.store(j["debug"]["passthrough"].get<bool>(), std::memory_order_relaxed);
    }

    if (j.contains("debug") && j["debug"].contains("di_only"))
    {
      diOnlyMode.store(j["debug"]["di_only"].get<bool>(), std::memory_order_relaxed);
    }
    if (j.contains("debug") && j["debug"].contains("bypass_nam"))
    {
      bypassNam.store(j["debug"]["bypass_nam"].get<bool>(), std::memory_order_relaxed);
    }

    if (j.contains("debug") && j["debug"].contains("bypass_ir"))
    {
      bypassIr.store(j["debug"]["bypass_ir"].get<bool>(), std::memory_order_relaxed);
    }

    if (j.contains("debug") && j["debug"].contains("nam_oversample_2x"))
    {
      namOversample2x.store(j["debug"]["nam_oversample_2x"].get<bool>(), std::memory_order_relaxed);
    }

    if (j.contains("debug") && j["debug"].contains("nam_use_input_level"))
    {
      namUseInputLevel.store(j["debug"]["nam_use_input_level"].get<bool>(), std::memory_order_relaxed);
    }

    if (j.contains("debug") && j["debug"].contains("nam_input_level_dbu"))
    {
      // User override: dBu corresponding to 0 dBFS sine.
      namInputLevelDbu.store(j["debug"]["nam_input_level_dbu"].get<float>(), std::memory_order_relaxed);
      namHasInputLevel.store(true, std::memory_order_relaxed);
    }

    std::printf("Config: inputTrimDb=%.1f dB\n", inputTrimDb.load());
    std::printf("Config: namModelPath=%s\n", namModelPath.empty() ? "(empty)" : namModelPath.c_str());
    std::printf("Config: irPath=%s\n", irPath.empty() ? "(empty)" : irPath.c_str());
    std::printf("Config: passthrough=%s\n", passthroughMode.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: di_only=%s\n", diOnlyMode.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: bypass_nam=%s\n", bypassNam.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: bypass_ir=%s\n", bypassIr.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: nam_oversample_2x=%s\n", namOversample2x.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: nam_pre_gain_db=%.1f dB (env NAM_PRE_GAIN_DB)\n",
                (double)namPreGainDb.load(std::memory_order_relaxed));
    std::printf("Config: nam_use_input_level=%s\n", namUseInputLevel.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: nam_input_level_dbu=%.2f (%s)\n",
                (double)namInputLevelDbu.load(std::memory_order_relaxed),
                namHasInputLevel.load(std::memory_order_relaxed) ? "set" : "unset");

    std::printf("Config: gate=%s (env NAM_DISABLE_GATE)\n",
                gateEnabled.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: capture_force_planar=%s (env CAPTURE_FORCE_PLANAR)\n",
                forceCapturePlanar.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: capture_force_interleaved=%s (env CAPTURE_FORCE_INTERLEAVED)\n",
                forceCaptureInterleaved.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: nam_force_bypass_output=%s (env NAM_FORCE_BYPASS_OUTPUT)\n",
                namForceBypassOutput.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: nam_di_monitor=%s (env NAM_DI_MONITOR)\n",
                namDiMonitor.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: nam_wet_mute=%s (env NAM_WET_MUTE)\n",
                namWetMute.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");

    std::printf("Config: force_output_zero=%s (env FORCE_OUTPUT_ZERO)\n",
                forceOutputZero.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");

    std::printf("Config: nam_post_lpf=%s (env NAM_POST_LPF_ENABLE)\n",
                namPostLpfEnable.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
    std::printf("Config: nam_post_lpf_hz=%.0f (env NAM_POST_LPF_HZ)\n",
                (double)namPostLpfHz.load(std::memory_order_relaxed));

    std::printf("Config: nam_os_2x_lpf_hz=%.0f (env NAM_OS_2X_LPF_HZ)\n",
                (double)namOs2xLpfHz.load(std::memory_order_relaxed));

    std::printf("Config: nam_disable_runtime_reset=%s (env NAM_DISABLE_RUNTIME_RESET)\n",
                namDisableRuntimeReset.load(std::memory_order_relaxed) ? "ENABLED" : "disabled");
  }
  catch (const std::exception &e)
  {
    std::printf("Config: parse error: %s\n", e.what());
  }
}

// -------------------- UDP control --------------------
static void udpControlThread()
{
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
  {
    std::perror("socket");
    return;
  }

  int yes = 1;
  (void)::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9000);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
  {
    std::perror("bind");
    ::close(sock);
    return;
  }

  std::printf("Control: UDP localhost:9000 (send: TRIM_DB <value>)\n");

  char buf[256];
  while (running.load(std::memory_order_relaxed))
  {
    sockaddr_in src{};
    socklen_t sl = sizeof(src);
    int n = ::recvfrom(sock, buf, sizeof(buf) - 1, MSG_DONTWAIT, (sockaddr *)&src, &sl);
    if (n < 0)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    buf[n] = 0;

    float valDb = 0.0f;
    if (std::sscanf(buf, "TRIM_DB %f", &valDb) == 1)
    {
      valDb = clampf(valDb, -24.0f, 24.0f);
      inputTrimDb.store(valDb, std::memory_order_relaxed);
      inputTrimLin.store(dbToLin(valDb), std::memory_order_relaxed);
      std::printf("Trim set to %.1f dB\n", valDb);
      std::fflush(stdout);
    }
    else
    {
      std::printf("Unknown cmd: %s\n", buf);
      std::fflush(stdout);
    }
  }

  ::close(sock);
}

// -------------------- PipeWire diagnostics --------------------
static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
  (void)data;
  std::fprintf(stderr, "[PWCORE] error id=%u seq=%d res=%d: %s\n", id, seq, res, message ? message : "(null)");
  std::fflush(stderr);
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static void on_stream_state_changed(void *data, enum pw_stream_state old_state, enum pw_stream_state state, const char *error)
{
  const char *tag = data ? (const char *)data : "stream";
  std::fprintf(stderr, "[PWSTREAM] %s state %d -> %d%s%s\n",
               tag,
               (int)old_state,
               (int)state,
               error ? " err=" : "",
               error ? error : "");
  std::fflush(stderr);
  printedStreamState.store(true, std::memory_order_relaxed);

  // By default we keep running even if PipeWire drops the stream(s), so we can
  // observe scheduling and recover without the whole process exiting.
  // Set PW_EXIT_ON_PW_DISCONNECT=1 to restore the old behavior.
  const bool exitOnDisconnect = (std::getenv("PW_EXIT_ON_PW_DISCONNECT") && std::atoi(std::getenv("PW_EXIT_ON_PW_DISCONNECT")) != 0);
  if (exitOnDisconnect)
  {
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
      running.store(false, std::memory_order_relaxed);
  }
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
  const char *tag = data ? (const char *)data : "stream";
  if (!param)
    return;
  if (id != SPA_PARAM_Format)
    return;

  struct spa_audio_info_raw info;
  std::memset(&info, 0, sizeof(info));
  if (spa_format_audio_raw_parse(param, &info) < 0)
  {
    std::fprintf(stderr, "[PWSTREAM] Format param received (failed to parse)\n");
    std::fflush(stderr);
    return;
  }

  // Print only once to avoid spam, but do apply rate updates every time.
  if (!printedFormat.exchange(true, std::memory_order_relaxed))
  {
    std::fprintf(stderr, "[PWSTREAM] %s negotiated format: rate=%u channels=%u format=%d\n",
                 tag, info.rate, info.channels, (int)info.format);
    std::fflush(stderr);
  }

  if (info.rate != 0 && info.rate != sampleRate)
  {
    sampleRate = info.rate;
    std::fprintf(stderr, "[PWSTREAM] Using negotiated sampleRate=%u\n", sampleRate);
    std::fflush(stderr);
  }

  if (info.channels != 0)
  {
    if (std::strcmp(tag, "capture") == 0)
      gCaptureChannels.store(info.channels, std::memory_order_relaxed);
  }

  if (namReferenceMode.load(std::memory_order_relaxed) && sampleRate != 48000)
  {
    std::fprintf(stderr,
                 "[PWSTREAM] NAM_REFERENCE_MODE requires 48000 Hz, but negotiated %u Hz. Forcing output mute.\n",
                 sampleRate);
    std::fflush(stderr);
    forceOutputZero.store(true, std::memory_order_relaxed);
  }

  // Initialize/re-init dumps based on the real negotiated rate.
  dumpNamInit(sampleRate);
  std::fflush(stderr);
}

static inline void clearPlayback(struct pw_buffer *outBuf)
{
  if (!outBuf || !outBuf->buffer)
    return;

  struct spa_buffer *outSpa = outBuf->buffer;
  if (outSpa->n_datas < 1)
    return;

  struct spa_data *d0 = &outSpa->datas[0];
  if (!d0->data || !d0->chunk)
    return;

  const bool splitPlanar = (outSpa->n_datas >= 2);
  struct spa_data *d1 = splitPlanar ? &outSpa->datas[1] : &outSpa->datas[0];
  if (splitPlanar && (!d1->data || !d1->chunk))
    d1 = d0;

  // Default stride depends on layout:
  //  - planar: 1 float per frame per plane
  //  - interleaved: 2 floats per frame
  const uint32_t defaultStride = splitPlanar ? (uint32_t)sizeof(float) : (uint32_t)(2 * sizeof(float));
  const uint32_t stride0 = (d0->chunk->stride != 0) ? (uint32_t)d0->chunk->stride : defaultStride;
  const uint32_t stride1 = (d1->chunk->stride != 0) ? (uint32_t)d1->chunk->stride : defaultStride;

  uint32_t nframes = 0u;
  if (!splitPlanar)
  {
    nframes = (stride0 >= defaultStride) ? (uint32_t)(d0->chunk->size / stride0) : 0u;
  }
  else
  {
    const uint32_t n0 = (stride0 >= defaultStride) ? (uint32_t)(d0->chunk->size / stride0) : 0u;
    const uint32_t n1 = (stride1 >= defaultStride) ? (uint32_t)(d1->chunk->size / stride1) : 0u;
    nframes = (n0 != 0u && n1 != 0u) ? std::min(n0, n1) : 0u;
  }
  if (nframes == 0 || nframes > 8192)
    nframes = requestedQuantum;

  if (!splitPlanar)
  {
    zeroPlanarStereo((uint8_t *)d0->data + d0->chunk->offset, stride0, nframes);
    d0->chunk->size = nframes * stride0;
    if (d0->chunk->stride == 0)
      d0->chunk->stride = stride0;
  }
  else
  {
    // Zero both planes so we never leak old garbage on one channel.
    zeroMono((float *)((uint8_t *)d0->data + d0->chunk->offset), nframes);
    zeroMono((float *)((uint8_t *)d1->data + d1->chunk->offset), nframes);
    d0->chunk->size = nframes * stride0;
    d1->chunk->size = nframes * stride1;
    if (d0->chunk->stride == 0)
      d0->chunk->stride = stride0;
    if (d1->chunk->stride == 0)
      d1->chunk->stride = stride1;
  }
}

// -------------------- PipeWire Duplex Callback (capture + playback) --------------------
// One stream => one callback => one clock domain.
// We read input buffer(s), run DSP, and write output buffer(s) for the same quantum.
static void on_duplex_process(void *userdata)
{
  const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  lastPlaybackCbMs.store(nowMs, std::memory_order_relaxed);
  lastCaptureCbMs.store(nowMs, std::memory_order_relaxed);

  processCalls.fetch_add(1, std::memory_order_relaxed);
  captureProcessCalls.fetch_add(1, std::memory_order_relaxed);
  // These are the counters used by the existing meterThread() output.
  playbackCbTotal.fetch_add(1, std::memory_order_relaxed);
  captureCbTotal.fetch_add(1, std::memory_order_relaxed);
  debugCalls.fetch_add(1, std::memory_order_relaxed);

  if (!duplex_stream)
  {
    earlyNoBuf.fetch_add(1, std::memory_order_relaxed);
    capEarlyNoStream.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Duplex stream: one pw_buffer contains both input and output SPA data.
  // Important: you must dequeue exactly ONE buffer per cycle and queue it back once.
  struct pw_buffer *buf = pw_stream_dequeue_buffer(duplex_stream);
  if (!buf)
  {
    earlyNoBuf.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  struct spa_buffer *spa = buf->buffer;
  if (!spa || spa->n_datas < 1)
  {
    earlyBadSpa.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(duplex_stream, buf);
    return;
  }

  // For full-duplex, PipeWire commonly exposes a single interleaved buffer for IO.
  // We'll treat datas[0] as the shared IO area; if a second plane exists, use it.
  struct spa_data *d_in0 = &spa->datas[0];
  struct spa_data *d_in1 = (spa->n_datas >= 2) ? &spa->datas[1] : &spa->datas[0];
  if (!d_in0->data || !d_in0->chunk)
  {
    capEarlyNoDataOrChunk.fetch_add(1, std::memory_order_relaxed);
    // Can't read input; output silence.
    clearPlayback(buf);
    pw_stream_queue_buffer(duplex_stream, buf);
    return;
  }

  struct spa_data *d_out0 = &spa->datas[0];
  if (!d_out0->data || !d_out0->chunk)
  {
    earlyMissingPtrs.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(duplex_stream, buf);
    return;
  }

  bool inSplitPlanar = (spa->n_datas >= 2);
  if (forceCapturePlanar.load(std::memory_order_relaxed))
    inSplitPlanar = true;
  if (forceCaptureInterleaved.load(std::memory_order_relaxed))
    inSplitPlanar = false;
  if (inSplitPlanar && spa->n_datas < 2)
    inSplitPlanar = false;

  const bool outSplitPlanar = (spa->n_datas >= 2);
  struct spa_data *d_out1 = outSplitPlanar ? &spa->datas[1] : &spa->datas[0];

  // Stride semantics:
  //  - planar: stride ~ sizeof(float)
  //  - interleaved stereo: stride ~ 2*sizeof(float)
  const uint32_t inDefaultStride = inSplitPlanar ? (uint32_t)sizeof(float) : (uint32_t)(2 * sizeof(float));
  const uint32_t inStride0 = (d_in0->chunk->stride != 0) ? (uint32_t)d_in0->chunk->stride : inDefaultStride;
  if (inStride0 < inDefaultStride)
  {
    capEarlyBadStride.fetch_add(1, std::memory_order_relaxed);
    clearPlayback(buf);
    pw_stream_queue_buffer(duplex_stream, buf);
    return;
  }

  const uint32_t outDefaultStride = outSplitPlanar ? (uint32_t)sizeof(float) : (uint32_t)(2 * sizeof(float));
  const uint32_t outStride0 = (d_out0->chunk->stride != 0) ? (uint32_t)d_out0->chunk->stride : outDefaultStride;
  if (outStride0 < outDefaultStride)
  {
    earlyMissingPtrs.fetch_add(1, std::memory_order_relaxed);
    clearPlayback(buf);
    pw_stream_queue_buffer(duplex_stream, buf);
    return;
  }

  uint8_t *inBase0 = (uint8_t *)d_in0->data + d_in0->chunk->offset;
  uint8_t *inBase1 = (uint8_t *)d_in1->data + d_in1->chunk->offset;

  // Determine nframes from output (authoritative for what we must fill), then
  // clamp against capture availability.
  uint32_t nframes = (uint32_t)(d_out0->chunk->size / outStride0);
  if (nframes == 0 || nframes > kMaxQuantum)
    nframes = requestedQuantum;
  if (nframes == 0 || nframes > kMaxQuantum)
    nframes = 128;

  // Compute input frame count defensively.
  const uint32_t inFrameBytes = inSplitPlanar ? (uint32_t)sizeof(float) : (uint32_t)(2 * sizeof(float));
  uint32_t inFrames = (uint32_t)(d_in0->chunk->size / inFrameBytes);
  if (inFrames == 0)
    inFrames = nframes;
  if (inFrames > kMaxQuantum)
    inFrames = nframes;

  const uint32_t capN = std::min(inFrames, nframes);
  ensureCapBuffer(capN);

  // Downmix capture to mono into capMono.
  float pkCap = 0.0f;
  for (uint32_t i = 0; i < capN; i++)
  {
    float l = 0.0f;
    float r = 0.0f;
    if (inSplitPlanar)
    {
      l = *(const float *)(inBase0 + (size_t)i * inStride0);
      r = *(const float *)(inBase1 + (size_t)i * inStride0);
    }
    else
    {
      const uint8_t *frame = inBase0 + (size_t)i * inStride0;
      l = *(const float *)(frame + 0);
      r = *(const float *)(frame + sizeof(float));
    }
    const float m = 0.5f * (l + r);
    capMono[i] = m;
    const float a = std::fabs(m);
    if (a > pkCap)
      pkCap = a;
  }
  for (uint32_t i = capN; i < nframes; i++)
    capMono[i] = 0.0f;

  capFramesAvail.store(capN, std::memory_order_release);
  lastCaptureNFrames.store(capN, std::memory_order_relaxed);
  captureFramesTotal.fetch_add(capN, std::memory_order_relaxed);
  captureCbTotal.fetch_add(1, std::memory_order_relaxed);
  updatePeak(peakCaptureRaw, pkCap);
  if (pkCap > 0.000001f)
    nonZeroInCalls.fetch_add(1, std::memory_order_relaxed);

  lastPlaybackNFrames.store(nframes, std::memory_order_relaxed);
  playbackFramesTotal.fetch_add(nframes, std::memory_order_relaxed);

  // Stream time (best-effort).
  {
    struct pw_time t;
    if (pw_stream_get_time_n(duplex_stream, &t, sizeof(t)) == 0)
    {
      lastPbTimeNow.store((int64_t)t.now, std::memory_order_relaxed);
      lastPbTimeRate.store((int64_t)t.rate.denom, std::memory_order_relaxed);
      lastPbTimeDelay.store((int64_t)t.delay, std::memory_order_relaxed);
      lastCapTimeNow.store((int64_t)t.now, std::memory_order_relaxed);
      lastCapTimeRate.store((int64_t)t.rate.denom, std::memory_order_relaxed);
      lastCapTimeDelay.store((int64_t)t.delay, std::memory_order_relaxed);
    }
  }

  // Ensure DSP buffers sized.
  if (namIn.size() < nframes)
    namIn.resize(nframes);
  if (namOut.size() < nframes)
    namOut.resize(nframes);
  if (irBlockOut.size() < nframes)
    irBlockOut.resize(nframes);

  // Copy capture mono into NAM input buffer.
  for (uint32_t i = 0; i < nframes; i++)
    namIn[i] = capMono[i];

  const float g = inputTrimLin.load(std::memory_order_relaxed);

  // DI-only: raw capture straight to output (with safety DC block + limiter).
  if (diOnlyMode.load(std::memory_order_relaxed))
  {
    const float lim = clampf(safetyLimiterAbs.load(std::memory_order_relaxed), 0.01f, 0.99f);
    const float gGate = gateForBypass();

    if (outSplitPlanar)
    {
      if (!d_out1->data || !d_out1->chunk)
      {
        pw_stream_queue_buffer(duplex_stream, buf);
        return;
      }
      float *outL = (float *)((uint8_t *)d_out0->data + d_out0->chunk->offset);
      float *outR = (float *)((uint8_t *)d_out1->data + d_out1->chunk->offset);
      for (uint32_t i = 0; i < nframes; i++)
      {
        float s = (namIn[i] * g) * gGate;
        s = dcBlock(s);
        s = clampf(s, -lim, lim);
        outL[i] = s;
        outR[i] = s;
      }
      d_out0->chunk->size = nframes * sizeof(float);
      d_out1->chunk->size = nframes * sizeof(float);
      d_out0->chunk->stride = sizeof(float);
      d_out1->chunk->stride = sizeof(float);
    }
    else
    {
      const uint32_t outStride = outStride0;
      uint8_t *outBase0 = (uint8_t *)d_out0->data + d_out0->chunk->offset;
      for (uint32_t i = 0; i < nframes; i++)
      {
        float s = (namIn[i] * g) * gGate;
        s = dcBlock(s);
        s = clampf(s, -lim, lim);
        *(float *)(outBase0 + (size_t)i * outStride) = s;
        *(float *)(outBase0 + (size_t)i * outStride + sizeof(float)) = s;
      }
      d_out0->chunk->size = nframes * outStride;
      if (d_out0->chunk->stride == 0)
        d_out0->chunk->stride = outStride;
    }

    float pk = 0.0f;
    for (uint32_t i = 0; i < nframes; i++)
    {
      float a = std::fabs(namIn[i] * g);
      if (a > pk)
        pk = a;
    }
    updatePeak(peakInput, pk);
    updatePeak(peakFinalOut, pk);

    pw_stream_queue_buffer(duplex_stream, buf);
    return;
  }

  // Passthrough: capture -> playback (safety-processed).
  if (passthroughMode.load(std::memory_order_relaxed))
  {
    const float lim = clampf(safetyLimiterAbs.load(std::memory_order_relaxed), 0.01f, 0.99f);
    const float gGate = gateForBypass();

    if (outSplitPlanar)
    {
      if (!d_out1->data || !d_out1->chunk)
      {
        pw_stream_queue_buffer(duplex_stream, buf);
        return;
      }
      float *outL = (float *)((uint8_t *)d_out0->data + d_out0->chunk->offset);
      float *outR = (float *)((uint8_t *)d_out1->data + d_out1->chunk->offset);
      for (uint32_t i = 0; i < nframes; i++)
      {
        float s = (namIn[i] * g) * gGate;
        s = dcBlock(s);
        s = clampf(s, -lim, lim);
        outL[i] = s;
        outR[i] = s;
      }
      d_out0->chunk->size = nframes * sizeof(float);
      d_out1->chunk->size = nframes * sizeof(float);
      d_out0->chunk->stride = sizeof(float);
      d_out1->chunk->stride = sizeof(float);
    }
    else
    {
      const uint32_t outStride = outStride0;
      uint8_t *outBase0 = (uint8_t *)d_out0->data + d_out0->chunk->offset;
      for (uint32_t i = 0; i < nframes; i++)
      {
        float s = (namIn[i] * g) * gGate;
        s = dcBlock(s);
        s = clampf(s, -lim, lim);
        *(float *)(outBase0 + (size_t)i * outStride) = s;
        *(float *)(outBase0 + (size_t)i * outStride + sizeof(float)) = s;
      }
      d_out0->chunk->size = nframes * outStride;
      if (d_out0->chunk->stride == 0)
        d_out0->chunk->stride = outStride;
    }

    float pkOut = 0.0f;
    for (uint32_t i = 0; i < nframes; i++)
    {
      float a = std::fabs(namIn[i] * g);
      if (a > pkOut)
        pkOut = a;
    }
    updatePeak(peakFinalOut, pkOut);

    pw_stream_queue_buffer(duplex_stream, buf);
    return;
  }

  // --- Main DSP chain (copied from the prior playback callback) ---

  // --- Noise gate (idle hiss killer) ---
  const float gateOpenTh = 0.0010f;  // ~ -60 dBFS
  const float gateCloseTh = 0.0003f; // ~ -70 dBFS
  const float envAtk = 0.04f;        // envelope attack smoothing
  const float envRel = 0.002f;       // envelope release smoothing
  const float gainAtk = 0.15f;       // gate open ramp
  const float gainRel = 0.01f;       // gate close ramp

  if (gateEnabled.load(std::memory_order_relaxed))
  {
    float inPk = peakCaptureRaw.load(std::memory_order_relaxed);
    if (inPk > gateEnv)
      gateEnv = gateEnv + (inPk - gateEnv) * envAtk;
    else
      gateEnv = gateEnv + (inPk - gateEnv) * envRel;

    if (!gateOpen)
    {
      if (gateEnv >= gateOpenTh)
        gateOpen = true;
    }
    else
    {
      if (gateEnv <= gateCloseTh)
        gateOpen = false;
    }

    const float target = gateOpen ? 1.0f : 0.0f;
    const float k = gateOpen ? gainAtk : gainRel;
    gateGain = gateGain + (target - gateGain) * k;
  }
  else
  {
    gateGain = 1.0f;
    gateOpen = true;
  }

  if (peakCaptureRaw.load(std::memory_order_relaxed) < 0.0000005f)
  {
    gateOpen = false;
    gateGain = 0.0f;
  }

  float pkIn = 0.0f;
  for (uint32_t i = 0; i < nframes; i++)
  {
    float a = std::fabs(namIn[i]);
    if (a > pkIn)
      pkIn = a;
  }
  updatePeak(peakInput, pkIn);

  const float inLim = 0.90f;
  float namLevelScale = 1.0f;
  if (namUseInputLevel.load(std::memory_order_relaxed) && namHasInputLevel.load(std::memory_order_relaxed))
  {
    constexpr float refDbu = 12.2f;
    const float modelDbu = namInputLevelDbu.load(std::memory_order_relaxed);
    namLevelScale = std::pow(10.0f, (refDbu - modelDbu) / 20.0f);
  }
  for (uint32_t i = 0; i < nframes; i++)
  {
    float x = namIn[i] * (g * gateGain) * namLevelScale * namPreGainLin.load(std::memory_order_relaxed);
    if (x > inLim)
      x = inLim;
    else if (x < -inLim)
      x = -inLim;
    namIn[i] = x;
  }

  float pkNamIn = 0.0f;
  for (uint32_t i = 0; i < nframes; i++)
  {
    float a = std::fabs(namIn[i]);
    if (a > pkNamIn)
      pkNamIn = a;
  }
  updatePeak(peakNamIn, pkNamIn);

  dumpNamPushIn(namIn.data(), nframes);

  const bool namOk = !bypassNam.load(std::memory_order_relaxed) &&
                     modelReady.load(std::memory_order_acquire) &&
                     (gModel != nullptr);
  if (namOk)
  {
    try
    {
      const bool os2x = namOversample2x.load(std::memory_order_relaxed);
      if (os2x)
      {
        const uint32_t osFrames = nframes * 2;
        static thread_local std::vector<float> osIn;
        static thread_local std::vector<float> osOut;
        osIn.resize(osFrames);
        osOut.resize(osFrames);
        for (uint32_t i = 0; i < nframes; i++)
        {
          const float x0 = namIn[i];
          const float x1 = namIn[(i + 1u < nframes) ? (i + 1u) : i];
          osIn[2u * i] = x0;
          osIn[2u * i + 1u] = 0.5f * (x0 + x1);
        }

        const uint32_t wantBlock = osFrames;
        if (gNamBlockSize != wantBlock)
        {
          if (!namDisableRuntimeReset.load(std::memory_order_relaxed))
          {
            gModel->ResetAndPrewarm((double)sampleRate * 2.0, (int)wantBlock);
            namRuntimeResets.fetch_add(1, std::memory_order_relaxed);
          }
          gNamBlockSize = wantBlock;
        }
        gModel->process(osIn.data(), osOut.data(), (int)osFrames);

        {
          static thread_local float z1 = 0.0f;
          static thread_local float z2 = 0.0f;
          const float fc = namOs2xLpfHz.load(std::memory_order_relaxed);
          const float fs = (float)sampleRate * 2.0f;
          const float w0 = 2.0f * 3.1415926f * (clampf(fc, 1000.0f, fs * 0.45f) / fs);
          const float cosw0 = std::cos(w0);
          const float sinw0 = std::sin(w0);
          constexpr float Q = 0.7071067f;
          const float alpha = sinw0 / (2.0f * Q);

          float b0 = (1.0f - cosw0) * 0.5f;
          float b1 = 1.0f - cosw0;
          float b2 = (1.0f - cosw0) * 0.5f;
          float a0 = 1.0f + alpha;
          float a1 = -2.0f * cosw0;
          float a2 = 1.0f - alpha;
          b0 /= a0;
          b1 /= a0;
          b2 /= a0;
          a1 /= a0;
          a2 /= a0;

          for (uint32_t i = 0; i < osFrames; i++)
          {
            const float x = osOut[i];
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            osOut[i] = y;
          }
        }

        for (uint32_t i = 0; i < nframes; i++)
          namOut[i] = osOut[2u * i];
      }
      else
      {
        const uint32_t wantBlock = nframes;
        if (gNamBlockSize != wantBlock)
        {
          if (!namDisableRuntimeReset.load(std::memory_order_relaxed))
          {
            gModel->ResetAndPrewarm((double)sampleRate, (int)wantBlock);
            namRuntimeResets.fetch_add(1, std::memory_order_relaxed);
          }
          gNamBlockSize = wantBlock;
        }
        gModel->process(namIn.data(), namOut.data(), (int)nframes);
      }
    }
    catch (...)
    {
      std::memcpy(namOut.data(), namIn.data(), sizeof(float) * nframes);
    }
  }
  else
  {
    std::memcpy(namOut.data(), namIn.data(), sizeof(float) * nframes);
  }

  if (namForceBypassOutput.load(std::memory_order_relaxed))
    std::memset(namOut.data(), 0, sizeof(float) * nframes);

  dumpNamPushOut(namOut.data(), nframes);

  if (namPostLpfEnable.load(std::memory_order_relaxed))
  {
    const float hz = namPostLpfHz.load(std::memory_order_relaxed);
    const float a = clampf(2.0f * 3.1415926f * hz / (float)sampleRate, 0.0001f, 0.99f);
    float y1 = namPostLpf_y1;
    for (uint32_t i = 0; i < nframes; i++)
    {
      y1 = y1 + a * (namOut[i] - y1);
      namOut[i] = y1;
    }
    namPostLpf_y1 = y1;
  }

  float pkNam = 0.0f;
  for (uint32_t i = 0; i < nframes; i++)
  {
    float a = std::fabs(namOut[i]);
    if (a > pkNam)
      pkNam = a;
  }
  updatePeak(peakNamOut, pkNam);

  static std::atomic<bool> irNeedsInit{true};
  const bool refMode = namReferenceMode.load(std::memory_order_relaxed);
  const bool wantIR = (!refMode) && irReady.load(std::memory_order_acquire);
  if (wantIR && (irNeedsInit.load(std::memory_order_relaxed) || (int)nframes != (int)bufferSize))
  {
    bufferSize = nframes;
    if (!gIrCached.empty())
    {
      if (gIR.init(gIrCached, (int)bufferSize))
      {
        irNeedsInit.store(false, std::memory_order_relaxed);
        std::printf("IR: re-init at blockSize=%u\n", (unsigned)bufferSize);
        std::fflush(stdout);
      }
    }
  }

  const bool irOk = wantIR && gIR.ready() && !bypassIr.load(std::memory_order_relaxed);
  const float *finalBuf = namOut.data();
  if (irOk)
  {
    bool success = gIR.processBlock(namOut.data(), irBlockOut.data(), (int)nframes);
    if (success)
      finalBuf = irBlockOut.data();
  }

  const bool wetMute = namWetMute.load(std::memory_order_relaxed);
  const bool diMon = namDiMonitor.load(std::memory_order_relaxed);
  static thread_local std::vector<float> mixBuf;
  const float *outBufFinal = finalBuf;
  if (wetMute || diMon)
  {
    mixBuf.resize(nframes);
    constexpr float diGain = 0.35f;
    if (wetMute)
    {
      for (uint32_t i = 0; i < nframes; i++)
        mixBuf[i] = capMono[i] * diGain;
    }
    else
    {
      for (uint32_t i = 0; i < nframes; i++)
        mixBuf[i] = finalBuf[i] + capMono[i] * diGain;
    }
    outBufFinal = mixBuf.data();
  }

  float pkIr = 0.0f;
  float pkOut = 0.0f;

  const bool hardMute = gateEnabled.load(std::memory_order_relaxed) && (gateGain < 0.0001f);
  const bool absMute = forceOutputZero.load(std::memory_order_relaxed);
  const float lim = clampf(safetyLimiterAbs.load(std::memory_order_relaxed), 0.01f, 0.99f);

  if (outSplitPlanar)
  {
    if (!d_out1->data || !d_out1->chunk)
    {
      pw_stream_queue_buffer(duplex_stream, buf);
      return;
    }
    float *outL = (float *)((uint8_t *)d_out0->data + d_out0->chunk->offset);
    float *outR = (float *)((uint8_t *)d_out1->data + d_out1->chunk->offset);
    for (uint32_t i = 0; i < nframes; i++)
    {
      float s = (hardMute || absMute) ? 0.0f : outBufFinal[i];
      s = dcBlock(s);
      s = clampf(s, -lim, lim);
      outL[i] = s;
      outR[i] = s;
      float a = std::fabs(s);
      if (a > pkOut)
        pkOut = a;
    }
    d_out0->chunk->size = nframes * sizeof(float);
    d_out1->chunk->size = nframes * sizeof(float);
    d_out0->chunk->stride = sizeof(float);
    d_out1->chunk->stride = sizeof(float);
  }
  else
  {
    const uint32_t outStride = outStride0;
    uint8_t *outBase0 = (uint8_t *)d_out0->data + d_out0->chunk->offset;
    for (uint32_t i = 0; i < nframes; i++)
    {
      float s = (hardMute || absMute) ? 0.0f : outBufFinal[i];
      s = dcBlock(s);
      s = clampf(s, -lim, lim);
      *(float *)(outBase0 + (size_t)i * outStride) = s;
      *(float *)(outBase0 + (size_t)i * outStride + sizeof(float)) = s;
      float a = std::fabs(s);
      if (a > pkOut)
        pkOut = a;
    }
    d_out0->chunk->size = nframes * outStride;
    if (d_out0->chunk->stride == 0)
      d_out0->chunk->stride = outStride;
  }

  if (irOk && outBufFinal == irBlockOut.data())
  {
    for (uint32_t i = 0; i < nframes; i++)
    {
      float a = std::fabs(outBufFinal[i]);
      if (a > pkIr)
        pkIr = a;
    }
  }
  updatePeak(peakIrOut, pkIr);
  updatePeak(peakFinalOut, pkOut);

  pw_stream_queue_buffer(duplex_stream, buf);
}

static void on_capture_process(void *userdata)
{
  (void)userdata;

  const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  lastCaptureCbMs.store(nowMs, std::memory_order_relaxed);

  captureProcessCalls.fetch_add(1, std::memory_order_relaxed);

  if (!capture_stream)
  {
    capEarlyNoStream.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  struct pw_buffer *inBuf = pw_stream_dequeue_buffer(capture_stream);
  if (!inBuf)
  {
    earlyNoCaptureBuf.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  struct spa_buffer *inSpa = inBuf->buffer;
  if (!inSpa || inSpa->n_datas < 1)
  {
    capEarlyNoSpa.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(capture_stream, inBuf);
    return;
  }

  const uint32_t ch = gCaptureChannels.load(std::memory_order_relaxed);
  bool splitPlanar = (ch > 1 && inSpa->n_datas >= 2);
  if (forceCapturePlanar.load(std::memory_order_relaxed))
    splitPlanar = true;
  if (forceCaptureInterleaved.load(std::memory_order_relaxed))
    splitPlanar = false;
  struct spa_data *d_in0 = &inSpa->datas[0];
  // If we force planar but PipeWire only provided one plane, fall back safely.
  // (This is expected on many graphs: n_datas==1 for interleaved stereo.)
  if (splitPlanar && inSpa->n_datas < 2)
  {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
    {
      std::fprintf(stderr,
                   "[CAPTURE] warning: requested planar capture but only n_datas=%u; falling back to interleaved\n",
                   (unsigned)inSpa->n_datas);
      std::fflush(stderr);
    }
    splitPlanar = false;
  }
  struct spa_data *d_in1 = splitPlanar ? &inSpa->datas[1] : &inSpa->datas[0];
  if (!d_in0->data || !d_in0->chunk)
  {
    capEarlyNoDataOrChunk.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(capture_stream, inBuf);
    return;
  }

  // Stride semantics:
  //  - planar: each plane is 1 channel => stride is usually sizeof(float)
  //  - interleaved: one plane contains frames => stride is frame size (2ch => 2*sizeof(float))
  const uint32_t defaultStride = (ch == 1)
                                     ? (uint32_t)sizeof(float)
                                     : (splitPlanar ? (uint32_t)sizeof(float) : (uint32_t)(2 * sizeof(float)));
  const uint32_t inStride0 = (d_in0->chunk->stride != 0) ? (uint32_t)d_in0->chunk->stride : defaultStride;
  if (inStride0 < defaultStride)
  {
    capEarlyBadStride.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(capture_stream, inBuf);
    return;
  }

  uint8_t *inBase0 = (uint8_t *)d_in0->data + d_in0->chunk->offset;
  uint8_t *inBase1 = (uint8_t *)d_in1->data + d_in1->chunk->offset;

  // Compute nframes defensively.
  const uint32_t frameBytes = (ch == 1)
                                  ? (uint32_t)sizeof(float)
                                  : (splitPlanar ? (uint32_t)sizeof(float) : (uint32_t)(2 * sizeof(float)));
  uint32_t nframes = (uint32_t)(d_in0->chunk->size / frameBytes);
  if (nframes == 0)
    nframes = requestedQuantum;
  if (nframes == 0 || nframes > kMaxQuantum)
  {
    capEarlyBadFrames.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(capture_stream, inBuf);
    return;
  }

  ensureCapBuffer(nframes);

  captureFramesTotal.fetch_add(nframes, std::memory_order_relaxed);
  captureCbTotal.fetch_add(1, std::memory_order_relaxed);
  lastCaptureNFrames.store(nframes, std::memory_order_relaxed);

  // Stream time (best-effort; safe to fail).
  if (capture_stream)
  {
    struct pw_time t;
    if (pw_stream_get_time_n(capture_stream, &t, sizeof(t)) == 0)
    {
      lastCapTimeNow.store((int64_t)t.now, std::memory_order_relaxed);
      lastCapTimeRate.store((int64_t)t.rate.denom, std::memory_order_relaxed);
      lastCapTimeDelay.store((int64_t)t.delay, std::memory_order_relaxed);
    }
  }

  float pkCap = 0.0f;
  float minS = 0.0f;
  float maxS = 0.0f;
  bool haveMinMax = false;
  float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
  for (uint32_t i = 0; i < nframes; i++)
  {
    float l = 0.0f;
    float r = 0.0f;
    if (ch == 1)
    {
      if (splitPlanar)
      {
        l = *(const float *)(inBase0 + (size_t)i * inStride0);
      }
      else
      {
        const uint8_t *frame = inBase0 + (size_t)i * inStride0;
        l = *(const float *)(frame + 0);
      }
      r = l;
    }
    else if (splitPlanar)
    {
      l = *(const float *)(inBase0 + (size_t)i * inStride0);
      r = *(const float *)(inBase1 + (size_t)i * inStride0);
    }
    else
    {
      const uint8_t *frame = inBase0 + (size_t)i * inStride0;
      l = *(const float *)(frame + 0);
      r = *(const float *)(frame + sizeof(float));
    }
    float m = 0.5f * (l + r);
    capMono[i] = m;
    if (!haveMinMax)
    {
      minS = maxS = m;
      haveMinMax = true;
    }
    else
    {
      if (m < minS)
        minS = m;
      if (m > maxS)
        maxS = m;
    }
    float a = std::fabs(m);
    if (a > pkCap)
      pkCap = a;

    if (i == 0)
      s0 = m;
    else if (i == 1)
      s1 = m;
    else if (i == 2)
      s2 = m;
  }

  capFramesAvail.store(nframes, std::memory_order_release);

  updatePeak(peakCaptureRaw, pkCap);
  if (pkCap > 0.000001f)
    nonZeroInCalls.fetch_add(1, std::memory_order_relaxed);

  // Rate-limited capture probe: print once, and then at most once every ~2 seconds.
  // This tells us definitively whether the buffers contain non-zero samples.
  static uint64_t lastMs = 0;
  if (captureDebugPrint.load(std::memory_order_relaxed) && (lastMs == 0 || nowMs - lastMs > 2000))
  {
    lastMs = nowMs;
    const uint32_t rawStride = (d_in0->chunk->stride != 0) ? (uint32_t)d_in0->chunk->stride : 0u;
    std::printf(
        "[CAPDBG] n_datas=%u layout=%s size=%u off=%u rawStride=%u stride=%u frameBytes=%u nframes=%u pk=%g min=%g max=%g s0=%g s1=%g s2=%g\n",
        (unsigned)inSpa->n_datas,
        splitPlanar ? "planar" : "interleaved",
        (unsigned)d_in0->chunk->size,
        (unsigned)d_in0->chunk->offset,
        (unsigned)rawStride,
        (unsigned)inStride0,
        (unsigned)frameBytes,
        (unsigned)nframes,
        (double)pkCap,
        (double)minS,
        (double)maxS,
        (double)s0,
        (double)s1,
        (double)s2);
    std::fflush(stdout);
  }

  pw_stream_queue_buffer(capture_stream, inBuf);
}

// -------------------- PipeWire Callback (capture + playback) --------------------
// NOTE: legacy (2-stream) callback retained for reference but not used in v1 duplex.
static void on_process(void *userdata)
{
  const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  lastPlaybackCbMs.store(nowMs, std::memory_order_relaxed);

  processCalls.fetch_add(1, std::memory_order_relaxed);
  const uint64_t dc = debugCalls.fetch_add(1, std::memory_order_relaxed);

  (void)dc;

  if (!playback_stream)
  {
    earlyNoBuf.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Dequeue playback buffer first: if capture isn't ready, we still return silence.
  struct pw_buffer *outBuf = pw_stream_dequeue_buffer(playback_stream);
  if (!outBuf)
  {
    earlyNoBuf.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  struct spa_buffer *outSpa = outBuf->buffer;
  if (!outSpa || outSpa->n_datas < 1)
  {
    earlyNoBuf.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(playback_stream, outBuf);
    return;
  }

  // Determine output quantum.
  struct spa_data *d_out0 = &outSpa->datas[0];
  if (!d_out0->data || !d_out0->chunk)
  {
    earlyMissingPtrs.fetch_add(1, std::memory_order_relaxed);
    pw_stream_queue_buffer(playback_stream, outBuf);
    return;
  }

  const bool splitPlanar = (outSpa->n_datas >= 2);
  struct spa_data *d_out1 = splitPlanar ? &outSpa->datas[1] : &outSpa->datas[0];

  const uint32_t outStride0 = (d_out0->chunk->stride != 0) ? (uint32_t)d_out0->chunk->stride : (uint32_t)(2 * sizeof(float));
  uint32_t nframes = (outStride0 >= sizeof(float)) ? (uint32_t)(d_out0->chunk->size / outStride0) : 0u;
  if (nframes == 0 || nframes > kMaxQuantum)
    nframes = requestedQuantum;
  if (nframes == 0 || nframes > kMaxQuantum)
    nframes = 128;

  playbackFramesTotal.fetch_add(nframes, std::memory_order_relaxed);
  playbackCbTotal.fetch_add(1, std::memory_order_relaxed);
  lastPlaybackNFrames.store(nframes, std::memory_order_relaxed);

  // Stream time (best-effort; safe to fail).
  if (playback_stream)
  {
    struct pw_time t;
    if (pw_stream_get_time_n(playback_stream, &t, sizeof(t)) == 0)
    {
      lastPbTimeNow.store((int64_t)t.now, std::memory_order_relaxed);
      lastPbTimeRate.store((int64_t)t.rate.denom, std::memory_order_relaxed);
      lastPbTimeDelay.store((int64_t)t.delay, std::memory_order_relaxed);
    }
  }

  // Grab the latest capture block.
  uint32_t capN = capFramesAvail.load(std::memory_order_acquire);
  if (capN == 0)
  {
    clearPlayback(outBuf);
    pw_stream_queue_buffer(playback_stream, outBuf);
    return;
  }
  if (capN > nframes)
    capN = nframes;

  // Ensure buffers sized (this should stabilize after the first callback).
  if (namIn.size() < nframes)
    namIn.resize(nframes);
  if (namOut.size() < nframes)
    namOut.resize(nframes);
  if (irBlockOut.size() < nframes)
    irBlockOut.resize(nframes);

  // Copy capture mono into NAM input buffer. (Rest of DSP chain expects mono.)
  for (uint32_t i = 0; i < capN; i++)
    namIn[i] = capMono[i];
  for (uint32_t i = capN; i < nframes; i++)
    namIn[i] = 0.0f;

  const float g = inputTrimLin.load(std::memory_order_relaxed);

  // DI-only: raw capture straight to output (with safety DC block + limiter).
  if (diOnlyMode.load(std::memory_order_relaxed))
  {
    const float lim = clampf(safetyLimiterAbs.load(std::memory_order_relaxed), 0.01f, 0.99f);
    const float gGate = gateForBypass();

    if (splitPlanar)
    {
      if (!d_out1->data || !d_out1->chunk)
      {
        pw_stream_queue_buffer(playback_stream, outBuf);
        return;
      }

      float *outL = (float *)((uint8_t *)d_out0->data + d_out0->chunk->offset);
      float *outR = (float *)((uint8_t *)d_out1->data + d_out1->chunk->offset);
      for (uint32_t i = 0; i < nframes; i++)
      {
        float s = (namIn[i] * g) * gGate;
        s = dcBlock(s);
        s = clampf(s, -lim, lim);
        outL[i] = s;
        outR[i] = s;
      }
      d_out0->chunk->size = nframes * sizeof(float);
      d_out1->chunk->size = nframes * sizeof(float);
      d_out0->chunk->stride = sizeof(float);
      d_out1->chunk->stride = sizeof(float);
    }
    else
    {
      const uint32_t outStride = (d_out0->chunk->stride != 0) ? (uint32_t)d_out0->chunk->stride : (uint32_t)(2 * sizeof(float));
      uint8_t *outBase0 = (uint8_t *)d_out0->data + d_out0->chunk->offset;
      for (uint32_t i = 0; i < nframes; i++)
      {
        float s = (namIn[i] * g) * gGate;
        s = dcBlock(s);
        s = clampf(s, -lim, lim);
        *(float *)(outBase0 + (size_t)i * outStride) = s;
        *(float *)(outBase0 + (size_t)i * outStride + sizeof(float)) = s;
      }
      d_out0->chunk->size = nframes * outStride;
      if (d_out0->chunk->stride == 0)
        d_out0->chunk->stride = outStride;
    }

    // Simple meters.
    float pk = 0.0f;
    for (uint32_t i = 0; i < nframes; i++)
    {
      float a = std::fabs(namIn[i] * g);
      if (a > pk)
        pk = a;
    }
    updatePeak(peakInput, pk);
    updatePeak(peakFinalOut, pk);

    pw_stream_queue_buffer(playback_stream, outBuf);
    return;
  }

  // Passthrough: capture -> playback (safety-processed).
  if (passthroughMode.load(std::memory_order_relaxed))
  {
    const float lim = clampf(safetyLimiterAbs.load(std::memory_order_relaxed), 0.01f, 0.99f);
    const float gGate = gateForBypass();

    if (splitPlanar)
    {
      if (!d_out1->data || !d_out1->chunk)
      {
        pw_stream_queue_buffer(playback_stream, outBuf);
        return;
      }

      float *outL = (float *)((uint8_t *)d_out0->data + d_out0->chunk->offset);
      float *outR = (float *)((uint8_t *)d_out1->data + d_out1->chunk->offset);

      for (uint32_t i = 0; i < nframes; i++)
      {
        float s = (namIn[i] * g) * gGate;
        s = dcBlock(s);
        s = clampf(s, -lim, lim);
        outL[i] = s;
        outR[i] = s;
      }
      d_out0->chunk->size = nframes * sizeof(float);
      d_out1->chunk->size = nframes * sizeof(float);
      d_out0->chunk->stride = sizeof(float);
      d_out1->chunk->stride = sizeof(float);
    }
    else
    {
      const uint32_t outStride = (d_out0->chunk->stride != 0) ? (uint32_t)d_out0->chunk->stride : (uint32_t)(2 * sizeof(float));
      uint8_t *outBase0 = (uint8_t *)d_out0->data + d_out0->chunk->offset;
      for (uint32_t i = 0; i < nframes; i++)
      {
        float s = (namIn[i] * g) * gGate;
        s = dcBlock(s);
        s = clampf(s, -lim, lim);
        *(float *)(outBase0 + (size_t)i * outStride) = s;
        *(float *)(outBase0 + (size_t)i * outStride + sizeof(float)) = s;
      }
      d_out0->chunk->size = nframes * outStride;
      if (d_out0->chunk->stride == 0)
        d_out0->chunk->stride = outStride;
    }

    // Meter output in passthrough.
    float pkOut = 0.0f;
    for (uint32_t i = 0; i < nframes; i++)
    {
      float a = std::fabs(namIn[i] * g);
      if (a > pkOut)
        pkOut = a;
    }
    // In passthrough, input peak already tracked by the capture callback.
    updatePeak(peakFinalOut, pkOut);
    pw_stream_queue_buffer(playback_stream, outBuf);
    return;
  }

  // --- Noise gate (idle hiss killer) ---
  // Thresholds in linear amplitude. Defaults tuned for guitar interface noise.
  const float gateOpenTh = 0.0010f;  // ~ -60 dBFS
  const float gateCloseTh = 0.0003f; // ~ -70 dBFS
  const float envAtk = 0.04f;        // envelope attack smoothing
  const float envRel = 0.002f;       // envelope release smoothing
  const float gainAtk = 0.15f;       // gate open ramp
  const float gainRel = 0.01f;       // gate close ramp

  if (gateEnabled.load(std::memory_order_relaxed))
  {
    // Update envelope from pre-trim input peak.
    float inPk = peakCaptureRaw.load(std::memory_order_relaxed);
    if (inPk > gateEnv)
      gateEnv = gateEnv + (inPk - gateEnv) * envAtk;
    else
      gateEnv = gateEnv + (inPk - gateEnv) * envRel;

    if (!gateOpen)
    {
      if (gateEnv >= gateOpenTh)
        gateOpen = true;
    }
    else
    {
      if (gateEnv <= gateCloseTh)
        gateOpen = false;
    }

    const float target = gateOpen ? 1.0f : 0.0f;
    const float k = gateOpen ? gainAtk : gainRel;
    gateGain = gateGain + (target - gateGain) * k;
  }
  else
  {
    gateGain = 1.0f;
    gateOpen = true;
  }

  // If the input is truly silent, bias the gate strongly toward closed.
  // This avoids the DAC/amp revealing tiny model/IR self-noise at idle.
  if (peakCaptureRaw.load(std::memory_order_relaxed) < 0.0000005f)
  {
    gateOpen = false;
    gateGain = 0.0f;
  }

  // Meter input peak (what we're actually feeding the DSP)
  float pkIn = 0.0f;
  for (uint32_t i = 0; i < nframes; i++)
  {
    float a = std::fabs(namIn[i]);
    if (a > pkIn)
      pkIn = a;
  }
  updatePeak(peakInput, pkIn);

  // Input trim + gate + conservative limiting.
  // The NAM model typically expects a reasonably clean, un-clipped signal.
  // Pre-driving it with a tanh here can create harsh intermodulation/buzz.
  const float inLim = 0.90f;

  // Optional: normalize into the NAM model's expected input level.
  // If user overrides nam_input_level_dbu, it takes precedence over model metadata.
  float namLevelScale = 1.0f;
  if (namUseInputLevel.load(std::memory_order_relaxed) && namHasInputLevel.load(std::memory_order_relaxed))
  {
    // Reference: many guitar interfaces are roughly around +12.2 dBu @ 0 dBFS sine.
    // If the model expects a larger number (e.g. +20 dBu), we should scale DOWN.
    // scale = 10^((ref - model)/20)
    constexpr float refDbu = 12.2f;
    const float modelDbu = namInputLevelDbu.load(std::memory_order_relaxed);
    namLevelScale = std::pow(10.0f, (refDbu - modelDbu) / 20.0f);
  }
  for (uint32_t i = 0; i < nframes; i++)
  {
    float x = namIn[i] * (g * gateGain) * namLevelScale * namPreGainLin.load(std::memory_order_relaxed);
    if (x > inLim)
      x = inLim;
    else if (x < -inLim)
      x = -inLim;
    namIn[i] = x;
  }

  // Meter NAM input peak after all scaling/limiting.
  float pkNamIn = 0.0f;
  for (uint32_t i = 0; i < nframes; i++)
  {
    float a = std::fabs(namIn[i]);
    if (a > pkNamIn)
      pkNamIn = a;
  }
  updatePeak(peakNamIn, pkNamIn);

  // Optional dump: capture exactly what we're feeding into NAM.
  dumpNamPushIn(namIn.data(), nframes);

  // NAM
  // If bypassNam is enabled, we skip the model but keep the rest of the chain.
  const bool namOk = !bypassNam.load(std::memory_order_relaxed) &&
                     modelReady.load(std::memory_order_acquire) &&
                     (gModel != nullptr);
  if (namOk)
  {
    try
    {
      const bool os2x = namOversample2x.load(std::memory_order_relaxed);
      if (os2x)
      {
        // 2x oversampling: simple linear upsample/downsample.
        // This can dramatically reduce aliasing-related "buzz" in nonlinear models.
        const uint32_t osFrames = nframes * 2;
        static thread_local std::vector<float> osIn;
        static thread_local std::vector<float> osOut;
        osIn.resize(osFrames);
        osOut.resize(osFrames);

        // Linear upsample: insert midpoint samples.
        // osIn[2*i] = x[i]
        // osIn[2*i+1] = 0.5*(x[i] + x[i+1])
        for (uint32_t i = 0; i < nframes; i++)
        {
          const float x0 = namIn[i];
          const float x1 = namIn[(i + 1u < nframes) ? (i + 1u) : i];
          osIn[2u * i] = x0;
          osIn[2u * i + 1u] = 0.5f * (x0 + x1);
        }

        // Keep model in sync with both quantum and oversampled sample rate.
        const uint32_t wantBlock = osFrames;
        if (gNamBlockSize != wantBlock)
        {
          if (!namDisableRuntimeReset.load(std::memory_order_relaxed))
          {
            gModel->ResetAndPrewarm((double)sampleRate * 2.0, (int)wantBlock);
            namRuntimeResets.fetch_add(1, std::memory_order_relaxed);
          }
          gNamBlockSize = wantBlock;
        }
        gModel->process(osIn.data(), osOut.data(), (int)osFrames);

        // Anti-alias lowpass before downsampling.
        // We use a light biquad LPF (RBJ cookbook). This is the key missing piece.
        {
          static thread_local float z1 = 0.0f;
          static thread_local float z2 = 0.0f;
          const float fc = namOs2xLpfHz.load(std::memory_order_relaxed);
          const float fs = (float)sampleRate * 2.0f;
          const float w0 = 2.0f * 3.1415926f * (clampf(fc, 1000.0f, fs * 0.45f) / fs);
          const float cosw0 = std::cos(w0);
          const float sinw0 = std::sin(w0);
          constexpr float Q = 0.7071067f; // Butterworth-ish
          const float alpha = sinw0 / (2.0f * Q);

          float b0 = (1.0f - cosw0) * 0.5f;
          float b1 = 1.0f - cosw0;
          float b2 = (1.0f - cosw0) * 0.5f;
          float a0 = 1.0f + alpha;
          float a1 = -2.0f * cosw0;
          float a2 = 1.0f - alpha;
          // normalize
          b0 /= a0;
          b1 /= a0;
          b2 /= a0;
          a1 /= a0;
          a2 /= a0;

          // Transposed Direct Form II
          for (uint32_t i = 0; i < osFrames; i++)
          {
            const float x = osOut[i];
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            osOut[i] = y;
          }
        }

        // Linear downsample: pick even samples.
        for (uint32_t i = 0; i < nframes; i++)
          namOut[i] = osOut[2u * i];
      }
      else
      {
        // If PipeWire changes the quantum, keep the model's internal block size in sync.
        const uint32_t wantBlock = nframes;
        if (gNamBlockSize != wantBlock)
        {
          if (!namDisableRuntimeReset.load(std::memory_order_relaxed))
          {
            gModel->ResetAndPrewarm((double)sampleRate, (int)wantBlock);
            namRuntimeResets.fetch_add(1, std::memory_order_relaxed);
          }
          gNamBlockSize = wantBlock;
        }
        gModel->process(namIn.data(), namOut.data(), (int)nframes);
      }
    }
    catch (...)
    {
      std::memcpy(namOut.data(), namIn.data(), sizeof(float) * nframes);
    }
  }
  else
  {
    std::memcpy(namOut.data(), namIn.data(), sizeof(float) * nframes);
  }

  // Debug: if static remains when NAM output is forcibly zero, it's not from NAM.
  if (namForceBypassOutput.load(std::memory_order_relaxed))
  {
    std::memset(namOut.data(), 0, sizeof(float) * nframes);
  }

  // Optional dump: capture NAM output (after any forced bypass).
  dumpNamPushOut(namOut.data(), nframes);

  // Optional post-NAM lowpass to reduce aliasing-like fizz.
  if (namPostLpfEnable.load(std::memory_order_relaxed))
  {
    const float hz = namPostLpfHz.load(std::memory_order_relaxed);
    // 1-pole LPF alpha for y[n] = y[n-1] + alpha*(x - y[n-1])
    const float a = clampf(2.0f * 3.1415926f * hz / (float)sampleRate, 0.0001f, 0.99f);
    float y1 = namPostLpf_y1;
    for (uint32_t i = 0; i < nframes; i++)
    {
      y1 = y1 + a * (namOut[i] - y1);
      namOut[i] = y1;
    }
    namPostLpf_y1 = y1;
  }

  // Meter NAM
  float pkNam = 0.0f;
  for (uint32_t i = 0; i < nframes; i++)
  {
    float a = std::fabs(namOut[i]);
    if (a > pkNam)
      pkNam = a;
  }
  updatePeak(peakNamOut, pkNam);

  // IR: re-init to the negotiated quantum if needed.
  // This keeps the convolver block size in lockstep with PipeWire's quantum.
  static std::atomic<bool> irNeedsInit{true};
  const bool refMode = namReferenceMode.load(std::memory_order_relaxed);
  const bool wantIR = (!refMode) && irReady.load(std::memory_order_acquire);
  if (wantIR && !gIR.ready())
  {
    // Convolver isn't ready yet; we'll keep bypassing until it's initialized.
  }
  if (wantIR && (irNeedsInit.load(std::memory_order_relaxed) || (int)nframes != (int)bufferSize))
  {
    bufferSize = nframes;
    if (!gIrCached.empty())
    {
      if (gIR.init(gIrCached, (int)bufferSize))
      {
        irNeedsInit.store(false, std::memory_order_relaxed);
        std::printf("IR: re-init at blockSize=%u\n", (unsigned)bufferSize);
        std::fflush(stdout);
      }
    }
  }

  const bool irOk = wantIR && gIR.ready() && !bypassIr.load(std::memory_order_relaxed);
  const float *finalBuf = namOut.data();
  if (irOk)
  {
    bool success = gIR.processBlock(namOut.data(), irBlockOut.data(), (int)nframes);
    if (success)
      finalBuf = irBlockOut.data();
  }

  // Optional debug monitor modes.
  // - NAM-only is the default (finalBuf from NAM/IR).
  // - NAM_WET_MUTE: output DI only.
  // - NAM_DI_MONITOR: mix a little DI into the wet path.
  // NOTE: DI source is capMono (raw capture mono). We keep the mix conservative.
  const bool wetMute = namWetMute.load(std::memory_order_relaxed);
  const bool diMon = namDiMonitor.load(std::memory_order_relaxed);
  static thread_local std::vector<float> mixBuf;
  const float *outBufFinal = finalBuf;
  if (wetMute || diMon)
  {
    mixBuf.resize(nframes);
    // Conservative DI gain so we don't accidentally clip.
    constexpr float diGain = 0.35f;
    if (wetMute)
    {
      for (uint32_t i = 0; i < nframes; i++)
        mixBuf[i] = capMono[i] * diGain;
    }
    else
    {
      for (uint32_t i = 0; i < nframes; i++)
        mixBuf[i] = finalBuf[i] + capMono[i] * diGain;
    }
    outBufFinal = mixBuf.data();
  }

  float pkIr = 0.0f;
  float pkOut = 0.0f;

  // If the gate is effectively closed, hard-mute the output. This prevents
  // constant model/IR noise or denormal artifacts from reaching headphones.
  const bool hardMute = gateEnabled.load(std::memory_order_relaxed) && (gateGain < 0.0001f);
  const bool absMute = forceOutputZero.load(std::memory_order_relaxed);

  // Safety: DC block + limiter, to prevent unpredictable full-scale blasts.
  // NOTE: We apply this later when writing out so it covers both bypass and processed paths.
  const float lim = clampf(safetyLimiterAbs.load(std::memory_order_relaxed), 0.01f, 0.99f);

  if (splitPlanar)
  {
    // Planar output
    if (!d_out1->data || !d_out1->chunk)
    {
      pw_stream_queue_buffer(playback_stream, outBuf);
      return;
    }
    float *outL = (float *)((uint8_t *)d_out0->data + d_out0->chunk->offset);
    float *outR = (float *)((uint8_t *)d_out1->data + d_out1->chunk->offset);
    for (uint32_t i = 0; i < nframes; i++)
    {
      float s = (hardMute || absMute) ? 0.0f : outBufFinal[i];
      s = dcBlock(s);
      s = clampf(s, -lim, lim);
      outL[i] = s;
      outR[i] = s;
      float a = std::fabs(s);
      if (a > pkOut)
        pkOut = a;
    }
    d_out0->chunk->size = nframes * sizeof(float);
    d_out1->chunk->size = nframes * sizeof(float);
    d_out0->chunk->stride = sizeof(float);
    d_out1->chunk->stride = sizeof(float);
  }
  else
  {
    // Planar stereo output in a single buffer: [L0 R0 L1 R1 ...] with stride.
    // We assume stride is 2*sizeof(float) for stereo; if not, we still write L then R.
    const uint32_t outStride = (d_out0->chunk->stride != 0) ? (uint32_t)d_out0->chunk->stride : (uint32_t)(2 * sizeof(float));
    uint8_t *outBase0 = (uint8_t *)d_out0->data + d_out0->chunk->offset;
    for (uint32_t i = 0; i < nframes; i++)
    {
      float s = (hardMute || absMute) ? 0.0f : outBufFinal[i];
      s = dcBlock(s);
      s = clampf(s, -lim, lim);
      *(float *)(outBase0 + (size_t)i * outStride) = s;
      *(float *)(outBase0 + (size_t)i * outStride + sizeof(float)) = s;
      float a = std::fabs(s);
      if (a > pkOut)
        pkOut = a;
    }
    d_out0->chunk->size = nframes * outStride;
    if (d_out0->chunk->stride == 0)
      d_out0->chunk->stride = outStride;
  }

  if (irOk && outBufFinal == irBlockOut.data())
  {
    for (uint32_t i = 0; i < nframes; i++)
    {
      float a = std::fabs(outBufFinal[i]);
      if (a > pkIr)
        pkIr = a;
    }
  }
  updatePeak(peakIrOut, pkIr);
  updatePeak(peakFinalOut, pkOut);

  pw_stream_queue_buffer(playback_stream, outBuf);
}

static const struct pw_stream_events capture_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_capture_process,
};

static const struct pw_stream_events playback_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_process,
};

static const struct pw_stream_events duplex_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_duplex_process,
};

static void logStreamProps(struct pw_properties *p, const char *tag)
{
  if (!p)
    return;
  const char *rate = pw_properties_get(p, PW_KEY_NODE_RATE);
  const char *forceRate = pw_properties_get(p, PW_KEY_NODE_FORCE_RATE);
  const char *lat = pw_properties_get(p, PW_KEY_NODE_LATENCY);
  const char *forceQ = pw_properties_get(p, PW_KEY_NODE_FORCE_QUANTUM);
  const char *driver = pw_properties_get(p, PW_KEY_NODE_DRIVER);
  std::fprintf(stderr,
               "[PWPROP] %s node.rate=%s node.force-rate=%s node.latency=%s node.force-quantum=%s node.driver=%s\n",
               tag,
               rate ? rate : "(null)",
               forceRate ? forceRate : "(null)",
               lat ? lat : "(null)",
               forceQ ? forceQ : "(null)",
               driver ? driver : "(null)");
  std::fflush(stderr);
}

// -------------------- Main --------------------
int main(int argc, char *argv[])
{
  loadConfig();

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  pw_init(&argc, &argv);

  // Use a thread-loop so we can reliably wait for registry events (target node
  // ID resolution) before connecting streams.
  tloop = pw_thread_loop_new("dsp_engine_pw", nullptr);
  if (!tloop)
  {
    std::fprintf(stderr, "Failed to create PipeWire thread loop\n");
    return 1;
  }
  pw_thread_loop_start(tloop);

  // All PipeWire core/stream operations must happen in the thread-loop context.
  pw_thread_loop_lock(tloop);

  context = pw_context_new(pw_thread_loop_get_loop(tloop), nullptr, 0);
  if (!context)
  {
    std::fprintf(stderr, "Failed to create PipeWire context\n");
    pw_thread_loop_unlock(tloop);
    pw_thread_loop_stop(tloop);
    pw_thread_loop_destroy(tloop);
    return 1;
  }

  core = pw_context_connect(context, nullptr, 0);
  if (!core)
  {
    std::fprintf(stderr, "Failed to connect to PipeWire\n");
    pw_context_destroy(context);
    pw_thread_loop_unlock(tloop);
    pw_thread_loop_stop(tloop);
    pw_thread_loop_destroy(tloop);
    return 1;
  }

  pw_core_add_listener(core, &core_listener, &core_events, nullptr);

  // Registry is used to resolve node.name -> node id for explicit targeting.
  // This avoids policy/autoconnect ambiguities when Dummy-Driver exists.
  registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  if (registry)
    pw_registry_add_listener(registry, &registry_listener, &registry_events, nullptr);

  // Let registry events flow.
  pw_thread_loop_unlock(tloop);

  std::printf("Audio: Initializing PipeWire stream...\n");

  // Optional driver mode toggle:
  //   PW_WANT_DRIVER=1 makes the duplex node request to be the driver.
  // This is useful as a diagnostic if scheduling appears throttled (pb_x << 1.0).
  // Default: do NOT ask to be a driver; instead follow the device clock.
  const bool wantDriver = (std::getenv("PW_WANT_DRIVER") && std::atoi(std::getenv("PW_WANT_DRIVER")) != 0);
  const bool diagNoReconnect = (std::getenv("PW_DIAG_NO_RECONNECT") && std::atoi(std::getenv("PW_DIAG_NO_RECONNECT")) != 0);
  const bool useLegacyStreams = (std::getenv("PW_USE_LEGACY_STREAMS") && std::atoi(std::getenv("PW_USE_LEGACY_STREAMS")) != 0);
  const bool capturePassive = !(std::getenv("PW_CAPTURE_PASSIVE") && std::atoi(std::getenv("PW_CAPTURE_PASSIVE")) == 0);
  const bool captureNoForce = (std::getenv("PW_CAPTURE_NO_FORCE") && std::atoi(std::getenv("PW_CAPTURE_NO_FORCE")) != 0);

  // Optional explicit targeting. This is the most reliable way to ensure our
  // capture/playback streams land in the iRig clock domain.
  // Examples:
  //   PW_TARGET_CAPTURE="alsa_input.usb-IK_Multimedia_iRig_HD_X_...pro-input-0"
  //   PW_TARGET_PLAYBACK="alsa_output.usb-IK_Multimedia_iRig_HD_X_...pro-output-0"
  const char *targetCapture = std::getenv("PW_TARGET_CAPTURE");
  const char *targetPlayback = std::getenv("PW_TARGET_PLAYBACK");
  if (targetCapture && targetCapture[0])
    gTargetCaptureName = targetCapture;
  if (targetPlayback && targetPlayback[0])
    gTargetPlaybackName = targetPlayback;

  // Optional explicit clock/group hints.
  // For the iRig in pro-audio profile, these are typically:
  //   PW_CLOCK_NAME="api.alsa.0"
  //   PW_NODE_GROUP="pro-audio-0"
  // Keeping our streams in the same node.group/link-group helps ensure the ALSA
  // device (not Dummy-Driver) clocks the graph.
  const char *wantClockName = std::getenv("PW_CLOCK_NAME");
  const char *wantNodeGroup = std::getenv("PW_NODE_GROUP");

  // Ask PipeWire for a deterministic (realtime) quantum. If PipeWire falls back to
  // a timer-based scheduler, we can end up running at a fraction of realtime
  // (observed pb_x~=0.165, cap_x~=0.083).
  const uint32_t wantQuantum = requestedQuantum;
  const uint32_t wantRate = sampleRate;
  char wantLatency[64];
  std::snprintf(wantLatency, sizeof(wantLatency), "%u/%u", wantQuantum, wantRate);

  uint8_t bufFmt[2048];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(bufFmt, sizeof(bufFmt));

  // We request 2 channels (FL/FR) for all stream types.
  struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
          .format = SPA_AUDIO_FORMAT_F32,
          .rate = sampleRate,
          .channels = 2);
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;

  const struct spa_pod *params[1];
  params[0] = (const struct spa_pod *)spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  if (useLegacyStreams)
  {
    // -------------------- Legacy two-stream mode --------------------
    uint32_t captureChannels = 2;
    if (const char *e = std::getenv("PW_CAPTURE_CHANNELS"))
    {
      const int v = std::atoi(e);
      if (v == 1 || v == 2)
        captureChannels = (uint32_t)v;
    }
    else if (targetCapture && std::strstr(targetCapture, "mono-fallback"))
    {
      captureChannels = 1;
    }

    struct pw_properties *cap_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_APP_NAME, "dsp_engine_v1",
        PW_KEY_NODE_NAME, "dsp_engine_v1.capture",
        PW_KEY_NODE_DESCRIPTION, "Guitar DSP Engine (capture)",
        PW_KEY_NODE_DONT_RECONNECT, diagNoReconnect ? "true" : "false",
        nullptr);

    // Treat capture as an app stream (not a device source).
    pw_properties_set(cap_props, PW_KEY_MEDIA_CLASS, "Audio/Stream");
    pw_properties_set(cap_props, "node.want-driver", "false");
    if (capturePassive)
      pw_properties_set(cap_props, "node.passive", "true");
    if (targetCapture && targetCapture[0])
      pw_properties_set(cap_props, PW_KEY_TARGET_OBJECT, targetCapture);
    if (wantClockName && wantClockName[0])
      pw_properties_set(cap_props, "clock.name", wantClockName);
    if (wantNodeGroup && wantNodeGroup[0])
    {
      pw_properties_set(cap_props, "node.group", wantNodeGroup);
      pw_properties_set(cap_props, "node.link-group", wantNodeGroup);
    }
    if (!captureNoForce)
    {
      pw_properties_set(cap_props, PW_KEY_NODE_LATENCY, wantLatency);
      pw_properties_setf(cap_props, PW_KEY_NODE_FORCE_RATE, "%u", wantRate);
      pw_properties_setf(cap_props, PW_KEY_NODE_FORCE_QUANTUM, "%u", wantQuantum);
    }

    if (tloop)
      pw_thread_loop_lock(tloop);
    capture_stream = pw_stream_new(core, "dsp_engine_v1.capture", cap_props);
    if (tloop)
      pw_thread_loop_unlock(tloop);
    if (!capture_stream)
    {
      std::fprintf(stderr, "Failed to create capture stream\n");
      pw_core_disconnect(core);
      pw_context_destroy(context);
      return 1;
    }

    static const char *kCaptureTag = "capture";
    if (tloop)
      pw_thread_loop_lock(tloop);
    pw_stream_add_listener(capture_stream, &capture_listener, &capture_events, (void *)kCaptureTag);
    if (tloop)
      pw_thread_loop_unlock(tloop);

    logStreamProps(cap_props, "capture");

    // Capture params (allow mono on mono-fallback profiles).
    struct spa_audio_info_raw infoCap = SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .rate = sampleRate,
            .channels = captureChannels);
    if (captureChannels == 1)
      infoCap.position[0] = SPA_AUDIO_CHANNEL_MONO;
    else
    {
      infoCap.position[0] = SPA_AUDIO_CHANNEL_FL;
      infoCap.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    const struct spa_pod *capParams[1];
    capParams[0] = (const struct spa_pod *)spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &infoCap);

    struct pw_properties *pb_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_APP_NAME, "dsp_engine_v1",
        PW_KEY_NODE_NAME, "dsp_engine_v1.playback",
        PW_KEY_NODE_DESCRIPTION, "Guitar DSP Engine (playback)",
        PW_KEY_NODE_DONT_RECONNECT, diagNoReconnect ? "true" : "false",
        nullptr);

    pw_properties_set(pb_props, PW_KEY_MEDIA_CLASS, "Audio/Stream");
    pw_properties_set(pb_props, "node.want-driver", wantDriver ? "true" : "false");
    if (targetPlayback && targetPlayback[0])
      pw_properties_set(pb_props, PW_KEY_TARGET_OBJECT, targetPlayback);
    if (wantClockName && wantClockName[0])
      pw_properties_set(pb_props, "clock.name", wantClockName);
    if (wantNodeGroup && wantNodeGroup[0])
    {
      pw_properties_set(pb_props, "node.group", wantNodeGroup);
      pw_properties_set(pb_props, "node.link-group", wantNodeGroup);
    }
    pw_properties_set(pb_props, PW_KEY_NODE_LATENCY, wantLatency);
    pw_properties_setf(pb_props, PW_KEY_NODE_FORCE_RATE, "%u", wantRate);
    pw_properties_setf(pb_props, PW_KEY_NODE_FORCE_QUANTUM, "%u", wantQuantum);

    if (tloop)
      pw_thread_loop_lock(tloop);
    playback_stream = pw_stream_new(core, "dsp_engine_v1.playback", pb_props);
    if (tloop)
      pw_thread_loop_unlock(tloop);
    if (!playback_stream)
    {
      std::fprintf(stderr, "Failed to create playback stream\n");
      pw_core_disconnect(core);
      pw_context_destroy(context);
      return 1;
    }

    static const char *kPlaybackTag = "playback";
    if (tloop)
      pw_thread_loop_lock(tloop);
    pw_stream_add_listener(playback_stream, &playback_listener, &playback_events, (void *)kPlaybackTag);
    if (tloop)
      pw_thread_loop_unlock(tloop);

    logStreamProps(pb_props, "playback");

    // Playback params are always stereo.
    struct spa_audio_info_raw infoPb = SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .rate = sampleRate,
            .channels = 2);
    infoPb.position[0] = SPA_AUDIO_CHANNEL_FL;
    infoPb.position[1] = SPA_AUDIO_CHANNEL_FR;
    const struct spa_pod *pbParams[1];
    pbParams[0] = (const struct spa_pod *)spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &infoPb);

    if (tloop)
      pw_thread_loop_lock(tloop);
    // NOTE: Use INPUT direction here so PipeWire exposes input_* ports and
    // we can link the device source outputs into this stream explicitly.
    // In practice on this system, OUTPUT direction leaves the capture stream
    // suspended with no visible ports.
    pw_stream_connect(capture_stream,
                      PW_DIRECTION_INPUT,
                      PW_ID_ANY,
                      (pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS |
                                        PW_STREAM_FLAG_AUTOCONNECT |
                                        PW_STREAM_FLAG_EARLY_PROCESS |
                                        PW_STREAM_FLAG_RT_PROCESS |
                                        0),
                      capParams, 1);
    pw_stream_connect(playback_stream,
                      PW_DIRECTION_INPUT,
                      PW_ID_ANY,
                      (pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS |
                                        PW_STREAM_FLAG_AUTOCONNECT |
                                        PW_STREAM_FLAG_EARLY_PROCESS |
                                        PW_STREAM_FLAG_RT_PROCESS |
                                        0),
                      pbParams, 1);
    if (tloop)
      pw_thread_loop_unlock(tloop);

    std::printf("PipeWire streams created (legacy capture/playback)\n");
    std::fflush(stdout);
  }
  else
  {
    // -------------------- Duplex stream --------------------
    // One node in the graph with both input and output ports.
    struct pw_properties *dp_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Duplex",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_APP_NAME, "dsp_engine_v1",
        PW_KEY_NODE_NAME, "dsp_engine_v1.duplex",
        PW_KEY_NODE_DESCRIPTION, "Guitar DSP Engine (duplex)",
        // In rare cases policy may need to reconnect/retarget; allow that during diagnostics.
        PW_KEY_NODE_DONT_RECONNECT, diagNoReconnect ? "true" : "false",
        nullptr);

    // Help session manager/policy understand what this node is.
    pw_properties_set(dp_props, PW_KEY_MEDIA_CLASS, "Audio/Duplex");

    // Keep duplex slaved to device/graph clock (don't become a driver).
    pw_properties_set(dp_props, "node.want-driver", wantDriver ? "true" : "false");
    // Prefer playback target as the clock domain anchor.
    const char *targetDuplex = (targetPlayback && targetPlayback[0]) ? targetPlayback : targetCapture;
    if (targetDuplex && targetDuplex[0])
      pw_properties_set(dp_props, PW_KEY_TARGET_OBJECT, targetDuplex);
    if (wantClockName && wantClockName[0])
      pw_properties_set(dp_props, "clock.name", wantClockName);
    if (wantNodeGroup && wantNodeGroup[0])
    {
      pw_properties_set(dp_props, "node.group", wantNodeGroup);
      pw_properties_set(dp_props, "node.link-group", wantNodeGroup);
    }

    pw_properties_set(dp_props, PW_KEY_NODE_LATENCY, wantLatency);
    pw_properties_setf(dp_props, PW_KEY_NODE_FORCE_RATE, "%u", wantRate);
    pw_properties_setf(dp_props, PW_KEY_NODE_FORCE_QUANTUM, "%u", wantQuantum);

    if (tloop)
      pw_thread_loop_lock(tloop);
    duplex_stream = pw_stream_new(core, "dsp_engine_v1.duplex", dp_props);
    if (tloop)
      pw_thread_loop_unlock(tloop);
    if (!duplex_stream)
    {
      std::fprintf(stderr, "Failed to create duplex stream\n");
      pw_core_disconnect(core);
      pw_context_destroy(context);
      return 1;
    }

    static const char *kDuplexTag = "duplex";
    if (tloop)
      pw_thread_loop_lock(tloop);
    pw_stream_add_listener(duplex_stream, &playback_listener, &duplex_events, (void *)kDuplexTag);
    if (tloop)
      pw_thread_loop_unlock(tloop);

    logStreamProps(dp_props, "duplex");

    // Connect the stream.
    // NOTE: pw_stream has a single direction; using OUTPUT here creates a sink-like
    // node that (in practice on this system) only exposes playback_* input ports.
    // We want a duplex-style filter node that has an input (playback_*) AND an
    // output (capture_*) port so we can link the engine output into a hardware sink.
    // Connecting as INPUT yields a source-like node and reliably exposes capture_*.
    if (tloop)
      pw_thread_loop_lock(tloop);
    pw_stream_connect(duplex_stream,
                      PW_DIRECTION_INPUT,
                      PW_ID_ANY,
                      (pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS |
                                        PW_STREAM_FLAG_AUTOCONNECT |
                                        PW_STREAM_FLAG_EARLY_PROCESS |
                                        PW_STREAM_FLAG_RT_PROCESS |
                                        0),
                      params, 1);
    if (tloop)
      pw_thread_loop_unlock(tloop);

    std::printf("PipeWire stream created (duplex)\n");
    std::fflush(stdout);
  }

  // Load NAM model
  std::printf("Loading NAM model from: %s\n", namModelPath.c_str());
  std::fflush(stdout);
  if (!namModelPath.empty())
  {
    try
    {
      if (std::filesystem::exists(namModelPath))
      {
        std::printf("NAM: File exists, loading...\n");
        std::fflush(stdout);

        const auto p = std::filesystem::path(namModelPath);
        gModel = nam::get_dsp(p);
        if (gModel)
        {
          std::printf("NAM: Model loaded, prewarming...\n");
          std::fflush(stdout);

          // Log model metadata that affects sound.
          const double expSR = gModel->GetExpectedSampleRate();
          if (expSR > 0.0)
            std::printf("NAM: expected sample rate: %.1f Hz\n", expSR);
          else
            std::printf("NAM: expected sample rate: (unknown)\n");

          if (gModel->HasInputLevel())
          {
            const float lvl = (float)gModel->GetInputLevel();
            namInputLevelDbu.store(lvl, std::memory_order_relaxed);
            namHasInputLevel.store(true, std::memory_order_relaxed);
            std::printf("NAM: input level (0 dBFS sine) = %.2f dBu\n", (double)lvl);
          }
          else
          {
            namHasInputLevel.store(false, std::memory_order_relaxed);
            std::printf("NAM: input level: (not provided by model)\n");
          }

          if (gModel->HasOutputLevel())
            std::printf("NAM: output level (0 dBFS sine) = %.2f dBu\n", gModel->GetOutputLevel());
          if (gModel->HasLoudness())
          {
            try
            {
              std::printf("NAM: loudness = %.2f dB\n", gModel->GetLoudness());
            }
            catch (...)
            {
            }
          }

          gModel->ResetAndPrewarm((double)sampleRate, (int)bufferSize);
          gNamBlockSize = bufferSize;
          modelReady.store(true, std::memory_order_release);
          ::printf("NAM: loaded and ready\n");
          ::fflush(stdout);
        }
      }
      else
      {
        std::printf("NAM: File not found\n");
        std::fflush(stdout);
      }
    }
    catch (const std::exception &e)
    {
      std::printf("NAM: error: %s\n", e.what());
      std::fflush(stdout);
    }
  }

  // Load IR
  ::printf("Loading IR from: %s\n", irPath.c_str());
  ::fflush(stdout);
  if (!irPath.empty())
  {
    IRData ir;
    std::string err;
    if (load_ir_mono(irPath, ir, err) && ir.sampleRate == (int)sampleRate)
    {
      ::printf("IR: File loaded, initializing convolver...\n");
      ::fflush(stdout);
      // Cache IR so we can re-init the convolver if PipeWire chooses a different quantum.
      gIrCached = ir.mono;

      if (gIR.init(gIrCached, (int)bufferSize))
      {
        irReady.store(true, std::memory_order_release);
        ::printf("IR: loaded and ready\n");
        ::fflush(stdout);
      }
    }
    else
    {
      ::printf("IR: Failed to load or wrong sample rate\n");
      ::fflush(stdout);
    }
  }

  // Start UDP control + meters
  ::printf("Starting UDP control thread...\n");
  ::fflush(stdout);
  std::thread ctl(udpControlThread);
  std::thread meter(meterThread);

  ::printf("DSP engine running. Press Ctrl+C to stop.\n");

  // The thread-loop is already running; just wait until we get a signal.
  while (running.load(std::memory_order_relaxed))
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Flush any pending NAM dump WAVs on shutdown.
  dumpNamFlush(sampleRate);

  // Cleanup
  running.store(false);
  if (ctl.joinable())
    ctl.join();
  if (meter.joinable())
    meter.join();

  if (tloop)
    pw_thread_loop_lock(tloop);

  if (duplex_stream)
  {
    pw_stream_destroy(duplex_stream);
    duplex_stream = nullptr;
  }
  if (capture_stream)
  {
    pw_stream_destroy(capture_stream);
    capture_stream = nullptr;
  }
  if (playback_stream)
  {
    pw_stream_destroy(playback_stream);
    playback_stream = nullptr;
  }
  if (registry)
  {
    pw_proxy_destroy((pw_proxy *)registry);
    registry = nullptr;
  }
  if (core)
  {
    pw_core_disconnect(core);
    core = nullptr;
  }
  if (context)
  {
    pw_context_destroy(context);
    context = nullptr;
  }

  if (tloop)
    pw_thread_loop_unlock(tloop);

  if (tloop)
  {
    pw_thread_loop_stop(tloop);
    pw_thread_loop_destroy(tloop);
    tloop = nullptr;
  }
  pw_deinit();

  return 0;
}
