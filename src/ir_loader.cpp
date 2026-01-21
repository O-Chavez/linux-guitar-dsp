#include "ir_loader.h"
#include <sndfile.h>
#include <cmath>

bool load_ir_mono(const std::string& path, IRData& out, std::string& err) {
  SF_INFO info;
  info.format = 0;

  SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
  if (!f) {
    err = std::string("sndfile open failed: ") + sf_strerror(nullptr);
    return false;
  }

  if (info.frames <= 0 || info.samplerate <= 0 || info.channels <= 0) {
    sf_close(f);
    err = "invalid audio file metadata";
    return false;
  }

  std::vector<float> interleaved((size_t)info.frames * (size_t)info.channels);
  sf_count_t got = sf_readf_float(f, interleaved.data(), info.frames);
  sf_close(f);

  if (got != info.frames) {
    err = "short read from audio file";
    return false;
  }

  out.sampleRate = info.samplerate;
  out.mono.resize((size_t)info.frames);

  if (info.channels == 1) {
    out.mono = std::move(interleaved);
  } else {
    for (sf_count_t i = 0; i < info.frames; i++) {
      double sum = 0.0;
      for (int c = 0; c < info.channels; c++) {
        sum += interleaved[(size_t)i * (size_t)info.channels + (size_t)c];
      }
      out.mono[(size_t)i] = (float)(sum / (double)info.channels);
    }
  }

  // Optional: remove DC-ish tiny offsets (helps some IRs)
  float mean = 0.0f;
  for (float v : out.mono) mean += v;
  mean /= (float)out.mono.size();
  for (float& v : out.mono) v -= mean;

  return true;
}
