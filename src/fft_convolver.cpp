#include "fft_convolver.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <utility>

FFTConvolverPartitioned::FFTConvolverPartitioned(FFTConvolverPartitioned &&other) noexcept
{
  *this = std::move(other);
}

FFTConvolverPartitioned &FFTConvolverPartitioned::operator=(FFTConvolverPartitioned &&other) noexcept
{
  if (this == &other)
    return *this;

  clear();

  mBlock = other.mBlock;
  mFFT = other.mFFT;
  mBins = other.mBins;
  mParts = other.mParts;
  mWrite = other.mWrite;
  mReady = other.mReady;

  mTimeIn = std::move(other.mTimeIn);
  mTimeOut = std::move(other.mTimeOut);
  mOverlap = std::move(other.mOverlap);

  mFreqY = other.mFreqY;
  other.mFreqY = nullptr;

  mH = std::move(other.mH);
  mX = std::move(other.mX);

  mPlanFwd = other.mPlanFwd;
  other.mPlanFwd = nullptr;
  mPlanInv = other.mPlanInv;
  other.mPlanInv = nullptr;

  other.mBlock = 0;
  other.mFFT = 0;
  other.mBins = 0;
  other.mParts = 0;
  other.mWrite = 0;
  other.mReady = false;
  other.mH.clear();
  other.mX.clear();

  return *this;
}

FFTConvolverPartitioned::~FFTConvolverPartitioned() { clear(); }

void FFTConvolverPartitioned::clear()
{
  if (mPlanFwd)
  {
    fftwf_destroy_plan(mPlanFwd);
    mPlanFwd = nullptr;
  }
  if (mPlanInv)
  {
    fftwf_destroy_plan(mPlanInv);
    mPlanInv = nullptr;
  }
  if (mFreqY)
  {
    fftwf_free(mFreqY);
    mFreqY = nullptr;
  }

  for (auto *p : mH)
    if (p)
      fftwf_free(p);
  for (auto *p : mX)
    if (p)
      fftwf_free(p);
  mH.clear();
  mX.clear();

  mTimeIn.clear();
  mTimeOut.clear();
  mOverlap.clear();

  mBlock = mFFT = mBins = mParts = mWrite = 0;
  mReady = false;
}

bool FFTConvolverPartitioned::init(const std::vector<float> &ir, int blockSize)
{
  clear();
  if (blockSize <= 0)
    return false;

  mBlock = blockSize;
  mFFT = 2 * mBlock;
  mBins = mFFT / 2 + 1;

  mParts = (int)((ir.size() + (size_t)mBlock - 1) / (size_t)mBlock);
  if (mParts <= 0)
    return false;

  if (const char *e = std::getenv("ALSA_LOG_IR_INIT"))
  {
    if (std::atoi(e) != 0)
    {
      std::fprintf(stderr,
                   "IR init: len=%zu block=%d fft=%d bins=%d parts=%d\n",
                   ir.size(), mBlock, mFFT, mBins, mParts);
    }
  }

  mTimeIn.assign((size_t)mFFT, 0.0f);
  mTimeOut.assign((size_t)mFFT, 0.0f);
  mOverlap.assign((size_t)mBlock, 0.0f);

  mFreqY = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * (size_t)mBins);
  if (!mFreqY)
    return false;

  // Allocate spectra arrays
  mH.resize((size_t)mParts);
  mX.resize((size_t)mParts);
  for (int k = 0; k < mParts; k++)
  {
    mH[(size_t)k] = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * (size_t)mBins);
    mX[(size_t)k] = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * (size_t)mBins);
    if (!mH[(size_t)k] || !mX[(size_t)k])
      return false;
    std::memset(mH[(size_t)k], 0, sizeof(fftwf_complex) * (size_t)mBins);
    std::memset(mX[(size_t)k], 0, sizeof(fftwf_complex) * (size_t)mBins);
  }

  // Plans (use ESTIMATE to keep init fast; MEASURE can be done later)
  mPlanFwd = fftwf_plan_dft_r2c_1d(mFFT, mTimeIn.data(), mX[0], FFTW_ESTIMATE);
  mPlanInv = fftwf_plan_dft_c2r_1d(mFFT, mFreqY, mTimeOut.data(), FFTW_ESTIMATE);
  if (!mPlanFwd || !mPlanInv)
    return false;

  // Precompute IR partitions in frequency domain
  // For each partition: time = [ir_part (N), 0... (N)]  -> FFT size 2N
  for (int k = 0; k < mParts; k++)
  {
    std::fill(mTimeIn.begin(), mTimeIn.end(), 0.0f);
    const size_t start = (size_t)k * (size_t)mBlock;
    const size_t end = std::min(start + (size_t)mBlock, ir.size());
    for (size_t i = start; i < end; i++)
    {
      mTimeIn[i - start] = ir[i];
    }
    // Temporarily use mX[0] as FFT output then copy into H[k]
    fftwf_execute_dft_r2c(mPlanFwd, mTimeIn.data(), mX[0]);
    std::memcpy(mH[(size_t)k], mX[0], sizeof(fftwf_complex) * (size_t)mBins);
  }

  // IMPORTANT: Clear all mX buffers after computing H partitions
  // mX is the input signal history ring buffer and must start clean
  for (int k = 0; k < mParts; k++)
  {
    std::memset(mX[(size_t)k], 0, sizeof(fftwf_complex) * (size_t)mBins);
  }

  mWrite = 0;
  mReady = true;
  return true;
}

bool FFTConvolverPartitioned::processBlock(const float *in, float *out, int n)
{
  if (!mReady || n != mBlock)
    return false;

  // Write new input block spectrum into ring
  // Only need to clear the second half of the FFT input.
  std::memcpy(mTimeIn.data(), in, sizeof(float) * (size_t)mBlock);
  std::memset(mTimeIn.data() + (size_t)mBlock, 0, sizeof(float) * (size_t)mBlock);
  fftwf_execute_dft_r2c(mPlanFwd, mTimeIn.data(), mX[(size_t)mWrite]);

  // Y = sum_{k} X[n-k] * H[k]
  std::memset(mFreqY, 0, sizeof(fftwf_complex) * (size_t)mBins);

  for (int k = 0; k < mParts; k++)
  {
    int idx = mWrite - k;
    if (idx < 0)
      idx += mParts;

    fftwf_complex *Xk = mX[(size_t)idx];
    fftwf_complex *Hk = mH[(size_t)k];
    for (int b = 0; b < mBins; b++)
    {
      cmul_acc(mFreqY[(size_t)b], Xk[(size_t)b], Hk[(size_t)b]);
    }
  }

  // IFFT to time
  fftwf_execute_dft_c2r(mPlanInv, mFreqY, mTimeOut.data());

  // FFTW doesn't normalize; divide by FFT size
  const float invFFT = 1.0f / (float)mFFT;

  // Overlap-add: first N samples + previous overlap
  for (int i = 0; i < mBlock; i++)
  {
    float y = (mTimeOut[(size_t)i] * invFFT) + mOverlap[(size_t)i];
    out[i] = y;
  }

  // Save new overlap = second half
  for (int i = 0; i < mBlock; i++)
  {
    mOverlap[(size_t)i] = (mTimeOut[(size_t)(i + mBlock)] * invFFT);
  }

  // advance ring write index
  mWrite++;
  if (mWrite >= mParts)
    mWrite = 0;

  return true;
}
