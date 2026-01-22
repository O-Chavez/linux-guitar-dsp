-- WirePlumber override: de-prioritize PipeWire's Dummy-Driver and Freewheel-Driver.
--
-- Motivation: These support drivers can end up clocking the graph (system monotonic)
-- instead of the ALSA device clock, leading to "slow" scheduling (e.g. ~0.16x realtime).
--
-- Install:
--   mkdir -p ~/.config/wireplumber/main.lua.d
--   cp scripts/wp_disable_dummy_freewheel.lua ~/.config/wireplumber/main.lua.d/52-disable-dummy-freewheel.lua
--   systemctl --user restart wireplumber
--
-- Notes:
-- - We don't disable the nodes entirely, we just drop their driver priority so
--   ALSA drivers can win.
-- - This is intended for a dedicated audio box; adjust values if needed.

alsa_monitor = alsa_monitor or {}
alsa_monitor.rules = alsa_monitor.rules or {}

table.insert(alsa_monitor.rules, {
  matches = {
    {
      { "node.name", "equals", "Dummy-Driver" },
    },
    {
      { "node.name", "equals", "Freewheel-Driver" },
    },
  },
  apply_properties = {
    -- Keep them as drivers, but make them the lowest priority.
    ["priority.driver"] = 1,
    -- Optionally also reduce session priority.
    ["priority.session"] = 1,
  },
})
