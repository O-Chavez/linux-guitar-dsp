#pragma once
#include <string>
#include <vector>

struct IRData {
  int sampleRate = 0;
  std::vector<float> mono; // normalized float, mono
};

// Loads a WAV/AIFF/etc via libsndfile, returns mono float IR.
// If file is multi-channel, it will downmix to mono (average).
// No resampling: sampleRate must match your JACK rate (48000).
bool load_ir_mono(const std::string& path, IRData& out, std::string& err);
