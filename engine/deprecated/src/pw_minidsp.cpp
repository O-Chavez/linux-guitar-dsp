#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// Minimal DSP engine:
//  - one PipeWire stream
//  - one callback
//  - downmix stereo -> mono
//  - apply simple gain + safety limiter
//  - upmix mono -> stereo
//
// This is intentionally tiny and boring to validate transport/clocking.

static std::atomic<bool> gRunning{true};
static std::atomic<uint32_t> gSampleRate{48000};
static std::atomic<uint32_t> gQuantum{128};

static std::atomic<uint64_t> gCbCount{0};
static std::atomic<uint64_t> gFrameCount{0};

static pw_thread_loop *gLoop = nullptr;
static pw_context *gContext = nullptr;
static pw_core *gCore = nullptr;
static pw_stream *gStream = nullptr;

static spa_hook gStreamListener;

static std::atomic<bool> gPrintedFormat{false};
static std::atomic<uint32_t> gNodeId{SPA_ID_INVALID};
static const char *gTargetPlayback = nullptr;

static inline float clampf(float x, float lo, float hi)
{
  return (x < lo) ? lo : (x > hi) ? hi
                                  : x;
}

static void onSignal(int)
{
  gRunning.store(false, std::memory_order_relaxed);
}

struct AudioView
{
  // Output
  bool outPlanar = false;
  uint8_t *outBase0 = nullptr;
  uint8_t *outBase1 = nullptr;
  uint32_t outStride0 = 0;
  uint32_t outStride1 = 0;
  uint32_t outFrames = 0;
};

static bool computeOutputView(spa_buffer *outSpa, uint32_t channels, uint32_t fallbackFrames, AudioView &v)
{
  if (!outSpa || outSpa->n_datas < 1)
    return false;

  // ----- Output -----
  v.outPlanar = (outSpa->n_datas >= channels);
  spa_data *out0 = (spa_data *)&outSpa->datas[0];
  spa_data *out1 = v.outPlanar ? (spa_data *)&outSpa->datas[1] : (spa_data *)&outSpa->datas[0];
  if (!out0->data || !out0->chunk)
    return false;
  if (v.outPlanar && (!out1->data || !out1->chunk))
    v.outPlanar = false;

  const uint32_t outDefaultStride = v.outPlanar ? (uint32_t)sizeof(float) : (uint32_t)(channels * sizeof(float));
  v.outStride0 = (out0->chunk->stride != 0) ? (uint32_t)out0->chunk->stride : outDefaultStride;
  v.outStride1 = v.outPlanar ? ((out1->chunk->stride != 0) ? (uint32_t)out1->chunk->stride : (uint32_t)sizeof(float)) : v.outStride0;
  if (v.outStride0 < outDefaultStride)
    return false;

  v.outBase0 = (uint8_t *)out0->data + out0->chunk->offset;
  v.outBase1 = (uint8_t *)out1->data + out1->chunk->offset;

  v.outFrames = (v.outStride0 > 0) ? (uint32_t)(out0->chunk->size / v.outStride0) : 0u;

  // Fallbacks when chunk sizes aren't yet meaningful.
  if (v.outFrames == 0)
    v.outFrames = fallbackFrames;

  return true;
}

static void on_stream_param_changed(void *, uint32_t id, const struct spa_pod *param)
{
  if (!param || id != SPA_PARAM_Format)
    return;

  struct spa_audio_info_raw info;
  std::memset(&info, 0, sizeof(info));
  if (spa_format_audio_raw_parse(param, &info) < 0)
    return;

  if (!gPrintedFormat.exchange(true, std::memory_order_relaxed))
  {
    std::fprintf(stderr, "[minidsp] negotiated format: rate=%u channels=%u format=%d\n",
                 info.rate, info.channels, (int)info.format);
  }
  if (info.rate)
    gSampleRate.store(info.rate, std::memory_order_relaxed);
}

static void on_stream_state_changed(void *, enum pw_stream_state oldS, enum pw_stream_state newS, const char *err)
{
  if (newS == PW_STREAM_STATE_STREAMING)
  {
    const uint32_t nid = pw_stream_get_node_id(gStream);
    gNodeId.store(nid, std::memory_order_relaxed);
  }
  std::fprintf(stderr, "[minidsp] state %d -> %d%s%s (target=%s node_id=%u)\n",
               (int)oldS, (int)newS,
               err ? ": " : "", err ? err : "",
               (gTargetPlayback && gTargetPlayback[0]) ? gTargetPlayback : "(unset)",
               gNodeId.load(std::memory_order_relaxed));
}

static void on_process(void *)
{
  if (!gStream)
    return;

  // One stream == one buffer queue. Dequeue exactly one buffer.
  // If capture isn't linked/available, PipeWire will typically provide silence (or no callbacks).
  struct pw_buffer *buf = pw_stream_dequeue_buffer(gStream);
  if (!buf)
    return;

  // Scheduling/clocking validation mode: OUTPUT-only stream.
  // Do not attempt to read capture here (duplex semantics/policy differ across graphs).
  spa_buffer *outSpa = buf->buffer;

  constexpr uint32_t channels = 2;
  AudioView v;
  if (!computeOutputView(outSpa, channels, gQuantum.load(std::memory_order_relaxed), v))
  {
    // Just clear output defensively.
    const uint32_t stride = (outSpa && outSpa->n_datas >= 2) ? (uint32_t)sizeof(float) : (uint32_t)(2 * sizeof(float));
    const uint32_t nframes = gQuantum.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < outSpa->n_datas; i++)
    {
      if (!outSpa->datas[i].data || !outSpa->datas[i].chunk)
        continue;
      std::memset((uint8_t *)outSpa->datas[i].data + outSpa->datas[i].chunk->offset, 0, nframes * stride);
      outSpa->datas[i].chunk->size = nframes * stride;
      if (outSpa->datas[i].chunk->stride == 0)
        outSpa->datas[i].chunk->stride = stride;
    }
    pw_stream_queue_buffer(gStream, buf);
    return;
  }

  const uint32_t nframes = v.outFrames;
  // Sine generator (or silence) so cb/s and scheduling are unambiguous.
  // Env:
  //   MINIDSP_SINE_HZ=440   # set <=0 for silence
  //   MINIDSP_GAIN=-12      # dBFS gain
  float hz = 440.0f;
  if (const char *e = std::getenv("MINIDSP_SINE_HZ"))
    hz = (float)std::atof(e);
  float gain = 1.0f;
  if (const char *e = std::getenv("MINIDSP_GAIN"))
    gain = std::pow(10.0f, (float)std::atof(e) / 20.0f);

  const float lim = 0.95f;
  static thread_local double phase = 0.0;
  const double sr = (double)gSampleRate.load(std::memory_order_relaxed);
  const double w = (sr > 0.0) ? (2.0 * M_PI * (double)hz / sr) : 0.0;

  // Write mono → stereo
  if (v.outPlanar)
  {
    for (uint32_t i = 0; i < nframes; i++)
    {
      float s = 0.0f;
      if (hz > 0.0f)
      {
        s = (float)std::sin(phase);
        phase += w;
        if (phase > 2.0 * M_PI)
          phase -= 2.0 * M_PI;
      }
      s *= gain;
      s = clampf(s, -lim, lim);
      *(float *)(v.outBase0 + (size_t)i * v.outStride0) = s;
      *(float *)(v.outBase1 + (size_t)i * v.outStride1) = s;
    }
    outSpa->datas[0].chunk->size = nframes * v.outStride0;
    outSpa->datas[1].chunk->size = nframes * v.outStride1;
    if (outSpa->datas[0].chunk->stride == 0)
      outSpa->datas[0].chunk->stride = v.outStride0;
    if (outSpa->datas[1].chunk->stride == 0)
      outSpa->datas[1].chunk->stride = v.outStride1;
  }
  else
  {
    for (uint32_t i = 0; i < nframes; i++)
    {
      uint8_t *frame = v.outBase0 + (size_t)i * v.outStride0;
      float s = 0.0f;
      if (hz > 0.0f)
      {
        s = (float)std::sin(phase);
        phase += w;
        if (phase > 2.0 * M_PI)
          phase -= 2.0 * M_PI;
      }
      s *= gain;
      s = clampf(s, -lim, lim);
      *(float *)(frame + 0) = s;
      *(float *)(frame + sizeof(float)) = s;
    }
    outSpa->datas[0].chunk->size = nframes * v.outStride0;
    if (outSpa->datas[0].chunk->stride == 0)
      outSpa->datas[0].chunk->stride = v.outStride0;
  }

  gCbCount.fetch_add(1, std::memory_order_relaxed);
  gFrameCount.fetch_add(nframes, std::memory_order_relaxed);

  pw_stream_queue_buffer(gStream, buf);
}

static const pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_process,
};

int main(int argc, char *argv[])
{
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  pw_init(&argc, &argv);

  gLoop = pw_thread_loop_new("dsp_engine_min", nullptr);
  if (!gLoop)
  {
    std::fprintf(stderr, "[minidsp] failed to create thread loop\n");
    return 1;
  }
  pw_thread_loop_start(gLoop);
  pw_thread_loop_lock(gLoop);

  gContext = pw_context_new(pw_thread_loop_get_loop(gLoop), nullptr, 0);
  if (!gContext)
  {
    std::fprintf(stderr, "[minidsp] failed to create context\n");
    pw_thread_loop_unlock(gLoop);
    return 1;
  }

  gCore = pw_context_connect(gContext, nullptr, 0);
  if (!gCore)
  {
    std::fprintf(stderr, "[minidsp] failed to connect core\n");
    pw_thread_loop_unlock(gLoop);
    return 1;
  }

  const char *target = std::getenv("PW_TARGET_PLAYBACK");
  gTargetPlayback = target;
  const bool forceTarget = (std::getenv("PW_MINIDSP_FORCE_TARGET") != nullptr);

  char wantLatency[64];
  std::snprintf(wantLatency, sizeof(wantLatency), "%u/%u", gQuantum.load(), gSampleRate.load());

  pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      // Keep as a normal playback/client stream.
      PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "Music",
      // This is an *app playback stream*, not a device sink.
      // Advertising as a Sink can confuse policy (sinks don't connect to sinks).
      // Use Audio/Stream so policy treats it as a normal playback stream.
      PW_KEY_MEDIA_CLASS, "Audio/Stream",
      PW_KEY_APP_NAME, "dsp_engine_min",
      PW_KEY_NODE_NAME, "dsp_engine_min.stream",
      PW_KEY_NODE_DESCRIPTION, "Minimal DSP (scheduling probe)",
      nullptr);

  // Don't hard-target by default: let policy route us like a normal app.
  // If you want to force-routing to a specific sink node.name, set:
  //   PW_MINIDSP_FORCE_TARGET=1 PW_TARGET_PLAYBACK=<node.name>
  if (forceTarget && target && target[0])
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);

  pw_properties_set(props, PW_KEY_NODE_LATENCY, wantLatency);
  pw_properties_setf(props, PW_KEY_NODE_FORCE_RATE, "%u", gSampleRate.load());
  pw_properties_setf(props, PW_KEY_NODE_FORCE_QUANTUM, "%u", gQuantum.load());

  gStream = pw_stream_new(gCore, "dsp_engine_min", props);
  if (!gStream)
  {
    std::fprintf(stderr, "[minidsp] failed to create stream\n");
    pw_thread_loop_unlock(gLoop);
    return 1;
  }

  std::memset(&gStreamListener, 0, sizeof(gStreamListener));
  pw_stream_add_listener(gStream, &gStreamListener, &stream_events, nullptr);

  uint8_t bufFmt[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(bufFmt, sizeof(bufFmt));
  spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
          .format = SPA_AUDIO_FORMAT_F32,
          .rate = gSampleRate.load(),
          .channels = 2);
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;

  const spa_pod *params[1];
  params[0] = (const spa_pod *)spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  // NOTE: If you ever see ports named capture_* for this stream, it means the
  // graph/policy is treating it like a capture stream. We want a pure playback
  // app stream here so ports should be output_* (or monitor_*) and direction
  // should be OUTPUT.
  pw_stream_connect(gStream,
                    // Playback client produces audio into the graph.
                    PW_DIRECTION_OUTPUT,
                    PW_ID_ANY,
                    (pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_AUTOCONNECT |
                                      PW_STREAM_FLAG_EARLY_PROCESS |
                                      PW_STREAM_FLAG_RT_PROCESS),
                    params, 1);

  pw_thread_loop_unlock(gLoop);

  std::fprintf(stderr, "[minidsp] running. env MINIDSP_GAIN=<dB> MINIDSP_SINE_HZ=<Hz>\n");
  std::fprintf(stderr, "[minidsp] optional: PW_MINIDSP_FORCE_TARGET=1 PW_TARGET_PLAYBACK=<node.name>\n");

  auto last = std::chrono::steady_clock::now();
  while (gRunning.load(std::memory_order_relaxed))
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto now = std::chrono::steady_clock::now();
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    if (dt >= 1000)
    {
      last = now;
      const uint64_t cb = gCbCount.exchange(0);
      const uint64_t fr = gFrameCount.exchange(0);
      std::fprintf(stderr, "[minidsp] cb/s=%llu frames/s=%llu\n",
                   (unsigned long long)cb,
                   (unsigned long long)fr);
    }
  }

  pw_thread_loop_lock(gLoop);
  if (gStream)
    pw_stream_destroy(gStream);
  gStream = nullptr;
  if (gCore)
    pw_core_disconnect(gCore);
  gCore = nullptr;
  if (gContext)
    pw_context_destroy(gContext);
  gContext = nullptr;
  pw_thread_loop_unlock(gLoop);

  pw_thread_loop_stop(gLoop);
  pw_thread_loop_destroy(gLoop);
  gLoop = nullptr;

  pw_deinit();
  return 0;
}
