#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <csignal>
#include <memory>
#include <string>
#include <vector>

#include <sndfile.h>

#include "get_dsp.h"

static inline float clampf(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi
                                                                                          : v; }
static inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

static volatile std::sig_atomic_t g_shouldStop = 0;
static void onSigInt(int) { g_shouldStop = 1; }

struct Args
{
  std::string modelPath;
  std::string outPath;
  int sampleRate = 48000;
  float seconds = 5.0f;
  int blockSize = 128;
  float inputGainDb = -12.0f;
  float toneHz = 110.0f;
  bool pcm16 = false;
  bool normalize = false;
};

static void usage(const char *argv0)
{
  std::fprintf(stderr,
               "Usage: %s --model <path.nam> --out <out.wav> [--seconds 5] [--sr 48000] [--block 128] "
               "[--gain-db -12] [--tone-hz 110] [--pcm16] [--normalize]\n",
               argv0);
}

static bool parseArgs(int argc, char **argv, Args &a)
{
  for (int i = 1; i < argc; i++)
  {
    std::string k = argv[i];
    auto need = [&](const char *name) -> const char *
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "Missing value for %s\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (k == "--model")
    {
      const char *v = need("--model");
      if (!v)
        return false;
      a.modelPath = v;
    }
    else if (k == "--out")
    {
      const char *v = need("--out");
      if (!v)
        return false;
      a.outPath = v;
    }
    else if (k == "--seconds")
    {
      const char *v = need("--seconds");
      if (!v)
        return false;
      a.seconds = std::strtof(v, nullptr);
    }
    else if (k == "--sr")
    {
      const char *v = need("--sr");
      if (!v)
        return false;
      a.sampleRate = std::atoi(v);
    }
    else if (k == "--block")
    {
      const char *v = need("--block");
      if (!v)
        return false;
      a.blockSize = std::atoi(v);
    }
    else if (k == "--gain-db")
    {
      const char *v = need("--gain-db");
      if (!v)
        return false;
      a.inputGainDb = std::strtof(v, nullptr);
    }
    else if (k == "--tone-hz")
    {
      const char *v = need("--tone-hz");
      if (!v)
        return false;
      a.toneHz = std::strtof(v, nullptr);
    }
    else if (k == "--pcm16")
    {
      a.pcm16 = true;
    }
    else if (k == "--normalize")
    {
      a.normalize = true;
    }
    else if (k == "-h" || k == "--help")
    {
      return false;
    }
    else
    {
      std::fprintf(stderr, "Unknown arg: %s\n", k.c_str());
      return false;
    }
  }

  if (a.modelPath.empty() || a.outPath.empty())
    return false;
  if (!std::filesystem::exists(a.modelPath))
  {
    std::fprintf(stderr, "Model not found: %s\n", a.modelPath.c_str());
    return false;
  }
  a.sampleRate = (a.sampleRate <= 0) ? 48000 : a.sampleRate;
  a.blockSize = (a.blockSize <= 0) ? 128 : a.blockSize;
  a.seconds = clampf(a.seconds, 0.1f, 60.0f);
  return true;
}

// Synthetic "guitar-ish" signal:
// - sine at toneHz
// - second harmonic
// - light nonlinearity to create some dynamics
// - a simple exponential pluck envelope repeating at 2 Hz
static void makeSynth(std::vector<float> &x, int sr, float seconds, float hz, float gainDb)
{
  const int n = (int)std::lround(seconds * (float)sr);
  x.resize(n);

  const float g = dbToLin(gainDb);
  const float w1 = 2.0f * (float)M_PI * hz / (float)sr;
  const float w2 = 2.0f * (float)M_PI * (2.0f * hz) / (float)sr;

  const float pluckRate = 2.0f;
  const float pluckPeriod = 1.0f / pluckRate;

  for (int i = 0; i < n; i++)
  {
    const float t = (float)i / (float)sr;
    const float phase1 = w1 * (float)i;
    const float phase2 = w2 * (float)i;

    float envT = std::fmod(t, pluckPeriod);
    float env = std::exp(-envT * 6.0f); // fast decay

    float s = 0.7f * std::sin(phase1) + 0.25f * std::sin(phase2);
    // tiny pick transient
    if (envT < 0.01f)
      s += 0.2f * (1.0f - envT / 0.01f);

    // mild waveshaping to add some realistic spectrum
    s = std::tanh(1.8f * s);

    x[i] = s * env * g;
  }
}

static bool writeWavMono(const std::string &path, const std::vector<float> &y, int sr, bool pcm16)
{
  SF_INFO info{};
  info.samplerate = sr;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | (pcm16 ? SF_FORMAT_PCM_16 : SF_FORMAT_FLOAT);

  SNDFILE *sf = sf_open(path.c_str(), SFM_WRITE, &info);
  if (!sf)
  {
    std::fprintf(stderr, "Failed to open output wav: %s\n", sf_strerror(nullptr));
    return false;
  }

  sf_count_t wrote = 0;
  if (pcm16)
  {
    std::vector<short> tmp(y.size());
    for (size_t i = 0; i < y.size(); i++)
    {
      const float s = clampf(y[i], -1.0f, 1.0f);
      tmp[i] = (short)std::lround(s * 32767.0f);
    }
    wrote = sf_write_short(sf, tmp.data(), (sf_count_t)tmp.size());
  }
  else
  {
    wrote = sf_write_float(sf, y.data(), (sf_count_t)y.size());
  }
  sf_close(sf);

  if (wrote != (sf_count_t)y.size())
  {
    std::fprintf(stderr, "Short write: wrote %lld of %zu\n", (long long)wrote, y.size());
    return false;
  }
  return true;
}

static void computeStats(const std::vector<float> &y, float &peak, float &rms)
{
  peak = 0.0f;
  double e = 0.0;
  if (y.empty())
  {
    rms = 0.0f;
    return;
  }
  for (float s : y)
  {
    peak = std::max(peak, std::fabs(s));
    e += (double)s * (double)s;
  }
  rms = (float)std::sqrt(e / (double)y.size());
}

int main(int argc, char **argv)
{
  std::signal(SIGINT, onSigInt);

  Args a;
  if (!parseArgs(argc, argv, a))
  {
    usage(argv[0]);
    return 2;
  }

  std::printf("NAM synth test\n");
  std::printf("  model: %s\n", a.modelPath.c_str());
  std::printf("  out:   %s\n", a.outPath.c_str());
  std::printf("  sr=%d block=%d seconds=%.2f gain_db=%.1f tone_hz=%.1f\n",
              a.sampleRate,
              a.blockSize,
              (double)a.seconds,
              (double)a.inputGainDb,
              (double)a.toneHz);
  std::printf("  wav:   %s%s\n", a.pcm16 ? "pcm16" : "float32", a.normalize ? " normalized" : "");

  auto model = nam::get_dsp(std::filesystem::path(a.modelPath));
  if (!model)
  {
    std::fprintf(stderr, "nam::get_dsp returned null\n");
    return 1;
  }

  model->ResetAndPrewarm((double)a.sampleRate, a.blockSize);

  std::vector<float> x;
  makeSynth(x, a.sampleRate, a.seconds, a.toneHz, a.inputGainDb);

  std::vector<float> y(x.size());
  std::vector<float> in(a.blockSize);
  std::vector<float> out(a.blockSize);

  // Process in fixed blocks.
  size_t idx = 0;
  const size_t total = x.size();
  const size_t reportEvery = (size_t)a.sampleRate; // ~1 second
  size_t nextReport = reportEvery;

  while (idx < total)
  {
    if (g_shouldStop)
    {
      std::fprintf(stderr, "Interrupted; stopping early at %zu/%zu samples\n", idx, total);
      y.resize(idx);
      break;
    }

    const size_t remain = x.size() - idx;
    const int n = (int)std::min(remain, (size_t)a.blockSize);

    std::memset(in.data(), 0, sizeof(float) * (size_t)a.blockSize);
    std::memset(out.data(), 0, sizeof(float) * (size_t)a.blockSize);
    std::memcpy(in.data(), x.data() + idx, sizeof(float) * (size_t)n);

    model->process(in.data(), out.data(), a.blockSize);

    std::memcpy(y.data() + idx, out.data(), sizeof(float) * (size_t)n);
    idx += (size_t)n;

    if (idx >= nextReport)
    {
      const float pct = 100.0f * (float)idx / (float)total;
      std::fprintf(stderr, "... %zu/%zu samples (%.1f%%)\n", idx, total, pct);
      nextReport += reportEvery;
    }
  }

  if (a.normalize && !y.empty())
  {
    float peak = 0.0f, rms = 0.0f;
    computeStats(y, peak, rms);
    if (peak > 0.0f)
    {
      const float target = 0.98f;
      const float g = target / peak;
      for (float &s : y)
        s *= g;
    }
  }

  float peak = 0.0f, rms = 0.0f;
  computeStats(y, peak, rms);
  std::printf("  stats: peak=%.6f rms=%.6f\n", (double)peak, (double)rms);

  if (!writeWavMono(a.outPath, y, a.sampleRate, a.pcm16))
    return 1;

  std::printf("Wrote %zu samples\n", y.size());
  return 0;
}
