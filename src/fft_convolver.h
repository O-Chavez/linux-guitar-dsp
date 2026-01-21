#pragma once
#include <vector>
#include <fftw3.h>

class FFTConvolverPartitioned
{
public:
  FFTConvolverPartitioned() = default;
  ~FFTConvolverPartitioned();

  FFTConvolverPartitioned(const FFTConvolverPartitioned &) = delete;
  FFTConvolverPartitioned &operator=(const FFTConvolverPartitioned &) = delete;

  FFTConvolverPartitioned(FFTConvolverPartitioned &&other) noexcept;
  FFTConvolverPartitioned &operator=(FFTConvolverPartitioned &&other) noexcept;

  // blockSize must match JACK buffer size for minimum latency.
  // ir must be mono float at same sample rate as the stream.
  bool init(const std::vector<float> &ir, int blockSize);

  // in/out length must be blockSize. Returns false if not initialized.
  bool processBlock(const float *in, float *out, int n);

  int blockSize() const { return mBlock; }
  bool ready() const { return mReady; }

private:
  void clear();

  int mBlock = 0;
  int mFFT = 0;
  int mBins = 0;
  int mParts = 0;
  int mWrite = 0;
  bool mReady = false;

  std::vector<float> mTimeIn;  // fft input (size mFFT)
  std::vector<float> mTimeOut; // ifft output (size mFFT)
  std::vector<float> mOverlap; // overlap (size mBlock)
  fftwf_complex *mFreqY = nullptr;

  std::vector<fftwf_complex *> mH; // IR partitions spectra
  std::vector<fftwf_complex *> mX; // ring buffer of input block spectra

  fftwf_plan mPlanFwd = nullptr;
  fftwf_plan mPlanInv = nullptr;

  static inline void cmul_acc(fftwf_complex &y, const fftwf_complex &a, const fftwf_complex &b)
  {
    // y += a*b
    const float ar = a[0], ai = a[1];
    const float br = b[0], bi = b[1];
    y[0] += ar * br - ai * bi;
    y[1] += ar * bi + ai * br;
  }
};
