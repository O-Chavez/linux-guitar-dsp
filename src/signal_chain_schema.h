#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "json.hpp"

namespace pedal::chain
{

  using Json = nlohmann::json;

  struct AssetRef
  {
    std::string path;
  };

  struct NodeSpec
  {
    std::string id;
    std::string type;
    std::string category;
    bool enabled = true;
    Json params = Json::object();
    std::optional<AssetRef> asset;
  };

  struct ChainSpec
  {
    int version = 1;
    uint32_t sampleRate = 48000;
    std::vector<NodeSpec> chain;
  };

  struct ValidationError
  {
    std::string message;
  };

  // Parses either the v1 canonical format, or the legacy format currently used by this repo.
  // Throws on malformed JSON types; returns std::nullopt for validation errors.
  std::optional<ChainSpec> parseChainJson(const Json &j, ValidationError &err);

  // Strict v1 validation of an ordered list.
  std::optional<ChainSpec> validateChainSpec(ChainSpec spec, ValidationError &err);

  Json chainSpecToJson(const ChainSpec &spec);

  // Helpers
  bool isBuiltinType(const std::string &type);

} // namespace pedal::chain
