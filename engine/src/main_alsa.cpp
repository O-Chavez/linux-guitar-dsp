#include <alsa/asoundlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sndfile.h>
#include <xmmintrin.h>
#ifdef __SSE3__
#include <pmmintrin.h>
#endif

#include "fft_convolver.h"
#include "get_dsp.h"
#include "ir_loader.h"
#include "json.hpp"
#include "signal_chain.h"
#include "signal_chain_schema.h"
#include "chain_control_server.h"

// UDP
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *buildTypeString()
{
#ifdef DSP_BUILD_TYPE
  return DSP_BUILD_TYPE;
#else
  return "unknown";
#endif
}

static void logBuildBanner()
{
  char exePath[4096] = {0};
  const ssize_t n = ::readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
  if (n > 0)
    exePath[n] = '\0';
  else
    std::snprintf(exePath, sizeof(exePath), "(unknown)");

#if defined(__clang__)
  const char *compiler = "clang";
#elif defined(__GNUC__)
  const char *compiler = "gcc";
#else
  const char *compiler = "unknown-cc";
#endif

  std::fprintf(stderr,
               "Build: type=%s exe=%s compiler=%s version=%s optimize=%d ndebug=%d\n",
               buildTypeString(),
               exePath,
               compiler,
               __VERSION__,
#ifdef __OPTIMIZE__
               1,
#else
               0,
#endif
#ifdef NDEBUG
               1
#else
               0
#endif
  );

  // Baseline gate: refuse to run non-Release binaries unless explicitly allowed.
  const char *envEnforce = std::getenv("ALSA_ENFORCE_RELEASE");
  const bool enforce = (envEnforce == nullptr) ? true : (std::atoi(envEnforce) != 0);

#if !defined(__OPTIMIZE__) || !defined(NDEBUG)
  std::fprintf(stderr,
               "ALSA: WARNING: non-Release build detected (optimize=%d ndebug=%d).\n",
#ifdef __OPTIMIZE__
               1,
#else
               0,
#endif
#ifdef NDEBUG
               1
#else
               0
#endif
  );
  if (enforce)
  {
    std::fprintf(stderr, "ALSA: Refusing to run (set ALSA_ENFORCE_RELEASE=0 to override).\n");
    std::fflush(stderr);
    std::exit(2);
  }
#else
  (void)enforce;
#endif
}

static void logThreadRtState()
{
  int policy = 0;
  sched_param sp{};
  if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0)
  {
    const char *pname = "UNKNOWN";
    switch (policy)
    {
    case SCHED_OTHER:
      pname = "SCHED_OTHER";
      break;
    case SCHED_FIFO:
      pname = "SCHED_FIFO";
      break;
    case SCHED_RR:
      pname = "SCHED_RR";
      break;
#ifdef SCHED_BATCH
    case SCHED_BATCH:
      pname = "SCHED_BATCH";
      break;
#endif
#ifdef SCHED_IDLE
    case SCHED_IDLE:
      pname = "SCHED_IDLE";
      break;
#endif
    default:
      break;
    }

    std::fprintf(stderr, "ALSA: thread sched policy=%s prio=%d\n", pname, sp.sched_priority);
  }
  else
  {
    std::fprintf(stderr, "ALSA: pthread_getschedparam failed: %s\n", std::strerror(errno));
  }

  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0)
  {
    std::string cpus;
    bool first = true;
    for (int i = 0; i < CPU_SETSIZE; i++)
    {
      if (CPU_ISSET(i, &set))
      {
        if (!first)
          cpus += ",";
        first = false;
        cpus += std::to_string(i);
      }
    }
    if (cpus.empty())
      cpus = "(none?)";
    std::fprintf(stderr, "ALSA: cpu affinity=%s\n", cpus.c_str());
  }
  else
  {
    std::fprintf(stderr, "ALSA: sched_getaffinity failed: %s\n", std::strerror(errno));
  }
}

static void logPcmNegotiated(snd_pcm_t *pcm, const char *label)
{
  snd_pcm_hw_params_t *params = nullptr;
  snd_pcm_hw_params_alloca(&params);
  if (snd_pcm_hw_params_current(pcm, params) < 0)
    return;

  unsigned int rate = 0;
  unsigned int channels = 0;
  int dir = 0;
  snd_pcm_uframes_t period = 0;
  unsigned int periods = 0;
  snd_pcm_uframes_t buffer = 0;

  (void)snd_pcm_hw_params_get_rate(params, &rate, &dir);
  (void)snd_pcm_hw_params_get_channels(params, &channels);
  dir = 0;
  (void)snd_pcm_hw_params_get_period_size(params, &period, &dir);
  dir = 0;
  (void)snd_pcm_hw_params_get_periods(params, &periods, &dir);
  (void)snd_pcm_hw_params_get_buffer_size(params, &buffer);

  std::fprintf(stderr,
               "ALSA: negotiated %s rate=%u ch=%u period=%lu periods=%u buffer=%lu\n",
               label,
               rate,
               channels,
               (unsigned long)period,
               periods,
               (unsigned long)buffer);
}

static std::atomic<bool> running{true};
static std::atomic<bool> debugOnce{false};

static void dump_alsa_device_hints()
{
  auto dumpFile = [](const char *path, const char *label)
  {
    std::ifstream f(path);
    if (!f.is_open())
      return;
    std::string line;
    std::fprintf(stderr, "ALSA: ---- %s (%s) ----\n", label, path);
    while (std::getline(f, line))
      std::fprintf(stderr, "%s\n", line.c_str());
  };

  dumpFile("/proc/asound/cards", "cards");
  dumpFile("/proc/asound/pcm", "pcm");
  std::fprintf(stderr,
               "ALSA: Hint: try 'aplay -l' / 'arecord -l' to find hw:<card>,<device> (or use plughw/plughw).\n");
}

// v1 orchestration: ordered signal chain with RT-safe swapping.
static pedal::control::ChainRuntimeState gChainState;
static std::thread gControlThread;
static pedal::chain::ChainSpec gBootSpec;
static std::atomic<bool> gBootSpecValid{false};
// Retire old chains outside the audio thread to avoid frees/teardowns in RT.
static constexpr uint32_t kRetireQueueSize = 128;
static std::shared_ptr<pedal::dsp::SignalChain> gRetireQueue[kRetireQueueSize];
// Single-producer (audio thread) + single-consumer (retire thread).
// Use 64-bit indices to avoid wraparound bugs on long uptimes.
static std::atomic<uint64_t> gRetireWrite{0};
static std::atomic<uint64_t> gRetireRead{0};
static std::atomic<bool> gRetireRunning{true};
static std::thread gRetireThread;
static std::atomic<uint64_t> gRetireQueueFull{0};

static void startRetireThread()
{
  if (gRetireThread.joinable())
    return;

  gRetireRunning.store(true, std::memory_order_relaxed);
  gRetireThread = std::thread([]()
                              {
    while (gRetireRunning.load(std::memory_order_relaxed))
    {
      uint64_t r = gRetireRead.load(std::memory_order_relaxed);
      const uint64_t w = gRetireWrite.load(std::memory_order_acquire);
      while (r != w)
      {
        const uint32_t idx = (uint32_t)(r % (uint64_t)kRetireQueueSize);
        gRetireQueue[idx].reset();
        r++;
      }
      gRetireRead.store(r, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Drain on shutdown.
    uint64_t r = gRetireRead.load(std::memory_order_relaxed);
    const uint64_t w = gRetireWrite.load(std::memory_order_acquire);
    while (r != w)
    {
      const uint32_t idx = (uint32_t)(r % (uint64_t)kRetireQueueSize);
      gRetireQueue[idx].reset();
      r++;
    }
    gRetireRead.store(r, std::memory_order_release); });
}

static inline bool retireQueueHasSpace() noexcept
{
  const uint64_t w = gRetireWrite.load(std::memory_order_relaxed);
  const uint64_t r = gRetireRead.load(std::memory_order_acquire);
  return ((w - r) < (uint64_t)kRetireQueueSize);
}

static inline bool retireChainFromAudioThread(std::shared_ptr<pedal::dsp::SignalChain> &old) noexcept
{
  if (!old)
    return true;

  const uint64_t w = gRetireWrite.load(std::memory_order_relaxed);
  const uint64_t r = gRetireRead.load(std::memory_order_acquire);
  if ((w - r) >= (uint64_t)kRetireQueueSize)
  {
    // Queue full (should be rare). Keep the shared_ptr alive and retry next block.
    gRetireQueueFull.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const uint32_t idx = (uint32_t)(w % (uint64_t)kRetireQueueSize);
  gRetireQueue[idx] = std::move(old);
  gRetireWrite.store(w + 1, std::memory_order_release);
  old.reset();
  return true;
}

// Debug/passthrough mode
static std::atomic<bool> passthroughMode{false};
// Debug: bypass NAM and/or IR stages (env overrides)
static std::atomic<bool> bypassNam{false};
static std::atomic<bool> bypassIr{false};
static std::atomic<bool> disableSoftclip{false};
static std::atomic<bool> useTanhSoftclip{false};
static std::atomic<bool> namUseInputLevel{true};
static std::atomic<float> namInputLevelDbu{0.0f};
static std::atomic<bool> namHasInputLevel{false};
static std::atomic<float> namPreGainDb{-12.0f};
static std::atomic<float> namPreGainLin{1.0f};
static std::atomic<float> namPostGainDb{0.0f};
static std::atomic<float> namPostGainLin{1.0f};
static std::atomic<float> namInLimit{0.90f};
static std::atomic<float> namLevelScaleLin{1.0f};

// Peak metering (updated in RT loop, read by meter thread)
static std::atomic<float> peakInput{0.0f};
static std::atomic<float> peakNamOut{0.0f};
static std::atomic<float> peakIrOut{0.0f};
static std::atomic<float> peakFinalOut{0.0f};

// Optional realtime dump of the signal going into/out of NAM (for offline analysis).
// Env:
//   DUMP_NAM_IN_WAV=/tmp/nam_in.wav
//   DUMP_NAM_OUT_WAV=/tmp/nam_out.wav
//   DUMP_NAM_SECONDS=10
// The dump is collected in the RT loop into a ring buffer and flushed on shutdown.
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

// Optional realtime dump of IR output (post-convolver) for offline analysis.
// Env:
//   DUMP_IR_OUT_WAV=/tmp/ir_out.wav
//   DUMP_IR_SECONDS=10
static std::string dumpIrOutPath;
static uint32_t dumpIrMaxFrames = 0;
static uint32_t dumpIrInitSr = 0;
static uint32_t dumpIrInitSecs = 0;
static std::atomic<uint32_t> dumpIrOutWritePos{0};
static std::atomic<uint32_t> dumpIrOutTotalWritten{0};
static std::vector<float> dumpIrOut;
static std::mutex dumpIrFlushMutex;

// Config
static std::atomic<float> inputTrimDb{0.0f};
static std::atomic<float> inputTrimLin{1.0f};
static std::atomic<float> outputGainDb{0.0f};
static std::atomic<float> outputGainLin{1.0f};
static std::atomic<bool> sanitizeOutput{false};
static std::atomic<bool> verboseXruns{false};
static std::atomic<float> irGainDb{0.0f};
static std::atomic<float> irGainLin{1.0f};
static std::atomic<float> irTargetDb{-6.0f};
static std::atomic<bool> irUseTarget{false};
static std::atomic<bool> logStats{false};
static std::atomic<bool> logTiming{false};
static std::string namModelPath;
static std::string irPath;

// Legacy NAM/IR globals removed: the ordered signal chain owns these resources now.

static inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
static inline float clampf(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi
                                                                                          : v; }
static inline float softclipFast(float x)
{
  if (x > 1.0f)
    return 1.0f;
  if (x < -1.0f)
    return -1.0f;
  const float b = 0.3333333f;
  return x - b * x * x * x;
}

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

static inline uint32_t readEnvU32AllowZero(const char *k, uint32_t def)
{
  if (const char *e = std::getenv(k))
  {
    long v = std::strtol(e, nullptr, 10);
    if (v >= 0)
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

static void dumpIrInit(uint32_t sr)
{
  const char *outP = std::getenv("DUMP_IR_OUT_WAV");
  if (outP)
    dumpIrOutPath = outP;
  if (dumpIrOutPath.empty())
    return;

  const uint32_t secs = readEnvU32("DUMP_IR_SECONDS", 10);

  if (dumpIrMaxFrames != 0)
  {
    if (sr == dumpIrInitSr && secs == dumpIrInitSecs)
      return;

    std::printf("Dump: IR re-init due to SR/secs change (old sr=%u secs=%u -> new sr=%u secs=%u)\n",
                dumpIrInitSr, dumpIrInitSecs, sr, secs);
    std::fflush(stdout);
  }

  dumpIrInitSr = sr;
  dumpIrInitSecs = secs;
  dumpIrMaxFrames = sr * secs;
  dumpIrOutWritePos.store(0, std::memory_order_relaxed);
  dumpIrOutTotalWritten.store(0, std::memory_order_relaxed);
  dumpIrOut.assign(dumpIrMaxFrames, 0.0f);

  std::printf("Dump: enabled IR dump for %u seconds (%u frames).\n", secs, dumpIrMaxFrames);
  std::printf("Dump: DUMP_IR_OUT_WAV=%s\n", dumpIrOutPath.c_str());
  std::fflush(stdout);
}

static inline void dumpNamPushIn(const float *in, uint32_t nframes)
{
  if (dumpNamMaxFrames == 0 || dumpNamIn.empty())
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
}

static inline void dumpNamPushOut(const float *out, uint32_t nframes)
{
  if (dumpNamMaxFrames == 0 || dumpNamOut.empty())
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

static inline void dumpIrPushOut(const float *out, uint32_t nframes)
{
  if (dumpIrMaxFrames == 0 || dumpIrOut.empty())
    return;
  uint32_t wp = dumpIrOutWritePos.load(std::memory_order_relaxed);
  if (wp >= dumpIrMaxFrames)
    return;
  const uint32_t toWrite = std::min(nframes, dumpIrMaxFrames - wp);
  if (toWrite == 0)
    return;
  if (out)
    std::memcpy(&dumpIrOut[wp], out, sizeof(float) * toWrite);
  else
    std::memset(&dumpIrOut[wp], 0, sizeof(float) * toWrite);
  dumpIrOutWritePos.store(wp + toWrite, std::memory_order_relaxed);
  dumpIrOutTotalWritten.store(wp + toWrite, std::memory_order_relaxed);
}

static bool writeWavPcm16Mono(const std::string &path, const std::vector<float> &y, uint32_t nframes, uint32_t sr)
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
      if (writeWavPcm16Mono(dumpNamInPath, y, (uint32_t)y.size(), sr))
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
      if (writeWavPcm16Mono(dumpNamOutPath, y, (uint32_t)y.size(), sr))
        std::printf("Dump: wrote %u frames (%.2fs) to %s\n",
                    (unsigned)y.size(),
                    (double)y.size() / (double)sr,
                    dumpNamOutPath.c_str());
    }
  }
  std::fflush(stdout);
}

static void dumpIrFlush(uint32_t sr)
{
  std::lock_guard<std::mutex> lock(dumpIrFlushMutex);
  if (dumpIrMaxFrames == 0)
    return;

  const uint32_t outTotal = dumpIrOutTotalWritten.load(std::memory_order_relaxed);
  const uint32_t outWp = dumpIrOutWritePos.load(std::memory_order_relaxed);

  std::printf("Dump: IR flush sr=%u maxFrames=%u out_wp=%u out_total=%u\n",
              sr, dumpIrMaxFrames, outWp, outTotal);
  std::fflush(stdout);

  if (!dumpIrOutPath.empty() && !dumpIrOut.empty())
  {
    const uint32_t total = std::min(outTotal, dumpIrMaxFrames);
    if (total > 0)
    {
      std::vector<float> y(dumpIrOut.begin(), dumpIrOut.begin() + total);
      if (writeWavPcm16Mono(dumpIrOutPath, y, (uint32_t)y.size(), sr))
        std::printf("Dump: wrote %u frames (%.2fs) to %s\n",
                    (unsigned)y.size(),
                    (double)y.size() / (double)sr,
                    dumpIrOutPath.c_str());
    }
  }
  std::fflush(stdout);
}

static void onSignal(int) { running.store(false); }

static void tryEnableRealtime()
{
  const char *envRt = std::getenv("ALSA_ENABLE_RT");
  const bool enable = (envRt == nullptr) ? true : (std::atoi(envRt) != 0);
  if (!enable)
    return;

  const int prio = std::getenv("ALSA_RT_PRIORITY") ? std::atoi(std::getenv("ALSA_RT_PRIORITY")) : 80;

  // Lock memory to avoid page faults in the audio thread.
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
    std::fprintf(stderr, "ALSA: mlockall failed (continuing): %s\n", std::strerror(errno));
  else
    std::fprintf(stderr, "ALSA: mlockall ok\n");

  // Try to elevate scheduling policy.
  sched_param sp{};
  sp.sched_priority = prio;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
  {
    std::fprintf(stderr, "ALSA: pthread_setschedparam failed (continuing): %s\n", std::strerror(errno));
  }
  else
    std::fprintf(stderr, "ALSA: requested realtime (SCHED_FIFO prio=%d)\n", prio);

  // Always log the effective state (policy/priority/affinity).
  logThreadRtState();
}

static void configureDenormals()
{
  const char *env = std::getenv("ALSA_DENORMALS_OFF");
  const bool enable = (env == nullptr) ? true : (std::atoi(env) == 0);
  if (!enable)
    return;

#if defined(__SSE__)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#ifdef _MM_DENORMALS_ZERO_ON
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
  std::printf("ALSA: denormals flushed to zero\n");
#else
  std::printf("ALSA: denormals flush not supported on this CPU\n");
#endif
}

static void updateNamLevelScale()
{
  if (!namUseInputLevel.load(std::memory_order_relaxed) ||
      !namHasInputLevel.load(std::memory_order_relaxed))
  {
    namLevelScaleLin.store(1.0f, std::memory_order_relaxed);
    return;
  }

  constexpr float refDbu = 12.2f;
  const float modelDbu = namInputLevelDbu.load(std::memory_order_relaxed);
  const float scale = std::pow(10.0f, (refDbu - modelDbu) / 20.0f);
  namLevelScaleLin.store(scale, std::memory_order_relaxed);
}

// -------------------- Config --------------------
static void loadConfig()
{
  const char *path = "/opt/pedal/config/chain.json";
  try
  {
    std::ifstream f(path);
    if (!f.is_open())
    {
      std::fprintf(stderr, "Config: could not open %s (using defaults)\n", path);
      return;
    }

    nlohmann::json j;
    f >> j;

    pedal::chain::ValidationError verr;
    auto parsed = pedal::chain::parseChainJson(j, verr);
    if (!parsed)
    {
      std::fprintf(stderr, "Config: invalid chain.json (parse): %s\n", verr.message.c_str());
      return;
    }

    auto validated = pedal::chain::validateChainSpec(*parsed, verr);
    if (!validated)
    {
      std::fprintf(stderr, "Config: invalid chain.json (validate): %s\n", verr.message.c_str());
      return;
    }

    gBootSpec = *validated;
    gBootSpecValid.store(true, std::memory_order_release);
    std::printf("Config: loaded ordered chain (nodes=%zu)\n", gBootSpec.chain.size());
  }
  catch (const std::exception &e)
  {
    std::fprintf(stderr, "Config: error loading %s: %s\n", path, e.what());
  }
}

static void applyEnvOverrides()
{
  if (const char *envTrim = std::getenv("ALSA_INPUT_TRIM_DB"))
  {
    const float v = clampf(std::strtof(envTrim, nullptr), -24.0f, 24.0f);
    inputTrimDb.store(v);
    inputTrimLin.store(dbToLin(v));
  }

  if (const char *envPt = std::getenv("ALSA_PASSTHROUGH"))
  {
    const int v = std::atoi(envPt);
    passthroughMode.store(v != 0);
  }

  if (const char *envOut = std::getenv("ALSA_OUTPUT_GAIN_DB"))
  {
    const float v = clampf(std::strtof(envOut, nullptr), -24.0f, 24.0f);
    outputGainDb.store(v);
    outputGainLin.store(dbToLin(v));
  }

  if (const char *envIr = std::getenv("ALSA_IR_GAIN_DB"))
  {
    const float v = clampf(std::strtof(envIr, nullptr), -24.0f, 24.0f);
    irGainDb.store(v);
    irGainLin.store(dbToLin(v));
  }

  if (const char *envIrTarget = std::getenv("ALSA_IR_TARGET_DB"))
  {
    const float v = clampf(std::strtof(envIrTarget, nullptr), -24.0f, 0.0f);
    irTargetDb.store(v);
    irUseTarget.store(true);
  }

  if (const char *envBypassNam = std::getenv("ALSA_BYPASS_NAM"))
  {
    const int v = std::atoi(envBypassNam);
    bypassNam.store(v != 0);
  }

  if (const char *envBypassIr = std::getenv("ALSA_BYPASS_IR"))
  {
    const int v = std::atoi(envBypassIr);
    bypassIr.store(v != 0);
  }

  if (const char *envNamUseLevel = std::getenv("ALSA_NAM_USE_INPUT_LEVEL"))
  {
    const int v = std::atoi(envNamUseLevel);
    namUseInputLevel.store(v != 0);
  }

  // Prefer ALSA_NAM_PRE_GAIN_DB, but keep NAM_PRE_GAIN_DB for backward compatibility.
  const char *envPre = std::getenv("ALSA_NAM_PRE_GAIN_DB");
  if (!envPre)
    envPre = std::getenv("NAM_PRE_GAIN_DB");
  if (envPre)
  {
    const float v = clampf(std::strtof(envPre, nullptr), -24.0f, 24.0f);
    namPreGainDb.store(v);
    namPreGainLin.store(dbToLin(v));
  }

  if (const char *envPost = std::getenv("ALSA_NAM_POST_GAIN_DB"))
  {
    const float v = clampf(std::strtof(envPost, nullptr), -24.0f, 24.0f);
    namPostGainDb.store(v);
    namPostGainLin.store(dbToLin(v));
  }

  if (const char *envLim = std::getenv("ALSA_NAM_IN_LIMIT"))
  {
    const float v = clampf(std::strtof(envLim, nullptr), 0.05f, 1.0f);
    namInLimit.store(v);
  }

  if (const char *envNoClip = std::getenv("ALSA_DISABLE_SOFTCLIP"))
  {
    const int v = std::atoi(envNoClip);
    disableSoftclip.store(v != 0);
  }

  if (const char *envTanh = std::getenv("ALSA_SOFTCLIP_TANH"))
  {
    const int v = std::atoi(envTanh);
    useTanhSoftclip.store(v != 0);
    if (v != 0)
      std::fprintf(stderr, "ALSA: warning: tanh softclip is expensive\n");
  }

  if (const char *envSan = std::getenv("ALSA_SANITIZE_OUTPUT"))
  {
    const int v = std::atoi(envSan);
    sanitizeOutput.store(v != 0);
  }

  if (const char *envX = std::getenv("ALSA_VERBOSE_XRUN"))
  {
    const int v = std::atoi(envX);
    verboseXruns.store(v != 0);
  }

  if (const char *envStats = std::getenv("ALSA_LOG_STATS"))
  {
    const int v = std::atoi(envStats);
    logStats.store(v != 0);
  }

  if (const char *envTiming = std::getenv("ALSA_LOG_TIMING"))
  {
    const int v = std::atoi(envTiming);
    logTiming.store(v != 0);
  }
}

// -------------------- Signal chain orchestration --------------------
static pedal::chain::ChainSpec defaultChainSpec(uint32_t sampleRate)
{
  pedal::chain::ChainSpec spec;
  spec.version = 1;
  spec.sampleRate = sampleRate;

  pedal::chain::NodeSpec input;
  input.id = "input";
  input.type = "input";
  input.category = "utility";
  input.enabled = true;
  input.params = nlohmann::json::object();
  input.params["inputTrimDb"] = 0.0;

  pedal::chain::NodeSpec amp;
  amp.id = "amp1";
  amp.type = "nam_model";
  amp.category = "amp";
  amp.enabled = true;
  amp.params = nlohmann::json::object();
  amp.params["preGainDb"] = -12.0;
  amp.params["postGainDb"] = 0.0;
  amp.params["levelDb"] = 0.0;

  pedal::chain::NodeSpec cab;
  cab.id = "cab1";
  cab.type = "ir_convolver";
  cab.category = "cab";
  cab.enabled = true;
  cab.params = nlohmann::json::object();
  cab.params["levelDb"] = 0.0;
  cab.params["targetDb"] = -6.0;

  pedal::chain::NodeSpec output;
  output.id = "output";
  output.type = "output";
  output.category = "utility";
  output.enabled = true;
  output.params = nlohmann::json::object();
  output.params["levelDb"] = 0.0;

  spec.chain = {input, amp, cab, output};
  return spec;
}

static void applyBypassFlagsToSpec(pedal::chain::ChainSpec &spec)
{
  const bool passthrough = passthroughMode.load(std::memory_order_relaxed);
  const bool bNam = bypassNam.load(std::memory_order_relaxed);
  const bool bIr = bypassIr.load(std::memory_order_relaxed);

  for (auto &n : spec.chain)
  {
    if (n.type == "nam_model")
    {
      if (passthrough || bNam)
        n.enabled = false;
    }
    else if (n.type == "ir_convolver")
    {
      if (passthrough || bIr)
        n.enabled = false;
    }
  }
}

static void initChainRuntime(uint32_t sampleRate, uint32_t maxBlockFrames)
{
  pedal::chain::ChainSpec spec;
  if (gBootSpecValid.load(std::memory_order_acquire))
    spec = gBootSpec;
  else
    spec = defaultChainSpec(sampleRate);

  spec.sampleRate = sampleRate;
  applyBypassFlagsToSpec(spec);

  // If caller provided an env override for trim, apply it into the chain input node.
  if (std::getenv("ALSA_INPUT_TRIM_DB") != nullptr)
  {
    const float v = clampf(inputTrimDb.load(std::memory_order_relaxed), -24.0f, 24.0f);
    for (auto &n : spec.chain)
    {
      if (n.type == "input")
      {
        if (!n.params.is_object())
          n.params = nlohmann::json::object();
        n.params["inputTrimDb"] = v;
        break;
      }
    }
  }

  pedal::chain::ValidationError verr;
  auto validated = pedal::chain::validateChainSpec(spec, verr);
  if (!validated)
  {
    std::fprintf(stderr, "Chain: boot chain invalid after normalization: %s\n", verr.message.c_str());
    spec = defaultChainSpec(sampleRate);
    applyBypassFlagsToSpec(spec);
  }

  gChainState.ctx.sampleRate = sampleRate;
  gChainState.ctx.maxBlockFrames = maxBlockFrames;

  // Provide realtime param pointers for nodes (RT-safe param updates).
  gChainState.ctx.inputTrimDb = &inputTrimDb;
  gChainState.ctx.inputTrimLin = &inputTrimLin;

  startRetireThread();

  // Socket path can be overridden for integration with Node backend.
  if (const char *sp = std::getenv("DSP_CONTROL_SOCK"))
    gChainState.socketPath = sp;

  std::string buildErr;
  auto built = pedal::dsp::buildChain(spec, gChainState.ctx, buildErr);
  if (!built || !built->chain)
  {
    std::fprintf(stderr, "Chain: failed to build boot chain: %s\n", buildErr.c_str());
    // Final fallback: bypassed default chain (DI-through).
    auto fb = defaultChainSpec(sampleRate);
    for (auto &n : fb.chain)
      if (n.type == "nam_model" || n.type == "ir_convolver")
        n.enabled = false;

    std::string fbErr;
    auto fbBuilt = pedal::dsp::buildChain(fb, gChainState.ctx, fbErr);
    if (fbBuilt && fbBuilt->chain)
    {
      std::atomic_store_explicit(&gChainState.activeChain, fbBuilt->chain, std::memory_order_release);
      gChainState.lastSpec = fb;
    }
    else
    {
      std::fprintf(stderr, "Chain: fatal - could not build fallback chain: %s\n", fbErr.c_str());
    }
  }
  else
  {
    std::atomic_store_explicit(&gChainState.activeChain, built->chain, std::memory_order_release);
    gChainState.lastSpec = spec;
    if (!built->warning.empty())
      std::fprintf(stderr, "Chain: warning: %s\n", built->warning.c_str());
  }

  if (!gControlThread.joinable())
    gControlThread = pedal::control::startControlServer(&gChainState);
}

// -------------------- UDP control --------------------
static void udpControlThread()
{
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
  {
    std::fprintf(stderr, "Control: failed to create UDP socket\n");
    return;
  }

  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  if (::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
  {
    std::fprintf(stderr, "Control: failed to set recv timeout: %s\n", std::strerror(errno));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9000);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
  {
    std::fprintf(stderr, "Control: failed to bind UDP socket\n");
    ::close(sock);
    return;
  }

  std::printf("Control: UDP localhost:9000 (send: TRIM_DB <value>)\n");

  char buf[256];
  while (running.load(std::memory_order_relaxed))
  {
    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    ssize_t n = ::recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr *)&src, &slen);
    if (n <= 0)
    {
      if (n < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
          continue;
      }
      continue;
    }
    buf[n] = '\0';

    float valDb = 0.0f;
    if (std::sscanf(buf, "TRIM_DB %f", &valDb) == 1)
    {
      valDb = clampf(valDb, -24.0f, 24.0f);
      inputTrimDb.store(valDb);
      inputTrimLin.store(dbToLin(valDb));

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

static int setup_pcm(snd_pcm_t *pcm,
                     snd_pcm_stream_t stream,
                     unsigned int &rate,
                     unsigned int channels,
                     snd_pcm_uframes_t &periodSize,
                     unsigned int &periods,
                     snd_pcm_uframes_t &bufferSize)
{
  snd_pcm_hw_params_t *hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);
  int err = snd_pcm_hw_params_any(pcm, hw);
  if (err < 0)
    return err;

  err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0)
    return err;

  err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
  if (err < 0)
    return err;

  err = snd_pcm_hw_params_set_channels(pcm, hw, channels);
  if (err < 0)
    return err;

  // Disable ALSA resampling when possible.
  snd_pcm_hw_params_set_rate_resample(pcm, hw, 0);

  unsigned int setRate = rate;
  err = snd_pcm_hw_params_set_rate_near(pcm, hw, &setRate, nullptr);
  if (err < 0)
    return err;

  snd_pcm_uframes_t setPeriod = periodSize;
  err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &setPeriod, nullptr);
  if (err < 0)
    return err;

  unsigned int setPeriods = periods;
  err = snd_pcm_hw_params_set_periods_near(pcm, hw, &setPeriods, nullptr);
  if (err < 0)
    return err;

  err = snd_pcm_hw_params(pcm, hw);
  if (err < 0)
    return err;

  periodSize = setPeriod;
  periods = setPeriods;
  rate = setRate;

  bufferSize = 0;
  snd_pcm_hw_params_get_buffer_size(hw, &bufferSize);

  std::printf("ALSA %s: rate=%u ch=%u period=%lu periods=%u buffer=%lu\n",
              (stream == SND_PCM_STREAM_CAPTURE) ? "capture" : "playback",
              setRate, channels, (unsigned long)periodSize, periods, (unsigned long)bufferSize);
  return 0;
}

static int setup_sw_params(snd_pcm_t *pcm,
                           snd_pcm_stream_t stream,
                           snd_pcm_uframes_t periodSize,
                           snd_pcm_uframes_t bufferSize)
{
  snd_pcm_sw_params_t *sw = nullptr;
  snd_pcm_sw_params_alloca(&sw);
  int err = snd_pcm_sw_params_current(pcm, sw);
  if (err < 0)
    return err;

  // Wake up when we have a full period available.
  err = snd_pcm_sw_params_set_avail_min(pcm, sw, periodSize);
  if (err < 0)
    return err;

  // Start playback once the buffer has enough data to avoid immediate underruns.
  if (stream == SND_PCM_STREAM_PLAYBACK)
    err = snd_pcm_sw_params_set_start_threshold(pcm, sw, bufferSize - periodSize);
  else
    // Start capture only after a full period is available.
    err = snd_pcm_sw_params_set_start_threshold(pcm, sw, periodSize);
  if (err < 0)
    return err;

  return snd_pcm_sw_params(pcm, sw);
}

static void log_pcm_state(snd_pcm_t *pcm, const char *label)
{
  const snd_pcm_state_t st = snd_pcm_state(pcm);
  std::fprintf(stderr, "ALSA: %s state=%s\n", label, snd_pcm_state_name(st));
}

static bool recover_pcm(snd_pcm_t *pcm, const char *label, int err)
{
  const bool doLogState = verboseXruns.load(std::memory_order_relaxed);

  if (err == -EPIPE || err == -ESTRPIPE)
  {
    snd_pcm_drop(pcm);
    int prep = snd_pcm_prepare(pcm);
    if (prep < 0)
    {
      std::fprintf(stderr, "ALSA: %s prepare failed after xrun: %s\n", label, snd_strerror(prep));
      return false;
    }
    if (doLogState)
      log_pcm_state(pcm, label);
    return true;
  }

  int r = snd_pcm_recover(pcm, err, 1);
  if (r < 0)
  {
    std::fprintf(stderr, "ALSA: %s recover failed: %s\n", label, snd_strerror(r));
    return false;
  }

  snd_pcm_state_t st = snd_pcm_state(pcm);
  if (st == SND_PCM_STATE_PREPARED)
  {
    if (doLogState)
      log_pcm_state(pcm, label);
  }
  else if (st == SND_PCM_STATE_XRUN || st == SND_PCM_STATE_SUSPENDED)
  {
    const int p = snd_pcm_prepare(pcm);
    if (p < 0)
      std::fprintf(stderr, "ALSA: %s prepare failed after recover: %s\n", label, snd_strerror(p));
    else if (doLogState)
      log_pcm_state(pcm, label);
  }

  return true;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  logBuildBanner();

  tryEnableRealtime();
  configureDenormals();

  loadConfig();
  applyEnvOverrides();

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  const char *dev = std::getenv("ALSA_DEVICE");
  const char *devDefault = (dev && dev[0]) ? dev : "hw:0,0";

  const char *envCapDev = std::getenv("ALSA_CAPTURE_DEVICE");
  const char *envPbDev = std::getenv("ALSA_PLAYBACK_DEVICE");
  const char *capDevName = (envCapDev && envCapDev[0]) ? envCapDev : devDefault;
  const char *pbDevName = (envPbDev && envPbDev[0]) ? envPbDev : devDefault;

  std::fprintf(stderr, "ALSA: devices capture='%s' playback='%s'\n", capDevName, pbDevName);

  unsigned int rate = 48000;
  unsigned int captureChannels = 1;
  unsigned int playbackChannels = 2;
  snd_pcm_uframes_t periodSize = 128;
  unsigned int periods = 3;

  if (const char *envRate = std::getenv("ALSA_RATE"))
    rate = (unsigned int)std::max(8000, std::atoi(envRate));
  if (const char *envCh = std::getenv("ALSA_CHANNELS"))
  {
    const unsigned int ch = (unsigned int)std::max(1, std::atoi(envCh));
    captureChannels = ch;
    playbackChannels = ch;
  }
  if (const char *envCapCh = std::getenv("ALSA_CAPTURE_CHANNELS"))
    captureChannels = (unsigned int)std::max(1, std::atoi(envCapCh));
  if (const char *envPbCh = std::getenv("ALSA_PLAYBACK_CHANNELS"))
    playbackChannels = (unsigned int)std::max(1, std::atoi(envPbCh));
  if (const char *envPeriod = std::getenv("ALSA_PERIOD"))
    periodSize = (snd_pcm_uframes_t)std::max(16, std::atoi(envPeriod));
  if (const char *envPeriods = std::getenv("ALSA_PERIODS"))
    periods = (unsigned int)std::max(2, std::atoi(envPeriods));

  snd_pcm_t *cap = nullptr;
  snd_pcm_t *pb = nullptr;

  int err = snd_pcm_open(&cap, capDevName, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0)
  {
    std::fprintf(stderr, "ALSA: failed to open capture %s: %s\n", capDevName, snd_strerror(err));
    dump_alsa_device_hints();
    return 1;
  }
  err = snd_pcm_open(&pb, pbDevName, SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0)
  {
    std::fprintf(stderr, "ALSA: failed to open playback %s: %s\n", pbDevName, snd_strerror(err));
    dump_alsa_device_hints();
    snd_pcm_close(cap);
    return 1;
  }

  snd_pcm_uframes_t capBuffer = 0;
  snd_pcm_uframes_t pbBuffer = 0;

  snd_pcm_uframes_t capPeriod = periodSize;
  snd_pcm_uframes_t pbPeriod = periodSize;
  unsigned int capPeriods = periods;
  unsigned int pbPeriods = periods;

  unsigned int capRate = rate;
  if ((err = setup_pcm(cap, SND_PCM_STREAM_CAPTURE, capRate, captureChannels, capPeriod, capPeriods, capBuffer)) < 0)
  {
    std::fprintf(stderr, "ALSA: capture setup failed: %s\n", snd_strerror(err));
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }
  if ((err = setup_sw_params(cap, SND_PCM_STREAM_CAPTURE, capPeriod, capBuffer)) < 0)
  {
    std::fprintf(stderr, "ALSA: capture sw_params failed: %s\n", snd_strerror(err));
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }
  unsigned int pbRate = rate;
  if ((err = setup_pcm(pb, SND_PCM_STREAM_PLAYBACK, pbRate, playbackChannels, pbPeriod, pbPeriods, pbBuffer)) < 0)
  {
    std::fprintf(stderr, "ALSA: playback setup failed: %s\n", snd_strerror(err));
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }
  if ((err = setup_sw_params(pb, SND_PCM_STREAM_PLAYBACK, pbPeriod, pbBuffer)) < 0)
  {
    std::fprintf(stderr, "ALSA: playback sw_params failed: %s\n", snd_strerror(err));
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }

  // Log what ALSA actually negotiated (baseline invariant).
  logPcmNegotiated(cap, "capture");
  logPcmNegotiated(pb, "playback");

  if (capRate != pbRate)
  {
    std::fprintf(stderr, "ALSA: cap/pb rate mismatch (cap=%u pb=%u) — aborting.\n", capRate, pbRate);
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }
  rate = capRate;

  if (capPeriod != pbPeriod)
  {
    std::fprintf(stderr, "ALSA: capture/playback period mismatch (cap=%lu pb=%lu)\n",
                 (unsigned long)capPeriod, (unsigned long)pbPeriod);
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }
  periodSize = capPeriod;
  periods = capPeriods;

  if (capPeriods != pbPeriods)
  {
    std::fprintf(stderr, "ALSA: capture/playback periods mismatch (cap=%u pb=%u)\n",
                 capPeriods, pbPeriods);
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }

  // Link capture + playback to keep them in sync when possible (optional).
  const bool disableLink = std::getenv("ALSA_DISABLE_LINK") != nullptr;
  bool linkOk = false;
  if (!disableLink)
  {
    if ((err = snd_pcm_link(cap, pb)) < 0)
    {
      std::fprintf(stderr, "ALSA: snd_pcm_link failed (continuing): %s\n", snd_strerror(err));
      std::fprintf(stderr, "ALSA: proceeding in unlinked mode\n");
    }
    else
    {
      linkOk = true;
    }
  }
  else
  {
    std::printf("ALSA: link disabled via ALSA_DISABLE_LINK\n");
  }

  std::fprintf(stderr, "ALSA: snd_pcm_link attempted=%s ok=%s\n",
               disableLink ? "false" : "true",
               linkOk ? "true" : "false");

  if ((err = snd_pcm_prepare(cap)) < 0)
  {
    std::fprintf(stderr, "ALSA: capture prepare failed: %s\n", snd_strerror(err));
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }
  if ((err = snd_pcm_prepare(pb)) < 0)
  {
    std::fprintf(stderr, "ALSA: playback prepare failed: %s\n", snd_strerror(err));
    snd_pcm_close(cap);
    snd_pcm_close(pb);
    return 1;
  }

  // After prepare, query again (some devices finalize params here).
  logPcmNegotiated(cap, "capture(prepared)");
  logPcmNegotiated(pb, "playback(prepared)");

  // Build + activate the ordered chain once ALSA is configured.
  initChainRuntime(rate, (uint32_t)periodSize);

  std::thread ctl(udpControlThread);

  std::vector<int32_t> inI32((size_t)periodSize * captureChannels);
  std::vector<int32_t> outI32((size_t)periodSize * playbackChannels);
  std::vector<float> inMono((size_t)periodSize);
  std::vector<float> dspOut((size_t)periodSize);

  inputTrimLin.store(dbToLin(inputTrimDb.load()));

  outputGainLin.store(dbToLin(outputGainDb.load()));
  irGainLin.store(dbToLin(irGainDb.load()));

  std::printf("Runtime: inputTrimDb=%.1f dB passthrough=%s bypassNam=%s bypassIr=%s\n",
              inputTrimDb.load(),
              passthroughMode.load() ? "true" : "false",
              bypassNam.load() ? "true" : "false",
              bypassIr.load() ? "true" : "false");
  std::printf("Runtime: outputGainDb=%.1f dB\n", outputGainDb.load());
  std::printf("Runtime: irGainDb=%.1f dB\n", irGainDb.load());
  if (irUseTarget.load())
    std::printf("Runtime: irTargetDb=%.1f dB\n", irTargetDb.load());
  std::printf("Runtime: disableSoftclip=%s\n", disableSoftclip.load() ? "true" : "false");
  std::printf("Runtime: namUseInputLevel=%s namPreGainDb=%.1f dB\n",
              namUseInputLevel.load() ? "true" : "false",
              namPreGainDb.load());
  std::printf("Runtime: namPostGainDb=%.1f dB namInLimit=%.2f\n",
              namPostGainDb.load(),
              namInLimit.load());
  std::printf("Runtime: sanitizeOutput=%s verboseXruns=%s\n",
              sanitizeOutput.load() ? "true" : "false",
              verboseXruns.load() ? "true" : "false");
  std::printf("Runtime: logStats=%s\n", logStats.load() ? "true" : "false");

  // Optional NAM input/output dump for offline inspection.
  dumpNamInit(rate);
  dumpIrInit(rate);

  std::printf("ALSA DSP engine running. Capture=%s Playback=%s\n", capDevName, pbDevName);
  std::printf("Ctrl+C to stop.\n");

  // Prime playback with silence to reduce initial underruns.
  // Avoid priming the entire hardware buffer (can be large / slow). Default: prime ~1 buffer less one period.
  std::fill(outI32.begin(), outI32.end(), 0);
  const snd_pcm_uframes_t defaultPrime = periodSize * (snd_pcm_uframes_t)std::max(1u, pbPeriods - 1);
  const snd_pcm_uframes_t primeTarget = (snd_pcm_uframes_t)readEnvU32AllowZero("ALSA_PRIME_FRAMES", (uint32_t)defaultPrime);
  snd_pcm_uframes_t primed = 0;
  const snd_pcm_uframes_t primeLimit = std::min(pbBuffer, primeTarget);
  while (primed < primeLimit)
  {
    const snd_pcm_uframes_t chunk = std::min<snd_pcm_uframes_t>(periodSize, primeLimit - primed);
    snd_pcm_sframes_t w = snd_pcm_writei(pb, outI32.data(), chunk);
    if (w < 0)
    {
      w = snd_pcm_recover(pb, (int)w, 1);
      if (w < 0)
        break;
    }
    else
    {
      primed += (snd_pcm_uframes_t)w;
    }
  }

  // Let read/write start the streams to avoid start-state errors.

  uint64_t xrunsRead = 0;
  uint64_t xrunsWrite = 0;
  uint64_t nonFinite = 0;
  uint64_t shortRead = 0;
  uint64_t shortWrite = 0;
  auto lastReport = std::chrono::steady_clock::now();

  auto activeChain = std::atomic_load_explicit(&gChainState.activeChain, std::memory_order_acquire);

  // If we ever can't enqueue an old chain for background retirement, keep it here and retry.
  std::shared_ptr<pedal::dsp::SignalChain> deferredRetire;
  std::shared_ptr<pedal::dsp::SignalChain> deferredSwap;

  const double deadlineUs = ((double)periodSize * 1000000.0) / (double)rate;
  const uint64_t deadlineUsInt = (uint64_t)std::llround(deadlineUs);

  // Capture sanity check (first N seconds): detects “processing silence” false positives.
  const float silentPeakThresh = []() -> float
  {
    if (const char *e = std::getenv("ALSA_CAPTURE_SILENT_PEAK"))
      return (float)std::strtod(e, nullptr);
    return 1.0e-5f;
  }();
  const uint32_t sanitySecs = readEnvU32("ALSA_CAPTURE_SANITY_SECS", 2);
  uint64_t sanityFramesRemaining = (uint64_t)rate * (uint64_t)sanitySecs;
  uint64_t sanityFramesSeen = 0;
  double sanitySumSq = 0.0;
  float sanityPeak = 0.0f;
  bool sanityReported = false;

  const bool baselineCheck = (std::getenv("ALSA_BASELINE") != nullptr);
  const uint64_t baselineChainUsMax = (uint64_t)readEnvU32AllowZero("ALSA_BASELINE_CHAIN_US_MAX", 2000);

  enum class SwapRampState
  {
    Idle,
    FadeOut,
    FadeIn
  };

  // Optional click-safe swap smoothing (disabled by default).
  // Unlike the old crossfade implementation, this never processes two chains in the same audio period.
  const bool chainXfade = (std::getenv("ALSA_CHAIN_XFADE") != nullptr);
  const uint32_t swapRampSamples = chainXfade ? readEnvU32AllowZero("ALSA_SWAP_RAMP_SAMPLES", 32) : 0;
  SwapRampState swapState = SwapRampState::Idle;
  std::shared_ptr<pedal::dsp::SignalChain> swapNext;

  uint64_t chainSwapCount = 0;
  uint64_t chainProcCalls = 0;
  uint64_t chainProcSumUs = 0;
  uint64_t chainProcMaxUs = 0;
  uint64_t chainOverruns = 0;

  auto applyFadeOut = [&](float *buf, uint32_t frames, uint32_t ramp) noexcept
  {
    if (ramp == 0 || frames == 0)
      return;
    ramp = std::min(ramp, frames);
    if (ramp == 1)
    {
      buf[frames - 1] = 0.0f;
      return;
    }
    for (uint32_t i = 0; i < ramp; i++)
    {
      const float t = (float)i / (float)(ramp - 1); // 0..1
      const float g = 1.0f - t;                     // 1..0
      buf[frames - ramp + i] *= g;
    }
  };

  auto applyFadeIn = [&](float *buf, uint32_t frames, uint32_t ramp) noexcept
  {
    if (ramp == 0 || frames == 0)
      return;
    ramp = std::min(ramp, frames);
    if (ramp == 1)
    {
      buf[0] = 0.0f;
      return;
    }
    for (uint32_t i = 0; i < ramp; i++)
    {
      const float t = (float)i / (float)(ramp - 1); // 0..1
      buf[i] *= t;
    }
  };

  while (running.load())
  {
    // Try to retire any deferred old chain first; if this can't be done, avoid piling on more swaps.
    if (deferredRetire)
      (void)retireChainFromAudioThread(deferredRetire);

    uint32_t filled = 0;
    while (filled < periodSize && running.load())
    {
      snd_pcm_sframes_t r = snd_pcm_readi(
          cap,
          inI32.data() + (size_t)filled * captureChannels,
          periodSize - filled);
      if (r < 0)
      {
        xrunsRead++;
        if (verboseXruns.load())
          std::fprintf(stderr, "ALSA: capture read error: %s\n", snd_strerror((int)r));
        if (!recover_pcm(cap, "capture", (int)r))
        {
          std::fprintf(stderr, "ALSA: capture read error: %s\n", snd_strerror((int)r));
          running.store(false);
          break;
        }
        continue;
      }
      if (r == 0)
        continue;
      filled += (uint32_t)r;
    }
    if (!running.load())
      break;
    if (filled != periodSize)
    {
      shortRead++;
      continue;
    }

    const uint32_t nframes = (uint32_t)periodSize;
    const bool passthrough = passthroughMode.load(std::memory_order_relaxed);

    float pkIn = 0.0f;
    constexpr float invI32 = 1.0f / 2147483648.0f;
    if (captureChannels == 1)
    {
      for (uint32_t i = 0; i < nframes; i++)
      {
        const float mono = (float)inI32[i] * invI32;
        inMono[i] = mono;
        const float absVal = std::fabs(mono);
        if (absVal > pkIn)
          pkIn = absVal;

        if (sanityFramesRemaining > 0)
        {
          sanitySumSq += (double)mono * (double)mono;
          if (absVal > sanityPeak)
            sanityPeak = absVal;
          sanityFramesRemaining--;
          sanityFramesSeen++;
        }
      }
    }
    else
    {
      for (uint32_t i = 0; i < nframes; i++)
      {
        double acc = 0.0;
        for (unsigned int c = 0; c < captureChannels; c++)
        {
          const int32_t v = inI32[(size_t)i * captureChannels + c];
          acc += (double)v * (double)invI32;
        }
        const float mono = (captureChannels > 0) ? (float)(acc / (double)captureChannels) : 0.0f;
        inMono[i] = mono;
        const float absVal = std::fabs(mono);
        if (absVal > pkIn)
          pkIn = absVal;

        if (sanityFramesRemaining > 0)
        {
          sanitySumSq += (double)mono * (double)mono;
          if (absVal > sanityPeak)
            sanityPeak = absVal;
          sanityFramesRemaining--;
          sanityFramesSeen++;
        }
      }
    }

    if (!sanityReported && sanityFramesRemaining == 0)
    {
      sanityReported = true;
      const double rms = (sanityFramesSeen > 0) ? std::sqrt(sanitySumSq / (double)sanityFramesSeen) : 0.0;
      std::fprintf(stderr,
                   "ALSA: capture_sanity secs=%u frames=%llu peak=%.6g rms=%.6g\n",
                   sanitySecs,
                   (unsigned long long)sanityFramesSeen,
                   (double)sanityPeak,
                   rms);
      if (sanityPeak < silentPeakThresh)
      {
        std::fprintf(stderr,
                     "ALSA: WARNING: Capture appears silent — verify ALSA device routing (peak<%.3g).\n",
                     (double)silentPeakThresh);
      }
    }

    float currentPeak = peakInput.load(std::memory_order_relaxed);
    if (pkIn > currentPeak)
      peakInput.store(pkIn, std::memory_order_relaxed);

    // Pull any pending chain swap request at a safe boundary (period boundary).
    // If a previous swap request was deferred (retire queue full), keep retrying and coalesce to the latest.
    std::shared_ptr<pedal::dsp::SignalChain> pending;
    if (deferredSwap)
    {
      pending = deferredSwap;

      auto newer = std::atomic_exchange_explicit(
          &gChainState.pendingChain,
          std::shared_ptr<pedal::dsp::SignalChain>{},
          std::memory_order_acq_rel);
      if (newer)
      {
        deferredSwap = std::move(newer);
        pending = deferredSwap;
      }
    }
    else
    {
      pending = std::atomic_exchange_explicit(
          &gChainState.pendingChain,
          std::shared_ptr<pedal::dsp::SignalChain>{},
          std::memory_order_acq_rel);
    }

    // Coalesce: if multiple updates arrive between audio periods, apply only the latest.
    if (pending)
    {
      while (true)
      {
        auto newer = std::atomic_exchange_explicit(
            &gChainState.pendingChain,
            std::shared_ptr<pedal::dsp::SignalChain>{},
            std::memory_order_acq_rel);
        if (!newer)
          break;
        pending = std::move(newer);
      }
    }

    // If a swap is requested, either swap immediately (default) or do a click-safe 2-block ramp
    // without ever processing both chains in the same audio callback.
    if (pending)
    {
      const bool canSwapNow = (!activeChain) || (!deferredRetire && retireQueueHasSpace());
      if (!canSwapNow)
      {
        deferredSwap = pending;
      }
      else
      {
        deferredSwap.reset();

        if (!passthrough && swapRampSamples > 0 && activeChain)
        {
          swapNext = std::move(pending);
          if (swapState == SwapRampState::Idle)
            swapState = SwapRampState::FadeOut;
        }
        else
        {
          auto old = activeChain;
          activeChain = std::move(pending);
          std::atomic_store_explicit(&gChainState.activeChain, activeChain, std::memory_order_release);
          chainSwapCount++;

          deferredRetire = std::move(old);
          (void)retireChainFromAudioThread(deferredRetire);
        }
      }
    }

    const bool wantTiming = logTiming.load(std::memory_order_relaxed);

    if (!passthrough && activeChain)
    {
      std::chrono::steady_clock::time_point t0;
      if (wantTiming)
        t0 = std::chrono::steady_clock::now();

      activeChain->process(inMono.data(), dspOut.data(), nframes);

      if (wantTiming)
      {
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        chainProcCalls++;
        chainProcSumUs += us;
        if (us > chainProcMaxUs)
          chainProcMaxUs = us;
        if (deadlineUsInt > 0 && us > deadlineUsInt)
          chainOverruns++;
      }
    }
    else
    {
      std::memcpy(dspOut.data(), inMono.data(), sizeof(float) * nframes);
    }

    if (!passthrough && swapRampSamples > 0)
    {
      if (swapState == SwapRampState::FadeOut)
      {
        applyFadeOut(dspOut.data(), nframes, swapRampSamples);

        if (swapNext)
        {
          if (deferredRetire || !retireQueueHasSpace())
          {
            // Can't safely retire old chain right now; defer swap.
            deferredSwap = std::move(swapNext);
            swapState = SwapRampState::Idle;
          }
          else
          {
            auto old = activeChain;
            activeChain = std::move(swapNext);
            std::atomic_store_explicit(&gChainState.activeChain, activeChain, std::memory_order_release);
            chainSwapCount++;

            deferredRetire = std::move(old);
            (void)retireChainFromAudioThread(deferredRetire);
            swapState = SwapRampState::FadeIn;
          }
        }
      }
      else if (swapState == SwapRampState::FadeIn)
      {
        applyFadeIn(dspOut.data(), nframes, swapRampSamples);
        swapState = SwapRampState::Idle;
      }
    }

    // Stage metering: v1 uses chain output as "IR" peak placeholder.
    float pkChain = 0.0f;
    for (uint32_t i = 0; i < nframes; i++)
    {
      const float absVal = std::fabs(dspOut[i]);
      if (absVal > pkChain)
        pkChain = absVal;
    }
    float currentIrPeak = peakIrOut.load(std::memory_order_relaxed);
    if (pkChain > currentIrPeak)
      peakIrOut.store(pkChain, std::memory_order_relaxed);
    float currentNamPeak = peakNamOut.load(std::memory_order_relaxed);
    if (pkChain > currentNamPeak)
      peakNamOut.store(pkChain, std::memory_order_relaxed);

    float pkOut = 0.0f;
    const float outG = outputGainLin.load(std::memory_order_relaxed);
    const bool doSan = sanitizeOutput.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < nframes; i++)
    {
      const float s = dspOut[i] * outG;
      float outS = s;
      if (doSan && !std::isfinite(outS))
      {
        outS = 0.0f;
        nonFinite++;
      }
      const float absVal = std::fabs(outS);
      if (absVal > pkOut)
        pkOut = absVal;

      const float clamped = clampf(outS, -1.0f, 1.0f);
      const int32_t v = (int32_t)std::lrintf(clamped * 2147483647.0f);
      for (unsigned int c = 0; c < playbackChannels; c++)
        outI32[(size_t)i * playbackChannels + c] = v;
    }

    float currentOutPeak = peakFinalOut.load(std::memory_order_relaxed);
    if (pkOut > currentOutPeak)
      peakFinalOut.store(pkOut, std::memory_order_relaxed);

    uint32_t written = 0;
    while (written < nframes && running.load())
    {
      snd_pcm_sframes_t w = snd_pcm_writei(
          pb,
          outI32.data() + (size_t)written * playbackChannels,
          nframes - written);
      if (w < 0)
      {
        xrunsWrite++;
        if (verboseXruns.load())
          std::fprintf(stderr, "ALSA: playback write error: %s\n", snd_strerror((int)w));
        if (!recover_pcm(pb, "playback", (int)w))
        {
          std::fprintf(stderr, "ALSA: playback write error: %s\n", snd_strerror((int)w));
          running.store(false);
          break;
        }
        continue;
      }
      if (w == 0)
        continue;
      written += (uint32_t)w;
    }
    if (!running.load())
      break;
    if (written != nframes)
      shortWrite++;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport > std::chrono::seconds(2))
    {
      if (logStats.load() || xrunsRead || xrunsWrite || nonFinite || shortRead || shortWrite)
      {
        if (logTiming.load(std::memory_order_relaxed))
        {
          const double chainAvgUs = (chainProcCalls > 0) ? ((double)chainProcSumUs / (double)chainProcCalls) : 0.0;
          const double chainPct = (deadlineUs > 0.0) ? ((double)chainProcMaxUs * 100.0 / deadlineUs) : 0.0;

          std::fprintf(stderr,
                       "ALSA: xruns(read=%llu write=%llu) short(read=%llu write=%llu) nonFinite=%llu swaps=%llu nframes=%u peakIn=%.3f peakNam=%.3f peakIr=%.3f peakOut=%.3f chain_us_avg=%.1f chain_us_max=%llu deadline_us=%.1f chain_max_pct=%.1f chain_overruns=%llu retireQ_full=%llu\n",
                       (unsigned long long)xrunsRead,
                       (unsigned long long)xrunsWrite,
                       (unsigned long long)shortRead,
                       (unsigned long long)shortWrite,
                       (unsigned long long)nonFinite,
                       (unsigned long long)chainSwapCount,
                       nframes,
                       (double)peakInput.load(),
                       (double)peakNamOut.load(),
                       (double)peakIrOut.load(),
                       (double)peakFinalOut.load(),
                       chainAvgUs,
                       (unsigned long long)chainProcMaxUs,
                       deadlineUs,
                       chainPct,
                       (unsigned long long)chainOverruns,
                       (unsigned long long)gRetireQueueFull.load(std::memory_order_relaxed));

          if (baselineCheck)
          {
            const bool okXruns = (xrunsRead == 0 && xrunsWrite == 0);
            const bool okOverruns = (chainOverruns == 0);
            const bool okMax = (baselineChainUsMax == 0) ? true : (chainProcMaxUs < baselineChainUsMax);
            const bool okCapture = sanityReported ? (sanityPeak >= silentPeakThresh) : true;
            std::fprintf(stderr,
                         "ALSA: baseline_check ok=%s xruns_ok=%s overruns_ok=%s chain_us_max_ok=%s capture_ok=%s (chain_us_max=%llu thresh=%llu)\n",
                         (okXruns && okOverruns && okMax && okCapture) ? "true" : "false",
                         okXruns ? "true" : "false",
                         okOverruns ? "true" : "false",
                         okMax ? "true" : "false",
                         okCapture ? "true" : "false",
                         (unsigned long long)chainProcMaxUs,
                         (unsigned long long)baselineChainUsMax);
          }

          // Optional per-node-type timing (debug). Printed as a single compact line.
          if (activeChain && activeChain->nodeTimingEnabled())
          {
            pedal::dsp::SignalChain::NodeTimingStat stats[16];
            const size_t n = activeChain->snapshotNodeTiming(stats, 16, true /*reset*/);
            if (n > 0)
            {
              std::fprintf(stderr, "ALSA: node_us_max");
              for (size_t i = 0; i < n; i++)
              {
                const char *t = stats[i].type ? stats[i].type : "?";
                std::fprintf(stderr, " %s=%llu", t, (unsigned long long)stats[i].maxUs);
              }
              std::fprintf(stderr, "\n");
            }
          }
        }
        else
        {
          std::fprintf(stderr,
                       "ALSA: xruns(read=%llu write=%llu) short(read=%llu write=%llu) nonFinite=%llu swaps=%llu nframes=%u peakIn=%.3f peakNam=%.3f peakIr=%.3f peakOut=%.3f\n",
                       (unsigned long long)xrunsRead,
                       (unsigned long long)xrunsWrite,
                       (unsigned long long)shortRead,
                       (unsigned long long)shortWrite,
                       (unsigned long long)nonFinite,
                       (unsigned long long)chainSwapCount,
                       nframes,
                       (double)peakInput.load(),
                       (double)peakNamOut.load(),
                       (double)peakIrOut.load(),
                       (double)peakFinalOut.load());
        }
      }

      chainSwapCount = 0;
      chainProcCalls = 0;
      chainProcSumUs = 0;
      chainProcMaxUs = 0;
      chainOverruns = 0;

      peakInput.store(0.0f, std::memory_order_relaxed);
      peakNamOut.store(0.0f, std::memory_order_relaxed);
      peakIrOut.store(0.0f, std::memory_order_relaxed);
      peakFinalOut.store(0.0f, std::memory_order_relaxed);
      lastReport = now;
    }
  }

  running.store(false);

  gChainState.running.store(false, std::memory_order_relaxed);
  if (gControlThread.joinable())
    gControlThread.join();

  if (ctl.joinable())
    ctl.join();

  gRetireRunning.store(false, std::memory_order_relaxed);
  if (gRetireThread.joinable())
    gRetireThread.join();

  dumpNamFlush(rate);
  dumpIrFlush(rate);

  snd_pcm_close(cap);
  snd_pcm_close(pb);

  return 0;
}
