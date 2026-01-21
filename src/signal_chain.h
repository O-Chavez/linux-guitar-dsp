#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "signal_chain_schema.h"
#include "signal_chain_nodes.h"

namespace pedal::dsp
{

  class SignalChain
  {
  public:
    struct NodeTimingStat
    {
      const char *type = nullptr; // stable for chain lifetime
      uint64_t calls = 0;
      uint64_t sumUs = 0;
      uint64_t maxUs = 0;
    };

    SignalChain(pedal::chain::ChainSpec spec, std::vector<std::unique_ptr<INode>> nodes, ProcessContext ctx);

    const pedal::chain::ChainSpec &spec() const { return spec_; }

    // Realtime-safe processing
    void process(const float *in, float *out, uint32_t nframes) noexcept;

    bool nodeTimingEnabled() const noexcept { return nodeTimingEnabled_; }
    // Copies timing stats into caller-provided buffer. If reset=true, clears counters after snapshot.
    // Returns number of entries written.
    size_t snapshotNodeTiming(NodeTimingStat *out, size_t cap, bool reset) noexcept;

    uint32_t sampleRate() const { return ctx_.sampleRate; }
    uint32_t maxBlockFrames() const { return ctx_.maxBlockFrames; }

  private:
    pedal::chain::ChainSpec spec_;
    std::vector<std::unique_ptr<INode>> nodes_;
    ProcessContext ctx_;

    std::vector<float> bufA_;
    std::vector<float> bufB_;

    struct TimingBucket
    {
      uint64_t calls = 0;
      uint64_t sumUs = 0;
      uint64_t maxUs = 0;
    };

    bool nodeTimingEnabled_ = false;
    std::vector<std::string> timingTypes_;
    std::vector<TimingBucket> timingBuckets_;
    std::vector<uint32_t> nodeToBucket_;
  };

  struct BuildChainResult
  {
    std::shared_ptr<SignalChain> chain;
    std::string warning;
  };

  // Build a chain (heavy work allowed). Returns nullopt on failure.
  std::optional<BuildChainResult> buildChain(const pedal::chain::ChainSpec &spec,
                                             const ProcessContext &ctx,
                                             std::string &err);

} // namespace pedal::dsp
