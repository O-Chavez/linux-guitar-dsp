#include "signal_chain.h"

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace pedal::dsp
{

  SignalChain::SignalChain(pedal::chain::ChainSpec spec,
                           std::vector<std::unique_ptr<INode>> nodes,
                           ProcessContext ctx)
      : spec_(std::move(spec)), nodes_(std::move(nodes)), ctx_(ctx)
  {
    bufA_.assign(ctx_.maxBlockFrames, 0.0f);
    bufB_.assign(ctx_.maxBlockFrames, 0.0f);

    if (const char *e = std::getenv("ALSA_NODE_TIMING"))
      nodeTimingEnabled_ = (std::atoi(e) != 0);

    if (nodeTimingEnabled_)
    {
      // Precompute node->bucket mapping (non-RT) so the audio thread only does array lookups.
      std::unordered_map<std::string, uint32_t> typeToBucket;
      typeToBucket.reserve(nodes_.size());

      nodeToBucket_.assign(nodes_.size(), 0);
      for (size_t i = 0; i < nodes_.size(); i++)
      {
        const std::string &t = nodes_[i]->type();
        auto it = typeToBucket.find(t);
        if (it == typeToBucket.end())
        {
          const uint32_t idx = (uint32_t)timingTypes_.size();
          timingTypes_.push_back(t);
          typeToBucket.emplace(timingTypes_.back(), idx);
          it = typeToBucket.find(timingTypes_.back());
        }
        nodeToBucket_[i] = it->second;
      }

      timingBuckets_.assign(timingTypes_.size(), TimingBucket{});
    }
  }

  size_t SignalChain::snapshotNodeTiming(NodeTimingStat *out, size_t cap, bool reset) noexcept
  {
    if (!nodeTimingEnabled_ || !out || cap == 0)
      return 0;
    const size_t n = std::min(cap, timingBuckets_.size());
    for (size_t i = 0; i < n; i++)
    {
      out[i].type = timingTypes_[i].c_str();
      out[i].calls = timingBuckets_[i].calls;
      out[i].sumUs = timingBuckets_[i].sumUs;
      out[i].maxUs = timingBuckets_[i].maxUs;
      if (reset)
      {
        timingBuckets_[i].calls = 0;
        timingBuckets_[i].sumUs = 0;
        timingBuckets_[i].maxUs = 0;
      }
    }
    return n;
  }

  void SignalChain::process(const float *in, float *out, uint32_t nframes) noexcept
  {
    const uint32_t frames = (nframes <= ctx_.maxBlockFrames) ? nframes : ctx_.maxBlockFrames;
    if (nodes_.empty())
    {
      if (out != in)
        std::memcpy(out, in, sizeof(float) * nframes);
      return;
    }

    float *a = bufA_.data();
    float *b = bufB_.data();

    if (!nodeTimingEnabled_)
    {
      // Node 0: in -> a
      nodes_[0]->process(in, a, frames);
      for (size_t i = 1; i < nodes_.size(); i++)
      {
        nodes_[i]->process(a, b, frames);
        std::swap(a, b);
      }
    }
    else
    {
      using Clock = std::chrono::steady_clock;

      // Node 0: in -> a
      {
        const auto t0 = Clock::now();
        nodes_[0]->process(in, a, frames);
        const auto t1 = Clock::now();
        const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        const uint32_t bi = nodeToBucket_.empty() ? 0u : nodeToBucket_[0];
        if (bi < timingBuckets_.size())
        {
          auto &bkt = timingBuckets_[bi];
          bkt.calls++;
          bkt.sumUs += us;
          if (us > bkt.maxUs)
            bkt.maxUs = us;
        }
      }

      for (size_t i = 1; i < nodes_.size(); i++)
      {
        const auto t0 = Clock::now();
        nodes_[i]->process(a, b, frames);
        const auto t1 = Clock::now();
        const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        const uint32_t bi = nodeToBucket_.empty() ? 0u : nodeToBucket_[i];
        if (bi < timingBuckets_.size())
        {
          auto &bkt = timingBuckets_[bi];
          bkt.calls++;
          bkt.sumUs += us;
          if (us > bkt.maxUs)
            bkt.maxUs = us;
        }
        std::swap(a, b);
      }
    }

    if (a != out)
      std::memcpy(out, a, sizeof(float) * frames);

    // Safety: if caller ever provides more frames than our internal buffers, passthrough the tail.
    if (frames < nframes)
      std::memcpy(out + frames, in + frames, sizeof(float) * (nframes - frames));
  }

  std::optional<BuildChainResult> buildChain(const pedal::chain::ChainSpec &spec,
                                             const ProcessContext &ctx,
                                             std::string &err)
  {
    std::vector<std::unique_ptr<INode>> nodes;
    nodes.reserve(spec.chain.size());

    std::string warnings;

    for (const auto &ns : spec.chain)
    {
      std::string nodeErr;
      auto built = buildNode(ns, ctx, nodeErr);
      if (!built || !built->node)
      {
        err = "Failed to build node '" + ns.id + "' (" + ns.type + "): " + nodeErr;
        return std::nullopt;
      }
      if (!built->warning.empty())
      {
        if (!warnings.empty())
          warnings += "\n";
        warnings += built->warning;
      }
      nodes.push_back(std::move(built->node));
    }

    BuildChainResult r;
    r.chain = std::make_shared<SignalChain>(spec, std::move(nodes), ctx);
    r.warning = warnings;
    return r;
  }

} // namespace pedal::dsp
