-- WirePlumber override: set sane low-latency ALSA buffering for the iRig HD X.
--
-- Why:
-- The current ALSA nodes show api.alsa.period-size=64 and api.alsa.period-num=512,
-- which implies ~0.68s of buffering at 48kHz and can lead to weird scheduling.
--
-- Strategy:
-- Start with a robust low-latency baseline:
--   period-size = 128 frames
--   period-num  = 3 periods
-- This is usually only +2.7ms vs period-num=2 at 48kHz, but is much more robust.
--
-- Install:
--   mkdir -p ~/.config/wireplumber/main.lua.d
--   cp scripts/wp_irig_alsa_periods.lua ~/.config/wireplumber/main.lua.d/53-irig-alsa-periods.lua
--   systemctl --user restart wireplumber
--
-- Notes:
-- - This matches both capture and playback nodes for the specific iRig HD X USB ID.
-- - If you want even lower latency, change api.alsa.period-num to 2.

alsa_monitor = alsa_monitor or {}
alsa_monitor.rules = alsa_monitor.rules or {}

table.insert(alsa_monitor.rules, {
  matches = {
    {
      { "node.name", "matches", "alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.*" },
    },
  },
  apply_properties = {
    -- Keep everything at 48k; your graph is already configured for 48k/128.
    ["audio.rate"] = 48000,

    -- Sane buffering.
    ["api.alsa.period-size"] = 128,
    ["api.alsa.period-num"] = 3,

    -- A small headroom helps avoid xruns without huge latency.
    ["api.alsa.headroom"] = 64,
  },
})

table.insert(alsa_monitor.rules, {
  matches = {
    {
      { "node.name", "matches", "alsa_input.usb-IK_Multimedia_iRig_HD_X_1001073-02.*" },
    },
  },
  apply_properties = {
    -- Keep capture at 48k as well.
    ["audio.rate"] = 48000,

    -- Match playback buffering for symmetry.
    ["api.alsa.period-size"] = 128,
    ["api.alsa.period-num"] = 3,
    ["api.alsa.headroom"] = 64,

    -- Make capture passive so the playback sink drives the graph clock.
    ["node.passive"] = true,
    ["priority.driver"] = 1,
  },
})
