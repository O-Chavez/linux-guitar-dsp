#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"
#include "signal_chain_schema.h"

namespace nam
{
  class DSP;
}

class FFTConvolverPartitioned;

namespace pedal::dsp
{

  using Json = nlohmann::json;

  struct ProcessContext
  {
    uint32_t sampleRate = 48000;
    uint32_t maxBlockFrames = 256;

    // Optional realtime parameters (owned outside the chain). Nodes may read these atomics once per block.
    // These pointers must remain valid for the lifetime of the running engine.
    std::atomic<float> *inputTrimDb = nullptr;
    std::atomic<float> *inputTrimLin = nullptr;
  };

  struct NodeStandardParams
  {
    bool enabled = true;
    float levelDb = 0.0f;
    float mix = 1.0f; // optional; default 1.0

    // Cached (non-RT) derived values. Builders should populate these so process() can stay pow-free.
    float levelLin = 1.0f;
    float mixWet = 1.0f;
    float mixDry = 0.0f;
  };

  class INode
  {
  public:
    virtual ~INode() = default;
    virtual const std::string &id() const = 0;
    virtual const std::string &type() const = 0;

    // Process mono buffer: in[0..nframes) -> out[0..nframes)
    // Must be realtime-safe: no allocations, no locks, no filesystem.
    virtual void process(const float *in, float *out, uint32_t nframes) noexcept = 0;
  };

  struct NodeBuildResult
  {
    std::unique_ptr<INode> node;
    std::string warning; // non-fatal
  };

  // Build a node from spec. Heavy work (model/IR loading) happens here, off the audio thread.
  // Returns nullptr node and error string on failure.
  std::optional<NodeBuildResult> buildNode(const pedal::chain::NodeSpec &spec,
                                           const ProcessContext &ctx,
                                           std::string &err);

  // Metadata for UI/backends (ranges/defaults). Kept minimal for v1.
  Json nodeTypeManifest();

} // namespace pedal::dsp
