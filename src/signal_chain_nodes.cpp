#include "signal_chain_nodes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

#include "fft_convolver.h"
#include "get_dsp.h"
#include "ir_loader.h"

namespace pedal::dsp
{

  static inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

  class PassthroughNode final : public INode
  {
  public:
    PassthroughNode(std::string id, std::string type, NodeStandardParams sp)
        : id_(std::move(id)), type_(std::move(type)), std_(sp)
    {
    }

    const std::string &id() const override { return id_; }
    const std::string &type() const override { return type_; }

    void process(const float *in, float *out, uint32_t nframes) noexcept override
    {
      if (!std_.enabled)
      {
        std::memcpy(out, in, sizeof(float) * nframes);
        return;
      }
      const float level = std_.levelLin;
      const float wetG = std_.mixWet;
      const float dryG = std_.mixDry;
      for (uint32_t i = 0; i < nframes; i++)
      {
        const float wet = in[i] * level;
        out[i] = in[i] * dryG + wet * wetG;
      }
    }

  private:
    std::string id_;
    std::string type_;
    NodeStandardParams std_;
  };
  static inline float clampf(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi
                                                                                            : v; }

  static inline float softclipFast(float x)
  {
    if (x > 1.0f)
      return 1.0f;
    if (x < -1.0f)
      return -1.0f;
    const float b = 0.3333333f;
    return x - b * x * x * x;
  }

  static NodeStandardParams parseStd(const pedal::chain::NodeSpec &spec)
  {
    NodeStandardParams p;
    p.enabled = spec.enabled;

    if (spec.params.is_object())
    {
      if (spec.params.contains("levelDb") && spec.params["levelDb"].is_number())
        p.levelDb = (float)spec.params["levelDb"].get<double>();
      if (spec.params.contains("outputGainDb") && spec.params["outputGainDb"].is_number())
        p.levelDb = (float)spec.params["outputGainDb"].get<double>();

      if (spec.params.contains("mix") && spec.params["mix"].is_number())
        p.mix = (float)spec.params["mix"].get<double>();
    }

    p.levelDb = clampf(p.levelDb, -48.0f, 24.0f);
    p.mix = clampf(p.mix, 0.0f, 1.0f);

    // Cache derived values so process() doesn't call pow().
    p.levelLin = dbToLin(p.levelDb);
    p.mixWet = p.mix;
    p.mixDry = 1.0f - p.mix;
    return p;
  }

  class InputNode final : public INode
  {
  public:
    explicit InputNode(std::string id,
                       NodeStandardParams sp,
                       std::atomic<float> *inputTrimLin,
                       float fallbackTrimLin)
        : id_(std::move(id)), std_(sp), inputTrimLin_(inputTrimLin), fallbackTrimLin_(fallbackTrimLin)
    {
      type_ = "input";
    }

    const std::string &id() const override { return id_; }
    const std::string &type() const override { return type_; }

    void process(const float *in, float *out, uint32_t nframes) noexcept override
    {
      const bool en = std_.enabled;
      const float trim = inputTrimLin_ ? inputTrimLin_->load(std::memory_order_relaxed) : fallbackTrimLin_;
      const float level = std_.levelLin;
      const float wetG = std_.mixWet;
      const float dryG = std_.mixDry;

      if (!en)
      {
        std::memcpy(out, in, sizeof(float) * nframes);
        return;
      }

      // Wet = in * trim * level
      for (uint32_t i = 0; i < nframes; i++)
      {
        const float wet = in[i] * trim * level;
        out[i] = in[i] * dryG + wet * wetG;
      }
    }

  private:
    std::string id_;
    std::string type_;
    NodeStandardParams std_;
    std::atomic<float> *inputTrimLin_ = nullptr;
    float fallbackTrimLin_ = 1.0f;
  };

  class OutputNode final : public INode
  {
  public:
    explicit OutputNode(std::string id, NodeStandardParams sp)
        : id_(std::move(id)), std_(sp)
    {
      type_ = "output";
    }

    const std::string &id() const override { return id_; }
    const std::string &type() const override { return type_; }

    void process(const float *in, float *out, uint32_t nframes) noexcept override
    {
      const bool en = std_.enabled;
      const float level = std_.levelLin;
      const float wetG = std_.mixWet;
      const float dryG = std_.mixDry;

      if (!en)
      {
        std::memcpy(out, in, sizeof(float) * nframes);
        return;
      }

      for (uint32_t i = 0; i < nframes; i++)
      {
        const float wet = in[i] * level;
        out[i] = in[i] * dryG + wet * wetG;
      }
    }

  private:
    std::string id_;
    std::string type_;
    NodeStandardParams std_;
  };

  class OverdriveNode final : public INode
  {
  public:
    explicit OverdriveNode(std::string id, NodeStandardParams sp, float drive, float tone, float outDb)
        : id_(std::move(id)), std_(sp)
    {
      type_ = "overdrive";

      drive_ = clampf(drive, 0.0f, 1.0f);
      tone_ = clampf(tone, 0.0f, 1.0f);
      outDb_ = outDb;

      pre_ = 1.0f + drive_ * 20.0f;
      a_ = 0.02f + (1.0f - tone_) * 0.2f;
      toneInv_ = 1.0f - tone_;
      postLin_ = dbToLin(outDb_) * std_.levelLin;
    }

    const std::string &id() const override { return id_; }
    const std::string &type() const override { return type_; }

    void process(const float *in, float *out, uint32_t nframes) noexcept override
    {
      if (!std_.enabled)
      {
        std::memcpy(out, in, sizeof(float) * nframes);
        return;
      }

      const float wetG = std_.mixWet;
      const float dryG = std_.mixDry;

      // Cheap tilt-ish: blend between lowpassed-ish and bright; implemented as simple one-pole.
      float z = z1_;

      for (uint32_t i = 0; i < nframes; i++)
      {
        float x = in[i] * pre_;
        float y = softclipFast(x);
        z = z + a_ * (y - z);
        float wet = (z * toneInv_ + y * tone_) * postLin_;
        out[i] = in[i] * dryG + wet * wetG;
      }

      z1_ = z;
    }

  private:
    std::string id_;
    std::string type_;
    NodeStandardParams std_;
    float drive_ = 0.5f;
    float tone_ = 0.5f;
    float outDb_ = 0.0f;
    float pre_ = 1.0f;
    float postLin_ = 1.0f;
    float a_ = 0.12f;
    float toneInv_ = 0.5f;
    float z1_ = 0.0f;
  };

  class NamModelNode final : public INode
  {
  public:
    NamModelNode(std::string id,
                 NodeStandardParams sp,
                 std::unique_ptr<nam::DSP> model,
                 uint32_t sampleRate,
                 uint32_t maxFrames,
                 float preGainDb,
                 float postGainDb,
                 float inLimit,
                 bool softclip,
                 bool softclipTanh,
                 bool useInputLevel)
        : id_(std::move(id)), std_(sp), model_(std::move(model)), sr_(sampleRate), maxFrames_(maxFrames)
    {
      type_ = "nam_model";
      in_.assign(maxFrames_, 0.0f);
      out_.assign(maxFrames_, 0.0f);

      preGainDb_ = preGainDb;
      postGainDb_ = postGainDb;
      inLimit_ = inLimit;
      softclip_ = softclip;
      softclipTanh_ = softclipTanh;
      useInputLevel_ = useInputLevel;

      if (model_)
      {
        model_->ResetAndPrewarm((double)sr_, (int)maxFrames_);
        if (useInputLevel_ && model_->HasInputLevel())
        {
          constexpr float refDbu = 12.2f;
          const float modelDbu = (float)model_->GetInputLevel();
          levelScaleLin_ = std::pow(10.0f, (refDbu - modelDbu) / 20.0f);
        }
      }

      preLin_ = dbToLin(preGainDb_) * levelScaleLin_;
      postLin_ = dbToLin(postGainDb_) * std_.levelLin;
      lim_ = clampf(inLimit_, 0.05f, 1.0f);
    }

    const std::string &id() const override { return id_; }
    const std::string &type() const override { return type_; }

    void process(const float *in, float *out, uint32_t nframes) noexcept override
    {
      const uint32_t frames = (nframes <= maxFrames_) ? nframes : maxFrames_;
      if (!std_.enabled || !model_)
      {
        std::memcpy(out, in, sizeof(float) * nframes);
        return;
      }

      const float pre = preLin_;
      const float post = postLin_;
      const float lim = lim_;
      const float wetG = std_.mixWet;
      const float dryG = std_.mixDry;

      // Prepare input
      for (uint32_t i = 0; i < frames; i++)
      {
        float x = in[i] * pre;
        if (x > lim)
          x = lim;
        else if (x < -lim)
          x = -lim;

        if (!softclip_)
          in_[i] = x;
        else
          in_[i] = softclipTanh_ ? std::tanh(x) : softclipFast(x);
      }

      try
      {
        model_->process(in_.data(), out_.data(), (int)frames);
      }
      catch (...)
      {
        std::memcpy(out_.data(), in_.data(), sizeof(float) * frames);
      }

      for (uint32_t i = 0; i < frames; i++)
      {
        const float wet = out_[i] * post;
        out[i] = in[i] * dryG + wet * wetG;
      }

      // Safety: if caller ever provides more frames than our internal buffers, passthrough the tail.
      for (uint32_t i = frames; i < nframes; i++)
        out[i] = in[i];
    }

  private:
    std::string id_;
    std::string type_;
    NodeStandardParams std_;
    std::unique_ptr<nam::DSP> model_;
    uint32_t sr_ = 48000;
    uint32_t maxFrames_ = 256;
    std::vector<float> in_;
    std::vector<float> out_;

    float preGainDb_ = -12.0f;
    float postGainDb_ = 0.0f;
    float inLimit_ = 0.90f;
    bool softclip_ = true;
    bool softclipTanh_ = false;
    bool useInputLevel_ = true;
    float levelScaleLin_ = 1.0f;

    float preLin_ = 1.0f;
    float postLin_ = 1.0f;
    float lim_ = 0.90f;
  };

  class IrConvolverNode final : public INode
  {
  public:
    IrConvolverNode(std::string id, NodeStandardParams sp, FFTConvolverPartitioned convolver, uint32_t maxFrames)
        : id_(std::move(id)), std_(sp), conv_(std::move(convolver)), maxFrames_(maxFrames)
    {
      type_ = "ir_convolver";
      out_.assign(maxFrames_, 0.0f);
    }

    const std::string &id() const override { return id_; }
    const std::string &type() const override { return type_; }

    void process(const float *in, float *out, uint32_t nframes) noexcept override
    {
      if (!std_.enabled || !conv_.ready())
      {
        std::memcpy(out, in, sizeof(float) * nframes);
        return;
      }

      const uint32_t frames = (nframes <= maxFrames_) ? nframes : maxFrames_;

      bool ok = conv_.processBlock(in, out_.data(), (int)frames);
      if (!ok)
        std::memcpy(out_.data(), in, sizeof(float) * frames);

      const float level = std_.levelLin;
      const float wetG = std_.mixWet;
      const float dryG = std_.mixDry;
      for (uint32_t i = 0; i < frames; i++)
      {
        const float wet = out_.data()[i] * level;
        out[i] = in[i] * dryG + wet * wetG;
      }

      for (uint32_t i = frames; i < nframes; i++)
        out[i] = in[i];
    }

  private:
    std::string id_;
    std::string type_;
    NodeStandardParams std_;
    FFTConvolverPartitioned conv_;
    uint32_t maxFrames_ = 256;
    std::vector<float> out_;
  };

  static std::optional<float> numParam(const pedal::chain::NodeSpec &spec, const char *k)
  {
    if (!spec.params.is_object() || !spec.params.contains(k) || !spec.params[k].is_number())
      return std::nullopt;
    return (float)spec.params[k].get<double>();
  }

  std::optional<NodeBuildResult> buildNode(const pedal::chain::NodeSpec &spec,
                                           const ProcessContext &ctx,
                                           std::string &err)
  {
    NodeBuildResult r;

    if (spec.type == "input")
    {
      const auto sp = parseStd(spec);
      float trimDb = 0.0f;
      if (auto v = numParam(spec, "inputTrimDb"))
        trimDb = clampf(*v, -24.0f, 24.0f);

      const float trimLin = dbToLin(trimDb);

      // If the engine provided a realtime param store, seed it from the spec.
      // This allows UI/boot config to set an initial trim, while runtime controls update atomically.
      if (ctx.inputTrimDb)
        ctx.inputTrimDb->store(trimDb, std::memory_order_relaxed);
      if (ctx.inputTrimLin)
        ctx.inputTrimLin->store(trimLin, std::memory_order_relaxed);

      r.node = std::make_unique<InputNode>(spec.id, sp, ctx.inputTrimLin, trimLin);
      return r;
    }

    if (spec.type == "output")
    {
      const auto sp = parseStd(spec);
      r.node = std::make_unique<OutputNode>(spec.id, sp);
      return r;
    }

    if (spec.type == "overdrive")
    {
      const auto sp = parseStd(spec);
      const float drive = numParam(spec, "drive").value_or(0.6f);
      const float tone = numParam(spec, "tone").value_or(0.5f);
      const float outDb = numParam(spec, "levelDb").value_or(0.0f);
      r.node = std::make_unique<OverdriveNode>(spec.id, sp, drive, tone, outDb);
      return r;
    }

    if (spec.type == "nam_model")
    {
      if (!spec.enabled)
      {
        auto sp = parseStd(spec);
        sp.enabled = false;
        r.node = std::make_unique<PassthroughNode>(spec.id, "nam_model", sp);
        return r;
      }

      if (!spec.asset || spec.asset->path.empty())
      {
        // Boot-safety: allow chain to run even if model asset missing.
        // For v1 we force-bypass the node (enabled=false) and surface a warning.
        auto sp = parseStd(spec);
        sp.enabled = false;
        r.warning = "nam_model missing asset.path (bypassing)";
        r.node = std::make_unique<PassthroughNode>(spec.id, "nam_model", sp);
        return r;
      }

      std::unique_ptr<nam::DSP> model;
      try
      {
        model = nam::get_dsp(std::filesystem::path(spec.asset->path));
      }
      catch (const std::exception &e)
      {
        err = std::string("Failed to load NAM model: ") + e.what();
        return std::nullopt;
      }

      if (!model)
      {
        err = "Failed to load NAM model (get_dsp returned null)";
        return std::nullopt;
      }

      // Optional safety: warn on SR mismatch but keep running (NAM can be tolerant).
      const double expSR = model->GetExpectedSampleRate();
      if (expSR > 0.0 && std::llround(expSR) != (long long)ctx.sampleRate)
      {
        r.warning = "NAM expected sampleRate=" + std::to_string((int)std::llround(expSR)) +
                    " but engine is " + std::to_string(ctx.sampleRate);
      }

      const auto sp = parseStd(spec);
      const float preGainDb = numParam(spec, "preGainDb").value_or(-12.0f);
      const float postGainDb = numParam(spec, "postGainDb").value_or(0.0f);
      const float inLimit = numParam(spec, "inLimit").value_or(0.90f);

      bool softclip = true;
      bool softclipTanh = false;
      bool useInputLevel = true;
      if (spec.params.is_object())
      {
        if (spec.params.contains("softclip") && spec.params["softclip"].is_boolean())
          softclip = spec.params["softclip"].get<bool>();
        if (spec.params.contains("softclipTanh") && spec.params["softclipTanh"].is_boolean())
          softclipTanh = spec.params["softclipTanh"].get<bool>();
        if (spec.params.contains("useInputLevel") && spec.params["useInputLevel"].is_boolean())
          useInputLevel = spec.params["useInputLevel"].get<bool>();
      }

      r.node = std::make_unique<NamModelNode>(spec.id,
                                              sp,
                                              std::move(model),
                                              ctx.sampleRate,
                                              ctx.maxBlockFrames,
                                              preGainDb,
                                              postGainDb,
                                              inLimit,
                                              softclip,
                                              softclipTanh,
                                              useInputLevel);
      return r;
    }

    if (spec.type == "ir_convolver")
    {
      if (!spec.enabled)
      {
        auto sp = parseStd(spec);
        sp.enabled = false;
        r.node = std::make_unique<PassthroughNode>(spec.id, "ir_convolver", sp);
        return r;
      }

      if (!spec.asset || spec.asset->path.empty())
      {
        auto sp = parseStd(spec);
        sp.enabled = false;
        r.warning = "ir_convolver missing asset.path (bypassing)";
        r.node = std::make_unique<PassthroughNode>(spec.id, "ir_convolver", sp);
        return r;
      }

      IRData ir{};
      std::string loadErr;
      if (!load_ir_mono(spec.asset->path, ir, loadErr))
      {
        err = std::string("Failed to load IR: ") + loadErr;
        return std::nullopt;
      }

      if (ir.sampleRate != (int)ctx.sampleRate)
      {
        err = "IR sample-rate mismatch (IR=" + std::to_string(ir.sampleRate) +
              " engine=" + std::to_string(ctx.sampleRate) + ")";
        return std::nullopt;
      }

      // Apply optional normalize/gain (non-RT)
      float gainDb = 0.0f;
      float targetDb = -6.0f;
      bool useTarget = false;
      if (auto v = numParam(spec, "gainDb"))
        gainDb = *v;
      if (auto v = numParam(spec, "targetDb"))
      {
        targetDb = *v;
        useTarget = true;
      }

      const float gainLin = dbToLin(clampf(gainDb, -24.0f, 24.0f));
      if (gainLin != 1.0f)
      {
        for (float &v : ir.mono)
          v *= gainLin;
      }

      if (useTarget)
      {
        float peak = 0.0f;
        for (float v : ir.mono)
          peak = std::max(peak, std::fabs(v));

        const float target = dbToLin(clampf(targetDb, -24.0f, 0.0f));
        if (peak > 0.0f)
        {
          const float normG = target / peak;
          for (float &v : ir.mono)
            v *= normG;
        }
      }

      // Optional IR trimming (non-RT). Cab IRs are typically short; long IRs can be prohibitively expensive
      // for uniform partitioned convolution. Enable via node params or env.
      uint32_t maxSamples = 0;
      if (auto v = numParam(spec, "maxSamples"))
      {
        const float vf = *v;
        if (vf > 0.0f)
          maxSamples = (uint32_t)std::llround(vf);
      }
      if (maxSamples == 0)
      {
        if (auto v = numParam(spec, "maxMs"))
        {
          const float ms = *v;
          if (ms > 0.0f)
          {
            const double s = (double)ms / 1000.0;
            maxSamples = (uint32_t)std::llround(s * (double)ctx.sampleRate);
          }
        }
      }
      if (maxSamples == 0)
      {
        if (const char *e = std::getenv("ALSA_IR_MAX_SAMPLES"))
        {
          const long v = std::strtol(e, nullptr, 10);
          if (v > 0)
            maxSamples = (uint32_t)v;
        }
      }

      if (maxSamples > 0 && ir.mono.size() > (size_t)maxSamples)
      {
        // Taper the end to reduce truncation artifacts.
        const uint32_t taper = std::min<uint32_t>(128u, maxSamples);
        if (taper > 1)
        {
          constexpr float kPi = 3.14159265358979323846f;
          const size_t start = (size_t)maxSamples - (size_t)taper;
          for (uint32_t i = 0; i < taper; i++)
          {
            const float t = (float)i / (float)(taper - 1);
            const float g = 0.5f * (1.0f + std::cos(kPi * t)); // 1..0
            ir.mono[start + i] *= g;
          }
        }

        const size_t oldLen = ir.mono.size();
        ir.mono.resize((size_t)maxSamples);
        r.warning = "IR trimmed from " + std::to_string(oldLen) + " to " + std::to_string(maxSamples) + " samples";
      }

      FFTConvolverPartitioned conv;
      if (!conv.init(ir.mono, (int)ctx.maxBlockFrames))
      {
        err = "IR convolver init failed";
        return std::nullopt;
      }

      const auto sp = parseStd(spec);
      r.node = std::make_unique<IrConvolverNode>(spec.id, sp, std::move(conv), ctx.maxBlockFrames);
      return r;
    }

    err = "Unknown node type: " + spec.type;
    return std::nullopt;
  }

  Json nodeTypeManifest()
  {
    // Minimal metadata for v1 UI integration.
    // Keep it stable: backend can cache and render a drawer.
    Json j;
    j["version"] = 1;
    j["types"] = Json::array({
        Json{{"type", "overdrive"},
             {"category", "fx"},
             {"params",
              Json::array({
                  Json{{"key", "enabled"}, {"type", "bool"}, {"default", true}},
                  Json{{"key", "mix"}, {"type", "float"}, {"min", 0.0}, {"max", 1.0}, {"default", 1.0}},
                  Json{{"key", "levelDb"}, {"type", "float"}, {"min", -48.0}, {"max", 24.0}, {"default", 0.0}},
                  Json{{"key", "drive"}, {"type", "float"}, {"min", 0.0}, {"max", 1.0}, {"default", 0.6}},
                  Json{{"key", "tone"}, {"type", "float"}, {"min", 0.0}, {"max", 1.0}, {"default", 0.5}},
              })}},
        Json{{"type", "nam_model"},
             {"category", "amp"},
             {"asset", Json{{"required", true}, {"kind", "nam_model"}}},
             {"params",
              Json::array({
                  Json{{"key", "enabled"}, {"type", "bool"}, {"default", true}},
                  Json{{"key", "mix"}, {"type", "float"}, {"min", 0.0}, {"max", 1.0}, {"default", 1.0}},
                  Json{{"key", "levelDb"}, {"type", "float"}, {"min", -48.0}, {"max", 24.0}, {"default", 0.0}},
                  Json{{"key", "preGainDb"}, {"type", "float"}, {"min", -24.0}, {"max", 24.0}, {"default", -12.0}},
                  Json{{"key", "postGainDb"}, {"type", "float"}, {"min", -24.0}, {"max", 24.0}, {"default", 0.0}},
                  Json{{"key", "inLimit"}, {"type", "float"}, {"min", 0.05}, {"max", 1.0}, {"default", 0.90}},
                  Json{{"key", "softclip"}, {"type", "bool"}, {"default", true}},
                  Json{{"key", "softclipTanh"}, {"type", "bool"}, {"default", false}},
                  Json{{"key", "useInputLevel"}, {"type", "bool"}, {"default", true}},
              })}},
        Json{{"type", "ir_convolver"},
             {"category", "cab"},
             {"asset", Json{{"required", true}, {"kind", "ir_wav"}}},
             {"params",
              Json::array({
                  Json{{"key", "enabled"}, {"type", "bool"}, {"default", true}},
                  Json{{"key", "mix"}, {"type", "float"}, {"min", 0.0}, {"max", 1.0}, {"default", 1.0}},
                  Json{{"key", "levelDb"}, {"type", "float"}, {"min", -48.0}, {"max", 24.0}, {"default", 0.0}},
                  Json{{"key", "gainDb"}, {"type", "float"}, {"min", -24.0}, {"max", 24.0}, {"default", 0.0}},
                  Json{{"key", "targetDb"}, {"type", "float"}, {"min", -24.0}, {"max", 0.0}, {"default", -6.0}},
                  Json{{"key", "maxSamples"}, {"type", "float"}, {"min", 0.0}, {"max", 192000.0}, {"default", 0.0}},
                  Json{{"key", "maxMs"}, {"type", "float"}, {"min", 0.0}, {"max", 500.0}, {"default", 0.0}},
              })}},
        Json{{"type", "input"}, {"category", "utility"}},
        Json{{"type", "output"}, {"category", "utility"}},
    });
    return j;
  }

} // namespace pedal::dsp
