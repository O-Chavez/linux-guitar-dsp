#include "signal_chain_schema.h"

#include <cstdio>
#include <unordered_set>

namespace pedal::chain
{

  static std::optional<std::string> getString(const Json &j, const char *key, ValidationError &err)
  {
    if (!j.contains(key))
      return std::nullopt;
    if (!j[key].is_string())
    {
      err.message = std::string("Field '") + key + "' must be a string";
      return std::nullopt;
    }
    return j[key].get<std::string>();
  }

  bool isBuiltinType(const std::string &type)
  {
    return type == "input" || type == "output";
  }

  static std::optional<NodeSpec> parseNode(const Json &jn, ValidationError &err)
  {
    if (!jn.is_object())
    {
      err.message = "Each chain element must be an object";
      return std::nullopt;
    }

    NodeSpec n;
    if (auto id = getString(jn, "id", err))
      n.id = *id;
    else
    {
      err.message = "Node missing required string field 'id'";
      return std::nullopt;
    }

    if (auto t = getString(jn, "type", err))
      n.type = *t;
    else
    {
      err.message = "Node missing required string field 'type'";
      return std::nullopt;
    }

    if (jn.contains("category"))
    {
      if (!jn["category"].is_string())
      {
        err.message = "Node field 'category' must be a string";
        return std::nullopt;
      }
      n.category = jn["category"].get<std::string>();
    }

    if (jn.contains("enabled"))
    {
      if (!jn["enabled"].is_boolean())
      {
        err.message = "Node field 'enabled' must be a boolean";
        return std::nullopt;
      }
      n.enabled = jn["enabled"].get<bool>();
    }

    if (jn.contains("params"))
    {
      if (!jn["params"].is_object())
      {
        err.message = "Node field 'params' must be an object";
        return std::nullopt;
      }
      n.params = jn["params"];
    }

    if (jn.contains("asset"))
    {
      if (!jn["asset"].is_object())
      {
        err.message = "Node field 'asset' must be an object";
        return std::nullopt;
      }
      AssetRef a;
      if (!jn["asset"].contains("path") || !jn["asset"]["path"].is_string())
      {
        err.message = "Node asset requires string field 'path'";
        return std::nullopt;
      }
      a.path = jn["asset"]["path"].get<std::string>();
      n.asset = a;
    }

    return n;
  }

  static std::optional<ChainSpec> parseCanonicalV1(const Json &j, ValidationError &err)
  {
    ChainSpec spec;
    if (!j.contains("version") || !j["version"].is_number_integer())
    {
      err.message = "Missing/invalid 'version' (must be integer)";
      return std::nullopt;
    }
    spec.version = j["version"].get<int>();

    if (spec.version != 1)
    {
      err.message = "Unsupported chain version";
      return std::nullopt;
    }

    if (j.contains("sampleRate"))
    {
      if (!j["sampleRate"].is_number_integer())
      {
        err.message = "'sampleRate' must be integer";
        return std::nullopt;
      }
      int sr = j["sampleRate"].get<int>();
      if (sr <= 0)
      {
        err.message = "'sampleRate' must be > 0";
        return std::nullopt;
      }
      spec.sampleRate = (uint32_t)sr;
    }

    if (!j.contains("chain") || !j["chain"].is_array())
    {
      err.message = "Missing/invalid 'chain' (must be array)";
      return std::nullopt;
    }

    for (const auto &jn : j["chain"])
    {
      auto n = parseNode(jn, err);
      if (!n)
        return std::nullopt;
      spec.chain.push_back(*n);
    }

    return spec;
  }

  static std::optional<ChainSpec> parseLegacy(const Json &j, ValidationError &err)
  {
    // Legacy format used by current ALSA engine:
    // { audio: { inputTrimDb }, chain: { namModelPath, irPath }, debug: { passthrough } }
    // Convert to canonical ordered list.
    ChainSpec spec;
    spec.version = 1;
    if (j.contains("audio") && j["audio"].is_object() && j["audio"].contains("sampleRate") &&
        j["audio"]["sampleRate"].is_number_integer())
    {
      spec.sampleRate = (uint32_t)j["audio"]["sampleRate"].get<int>();
    }

    NodeSpec input;
    input.id = "input";
    input.type = "input";
    input.category = "utility";
    input.enabled = true;
    input.params = Json::object();
    if (j.contains("audio") && j["audio"].is_object() && j["audio"].contains("inputTrimDb"))
    {
      if (!j["audio"]["inputTrimDb"].is_number())
      {
        err.message = "legacy audio.inputTrimDb must be number";
        return std::nullopt;
      }
      input.params["inputTrimDb"] = j["audio"]["inputTrimDb"];
    }

    NodeSpec amp;
    amp.id = "amp1";
    amp.type = "nam_model";
    amp.category = "amp";
    amp.enabled = true;
    amp.params = Json::object();

    NodeSpec cab;
    cab.id = "cab1";
    cab.type = "ir_convolver";
    cab.category = "cab";
    cab.enabled = true;
    cab.params = Json::object();

    if (j.contains("chain") && j["chain"].is_object())
    {
      if (j["chain"].contains("namModelPath"))
      {
        if (!j["chain"]["namModelPath"].is_string())
        {
          err.message = "legacy chain.namModelPath must be string";
          return std::nullopt;
        }
        amp.asset = AssetRef{j["chain"]["namModelPath"].get<std::string>()};
      }
      if (j["chain"].contains("irPath"))
      {
        if (!j["chain"]["irPath"].is_string())
        {
          err.message = "legacy chain.irPath must be string";
          return std::nullopt;
        }
        cab.asset = AssetRef{j["chain"]["irPath"].get<std::string>()};
      }
    }

    NodeSpec output;
    output.id = "output";
    output.type = "output";
    output.category = "utility";
    output.enabled = true;
    output.params = Json::object();

    spec.chain = {input, amp, cab, output};
    return spec;
  }

  std::optional<ChainSpec> parseChainJson(const Json &j, ValidationError &err)
  {
    if (!j.is_object())
    {
      err.message = "Top-level JSON must be an object";
      return std::nullopt;
    }

    // Heuristic: canonical has version + chain array.
    if (j.contains("version") && j.contains("chain") && j["chain"].is_array())
      return parseCanonicalV1(j, err);

    // Legacy compatibility.
    return parseLegacy(j, err);
  }

  static bool hasType(const std::vector<NodeSpec> &nodes, const std::string &type)
  {
    for (const auto &n : nodes)
      if (n.type == type)
        return true;
    return false;
  }

  std::optional<ChainSpec> validateChainSpec(ChainSpec spec, ValidationError &err)
  {
    if (spec.version != 1)
    {
      err.message = "Only chain version 1 is supported";
      return std::nullopt;
    }

    if (spec.chain.size() < 2)
    {
      err.message = "Chain must contain at least input and output";
      return std::nullopt;
    }

    // IDs unique
    std::unordered_set<std::string> ids;
    for (const auto &n : spec.chain)
    {
      if (n.id.empty())
      {
        err.message = "Node id must be non-empty";
        return std::nullopt;
      }
      if (!ids.insert(n.id).second)
      {
        err.message = "Duplicate node id: " + n.id;
        return std::nullopt;
      }
      if (n.type.empty())
      {
        err.message = "Node type must be non-empty";
        return std::nullopt;
      }
    }

    // Must start with input and end with output.
    if (spec.chain.front().type != "input")
    {
      err.message = "First node must be type 'input'";
      return std::nullopt;
    }
    if (spec.chain.back().type != "output")
    {
      err.message = "Last node must be type 'output'";
      return std::nullopt;
    }

    // v1 hard constraint: Amp -> Cab mandatory.
    if (!hasType(spec.chain, "nam_model"))
    {
      err.message = "Chain must contain a 'nam_model' node";
      return std::nullopt;
    }
    if (!hasType(spec.chain, "ir_convolver"))
    {
      err.message = "Chain must contain an 'ir_convolver' node";
      return std::nullopt;
    }

    int ampIdx = -1;
    int cabIdx = -1;
    for (size_t i = 0; i < spec.chain.size(); i++)
    {
      if (spec.chain[i].type == "nam_model" && ampIdx < 0)
        ampIdx = (int)i;
      if (spec.chain[i].type == "ir_convolver" && cabIdx < 0)
        cabIdx = (int)i;
    }
    if (ampIdx < 0 || cabIdx < 0 || ampIdx >= cabIdx)
    {
      err.message = "Invalid ordering: 'nam_model' must appear before 'ir_convolver'";
      return std::nullopt;
    }

    // No other constraints for v1 beyond ordered list.
    return spec;
  }

  Json chainSpecToJson(const ChainSpec &spec)
  {
    Json j;
    j["version"] = spec.version;
    j["sampleRate"] = spec.sampleRate;
    Json arr = Json::array();
    for (const auto &n : spec.chain)
    {
      Json jn;
      jn["id"] = n.id;
      jn["type"] = n.type;
      if (!n.category.empty())
        jn["category"] = n.category;
      jn["enabled"] = n.enabled;
      jn["params"] = n.params.is_object() ? n.params : Json::object();
      if (n.asset)
        jn["asset"] = Json{{"path", n.asset->path}};
      arr.push_back(jn);
    }
    j["chain"] = arr;
    return j;
  }

} // namespace pedal::chain
