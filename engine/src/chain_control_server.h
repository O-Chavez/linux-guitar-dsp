#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "json.hpp"
#include "signal_chain.h"
#include "signal_chain_schema.h"

namespace pedal::control
{

  struct ChainRuntimeState
  {
    // Atomic ops on shared_ptr use std::atomic_load/store/exchange free functions.
    std::shared_ptr<pedal::dsp::SignalChain> activeChain;
    std::shared_ptr<pedal::dsp::SignalChain> pendingChain;

    // Last accepted spec (what we persist / return from get_chain).
    // Only accessed on the control thread.
    pedal::chain::ChainSpec lastSpec;

    pedal::dsp::ProcessContext ctx;

    std::atomic<bool> running{true};

    std::string configPath = "/opt/pedal/config/chain.json";
    std::string socketPath = "/tmp/pedal-dsp.sock";
  };

  // Starts a simple line-delimited JSON Unix-domain socket control server.
  // Requests (one per line):
  //   {"cmd":"get_chain"}
  //   {"cmd":"set_chain","chain":{...}}
  //   {"cmd":"list_types"}
  // Responses are one JSON per line.
  std::thread startControlServer(ChainRuntimeState *state);

  // Writes canonical chain JSON to disk atomically.
  bool persistChainToDisk(const std::string &path, const pedal::chain::ChainSpec &spec, std::string &err);

} // namespace pedal::control
