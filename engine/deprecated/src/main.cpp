#include <jack/jack.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <cmath>
#include <fstream>
#include <filesystem>

#include "json.hpp"
#include "get_dsp.h"

#include "ir_loader.h"
#include "fft_convolver.h"

// UDP
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static jack_client_t *client = nullptr;
static jack_port_t *inPort = nullptr;
static jack_port_t *outL = nullptr;
static jack_port_t *outR = nullptr;

static std::atomic<bool> running{true};
static std::atomic<bool> debugOnce{false};

// Debug/passthrough mode
static std::atomic<bool> passthroughMode{false};

// Peak metering (updated in RT callback, read by meter thread)
static std::atomic<float> peakInput{0.0f};
static std::atomic<float> peakNamOut{0.0f};
static std::atomic<float> peakIrOut{0.0f};
static std::atomic<float> peakFinalOut{0.0f};

// Config
static std::atomic<float> inputTrimDb{0.0f};
static std::atomic<float> inputTrimLin{1.0f};
static std::string namModelPath;
static std::string irPath;

// NAM
static std::unique_ptr<nam::DSP> gModel;
static std::atomic<bool> modelReady{false};
static std::vector<float> namIn;
static std::vector<float> namOut;

// IR
static FFTConvolverPartitioned gIR;
static std::atomic<bool> irReady{false};
static std::vector<float> irBlockIn;
static std::vector<float> irBlockOut;

static inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
static inline float clampf(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi
                                                                                          : v; }

static void onSignal(int) { running.store(false); }

// -------------------- Config --------------------
static void loadConfig()
{
  const char *path = "/opt/pedal/config/chain.json";
  try
  {
    std::ifstream f(path);
    if (!f.is_open())
    {
      std::printf("Config: could not open %s (using defaults)\n", path);
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

    std::printf("Config: inputTrimDb=%.1f dB\n", inputTrimDb.load());
    std::printf("Config: namModelPath=%s\n", namModelPath.empty() ? "(empty)" : namModelPath.c_str());
    std::printf("Config: irPath=%s\n", irPath.empty() ? "(empty)" : irPath.c_str());
    std::printf("Config: passthrough=%s\n", passthroughMode.load() ? "ENABLED" : "disabled");
  }
  catch (const std::exception &e)
  {
    std::printf("Config: parse error: %s\n", e.what());
  }
}

// -------------------- Peak Meter Thread --------------------
static void meterThread()
{
  while (running.load(std::memory_order_relaxed))
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    float pkIn = peakInput.exchange(0.0f, std::memory_order_relaxed);
    float pkNam = peakNamOut.exchange(0.0f, std::memory_order_relaxed);
    float pkIr = peakIrOut.exchange(0.0f, std::memory_order_relaxed);
    float pkOut = peakFinalOut.exchange(0.0f, std::memory_order_relaxed);

    // Convert to dBFS for readability
    auto toDb = [](float peak) -> float
    {
      if (peak < 0.000001f)
        return -120.0f;
      return 20.0f * std::log10(peak);
    };

    std::printf("[METER] Input: %6.1f dBFS | NAM: %6.1f dBFS | IR: %6.1f dBFS | Out: %6.1f dBFS%s\n",
                toDb(pkIn), toDb(pkNam), toDb(pkIr), toDb(pkOut),
                passthroughMode.load() ? " [PASSTHROUGH]" : "");
    std::fflush(stdout);
  }
}

// -------------------- Auto-connect --------------------
static void tryConnect(const char *src, const char *dst)
{
  int r = jack_connect(client, src, dst);
  if (r == 0)
  {
    std::printf("AutoConnect: %s -> %s\n", src, dst);
  }
  else if (r == EEXIST)
  {
    std::printf("AutoConnect: already connected: %s -> %s\n", src, dst);
  }
  else
  {
    std::printf("AutoConnect: FAILED (%d): %s -> %s\n", r, src, dst);
  }
  std::fflush(stdout);
}

static void autoWire()
{
  std::printf("AutoWire: Using pw-link for native PipeWire connections...\n");
  std::fflush(stdout);

  // Give JACK/PipeWire a moment to register our ports
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Use pw-link to connect directly to PipeWire nodes, bypassing pw-jack translation
  // This works around the pw-jack input data passthrough bug

  // Connect iRig input to our input port
  std::printf("Connecting input...\n");
  int r1 = system("pw-link 'alsa_input.usb-IK_Multimedia_iRig_HD_X_1001073-02.mono-fallback:capture_MONO' 'dsp_engine_v1:in_mono' 2>&1");
  std::printf("  Input connection result: %d\n", r1);

  // Connect our outputs to iRig playback
  std::printf("Connecting outputs...\n");
  int r2 = system("pw-link 'dsp_engine_v1:out_L' 'alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FL' 2>&1");
  int r3 = system("pw-link 'dsp_engine_v1:out_R' 'alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FR' 2>&1");
  std::printf("  Output L connection result: %d\n", r2);
  std::printf("  Output R connection result: %d\n", r3);

  std::fflush(stdout);
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

// -------------------- Audio callback --------------------
int process(jack_nframes_t nframes, void *)
{
  if (!inPort || !outL || !outR)
    return 0;

  auto *in = (jack_default_audio_sample_t *)jack_port_get_buffer(inPort, nframes);
  auto *oL = (jack_default_audio_sample_t *)jack_port_get_buffer(outL, nframes);
  auto *oR = (jack_default_audio_sample_t *)jack_port_get_buffer(outR, nframes);

  const float g = inputTrimLin.load(std::memory_order_relaxed);
  const bool passthrough = passthroughMode.load(std::memory_order_relaxed);

  // Make sure our buffers exist (defensive)
  if (namIn.size() < nframes)
    namIn.resize(nframes);
  if (namOut.size() < nframes)
    namOut.resize(nframes);
  if (irBlockIn.size() < nframes)
    irBlockIn.resize(nframes);
  if (irBlockOut.size() < nframes)
    irBlockOut.resize(nframes);

  // Calculate input peak for metering
  float pkIn = 0.0f;
  for (jack_nframes_t i = 0; i < nframes; i++)
  {
    float absVal = std::fabs(in[i]);
    if (absVal > pkIn)
      pkIn = absVal;
  }

  // Update peak atomically (relaxed is fine for metering)
  float currentPeak = peakInput.load(std::memory_order_relaxed);
  if (pkIn > currentPeak)
    peakInput.store(pkIn, std::memory_order_relaxed);

  // PASSTHROUGH MODE: Just apply gain and bypass all processing
  if (passthrough)
  {
    for (jack_nframes_t i = 0; i < nframes; i++)
    {
      float sample = in[i] * g;
      oL[i] = sample;
      oR[i] = sample;
    }

    // Update final output peak
    float pkOut = pkIn * g;
    float currentOutPeak = peakFinalOut.load(std::memory_order_relaxed);
    if (pkOut > currentOutPeak)
      peakFinalOut.store(pkOut, std::memory_order_relaxed);

    return 0;
  }

  // NORMAL MODE: Full DSP chain
  // 1) input trim + softclip into namIn
  for (jack_nframes_t i = 0; i < nframes; i++)
  {
    namIn[i] = std::tanh(in[i] * g);
  }

  // 2) NAM (or bypass)
  const bool namOk = modelReady.load(std::memory_order_acquire) && (gModel != nullptr);
  if (namOk)
  {
    try
    {
      gModel->process(namIn.data(), namOut.data(), (int)nframes);
    }
    catch (const std::exception &e)
    {
      if (!debugOnce.exchange(true))
      {
        std::fprintf(stderr, "ERROR: NAM process threw exception: %s\n", e.what());
        std::fflush(stderr);
      }
      // Bypass on error
      std::memcpy(namOut.data(), namIn.data(), sizeof(float) * nframes);
    }
    catch (...)
    {
      if (!debugOnce.exchange(true))
      {
        std::fprintf(stderr, "ERROR: NAM process threw unknown exception\n");
        std::fflush(stderr);
      }
      // Bypass on error
      std::memcpy(namOut.data(), namIn.data(), sizeof(float) * nframes);
    }
  }
  else
  {
    std::memcpy(namOut.data(), namIn.data(), sizeof(float) * nframes);
  }

  // NAM output peak
  float pkNam = 0.0f;
  for (jack_nframes_t i = 0; i < nframes; i++)
  {
    float absVal = std::fabs(namOut[i]);
    if (absVal > pkNam)
      pkNam = absVal;
  }
  float currentNamPeak = peakNamOut.load(std::memory_order_relaxed);
  if (pkNam > currentNamPeak)
    peakNamOut.store(pkNam, std::memory_order_relaxed);

  // 3) IR convolution
  const bool irOk = irReady.load(std::memory_order_acquire) && gIR.ready();
  if (irOk)
  {
    bool success = gIR.processBlock(namOut.data(), irBlockOut.data(), (int)nframes);
    if (!success)
    {
      // IR failed, bypass
      std::memcpy(irBlockOut.data(), namOut.data(), sizeof(float) * nframes);
    }
  }
  else
  {
    // No IR, pass NAM output
    std::memcpy(irBlockOut.data(), namOut.data(), sizeof(float) * nframes);
  }

  // IR output peak
  float pkIr = 0.0f;
  for (jack_nframes_t i = 0; i < nframes; i++)
  {
    float absVal = std::fabs(irBlockOut[i]);
    if (absVal > pkIr)
      pkIr = absVal;
  }
  float currentIrPeak = peakIrOut.load(std::memory_order_relaxed);
  if (pkIr > currentIrPeak)
    peakIrOut.store(pkIr, std::memory_order_relaxed);

  // 4) Output final processed signal
  float pkOut = 0.0f;
  for (jack_nframes_t i = 0; i < nframes; i++)
  {
    oL[i] = irBlockOut[i];
    oR[i] = irBlockOut[i];
    float absVal = std::fabs(irBlockOut[i]);
    if (absVal > pkOut)
      pkOut = absVal;
  }
  float currentOutPeak = peakFinalOut.load(std::memory_order_relaxed);
  if (pkOut > currentOutPeak)
    peakFinalOut.store(pkOut, std::memory_order_relaxed);

  return 0;
}

// -------------------- Main --------------------
int main()
{
  loadConfig();

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  jack_status_t status;
  client = jack_client_open("dsp_engine_v1", JackNullOption, &status);
  if (!client)
  {
    std::fprintf(stderr, "Failed to open JACK client (status=%u)\n", status);
    return 1;
  }

  const jack_nframes_t sr = jack_get_sample_rate(client);
  const jack_nframes_t bs = jack_get_buffer_size(client);
  std::printf("Audio: sample_rate=%u buffer_size=%u\n", (unsigned)sr, (unsigned)bs);

  // Pre-size buffers to block size
  namIn.resize((size_t)bs);
  namOut.resize((size_t)bs);
  irBlockIn.resize((size_t)bs);
  irBlockOut.resize((size_t)bs);

  // Load NAM model (non-realtime)
  if (!namModelPath.empty())
  {
    try
    {
      std::filesystem::path p(namModelPath);
      if (!std::filesystem::exists(p))
      {
        std::printf("NAM: model path does not exist: %s (bypassing)\n", namModelPath.c_str());
      }
      else
      {
        gModel = nam::get_dsp(p);
        if (gModel)
        {
          std::printf("NAM: loaded model file: %s\n", namModelPath.c_str());
          gModel->ResetAndPrewarm((double)sr, (int)bs);
          modelReady.store(true, std::memory_order_release);
          std::printf("NAM: ready (expected_sr=%.1f)\n", gModel->GetExpectedSampleRate());
        }
        else
        {
          std::printf("NAM: get_dsp returned null (bypassing)\n");
        }
      }
    }
    catch (const std::exception &e)
    {
      std::printf("NAM: load error: %s (bypassing)\n", e.what());
      gModel.reset();
      modelReady.store(false, std::memory_order_release);
    }
  }
  else
  {
    std::printf("NAM: no model configured (bypassing)\n");
  }

  // Load IR (non-realtime)
  if (!irPath.empty())
  {
    IRData ir{};
    std::string err;
    if (!load_ir_mono(irPath, ir, err))
    {
      std::printf("IR: load failed: %s (bypassing)\n", err.c_str());
    }
    else if (ir.sampleRate != (int)sr)
    {
      std::fprintf(stderr, "IR: SAMPLE RATE MISMATCH - IR file is %d Hz but JACK is running at %u Hz.\n",
                   ir.sampleRate, (unsigned)sr);
      std::fprintf(stderr, "IR: Please resample your IR file to %u Hz or adjust JACK sample rate. (BYPASSING IR)\n",
                   (unsigned)sr);
    }
    else if (!gIR.init(ir.mono, (int)bs))
    {
      std::printf("IR: convolver init failed (bypassing)\n");
    }
    else
    {
      irReady.store(true, std::memory_order_release);
      const int parts = (int)((ir.mono.size() + (size_t)bs - 1) / (size_t)bs);
      std::printf("IR: ready (len=%zu samples, partitions=%d)\n", ir.mono.size(), parts);
    }
  }
  else
  {
    std::printf("IR: no cab IR configured (bypassing)\n");
  }

  // JACK ports + callback
  if (jack_set_process_callback(client, process, nullptr) != 0)
  {
    std::fprintf(stderr, "Failed to set JACK process callback\n");
    jack_client_close(client);
    return 1;
  }

  inPort = jack_port_register(client, "in_mono", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
  outL = jack_port_register(client, "out_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  outR = jack_port_register(client, "out_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

  if (!inPort || !outL || !outR)
  {
    std::fprintf(stderr, "Failed to register JACK ports\n");
    jack_client_close(client);
    return 1;
  }

  if (jack_activate(client))
  {
    std::fprintf(stderr, "Failed to activate JACK client\n");
    jack_client_close(client);
    return 1;
  }

  std::printf("DSP engine running.\n");
  std::printf("Ports:\n");
  std::printf("  input : dsp_engine_v1:in_mono\n");
  std::printf("  output: dsp_engine_v1:out_L, dsp_engine_v1:out_R\n");
  std::printf("Ctrl+C to stop.\n");
  std::printf("Note: Use deprecated/start_dsp.sh for automatic PipeWire connections (legacy)\n");

  std::thread ctl(udpControlThread);
  std::thread meter(meterThread);

  // Don't auto-wire - let the startup script handle PipeWire connections
  // autoWire();

  while (running.load())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  if (ctl.joinable())
    ctl.join();
  if (meter.joinable())
    meter.join();

  jack_deactivate(client);
  jack_client_close(client);
  return 0;
}
