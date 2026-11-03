# Sampled system sounds

`tools/sounds/make_sounds.py` deterministically renders the SoundSenses palette
into `tools/sounds/out/`: 19 mono PCM16 WAVs at **32 kHz**, an alarm fade-up
preview, a machine-readable contact listing, and a 344,008-byte `ORBSOUND` section.
Ticks use filtered noise, airy cues use enveloped noise, and chimes use short
inharmonic FM/additive strikes with exponential decays. Every packed source has
zero integer DC, zero endpoints and a -1 dBFS peak. The timer/alarm sources are
2-second loops; the mixer fades them up over 20 seconds and repeats until stopped.
Spoken replies come from the hub rather than this generated palette.

`scripts/fonts.sh assets --version N` regenerates the palette, bundles fonts and
sounds in the v2 signed envelope, and uses the existing RSA-3072 Secure Boot v2
signing path. The entire bundle, its section offsets and bytes are authenticated.
V1 font-only envelopes remain readable. New loads validate non-overlapping
section bounds and the compiled font identity; malformed/missing sound data
fails silent. A worker copies authenticated PCM into PSRAM before accepting
palette requests. That copy stays valid across an assets OTA erase/write.

`orb_sound_play()` queues without allocation, locks, I2C, flash reads, NVS or clock
conversion. Two system voices mix in 5 ms blocks with 6 dB per-voice headroom;
repeated cues vary subtly in level. Speech is a bounded pull callback into the
same continuous output owner, and ducks every system sound by 14 dB. Speech's
0.75 gain plus both ducked system voices stays below full scale. Hardware volume
uses the ES8311's digital dB mapping, click-free ramp and DRC/limiter through the
board API. The codec stays open between cues to avoid turn-on latency.

Silent and Quiet Hours mute ordinary cues and spoken replies; **timer and alarm
remain audible**. Face-down, Always On and volume zero mute everything, cancel
queued/active sounds, and never resurrect a loop after waking. UI code explicitly
calls `orb_sound_stop(ORB_SOUND_TIMER/ALARM)` when dismissing the alert. The legacy
semantic cue names and preferences API remain source-compatible. Every sound
still needs simultaneous visible feedback (hig.md §8); this component adds no UI.
The explicitly requested SoundSenses durations include 160/180/240 ms cues;
these exceed hig.md's older 150 ms guideline and follow the owner's exact palette.

Volume 0–100, Silent, spoken replies and Quiet Hours persist in NVS namespace
`orb_sound`, in the existing `preferences` u32. Silent adds bit 19; older saved
values therefore keep Silent off without changing their other choices. Defaults
are 60%, Silent off, spoken replies on, and 22:00–07:00 with Quiet Hours disabled.
Preferences load on the main service owner before audio/UI startup. The shell
restores Silent through `orb_shell_set_silent` when its workspace is created. Face-down
and Always On are transient power policy and are never stored as Silent.
UI setters update atomics immediately;
the existing internal-stack main service commits after a 500 ms debounce, keeping
NVS off the PSRAM audio stack and UI frame deadline. Requests wake main and its
wait includes the save deadline even during minimal idle service. Save errors
are exposed in `orb_sound_preferences()`.

The sound worker owns both 32 kHz microphones and speaker. RX reads have zero
wait, so missing mic data cannot stall sound output or recording takeover. The
speech service receives stereo PCM plus the known playback RMS envelope. A
recording request cancels pending/active sounds, closes duplex at the next block,
and takes an exclusive lease with a 160 ms ceiling; release resumes output.
No cues from before recording replay afterwards.

Static sample memory is 344,008 bytes in PSRAM. The audio stack is also PSRAM:
6,144 bytes; its TCB remains internal. Persistence reuses main rather
than allocating the former 3,072-byte preference stack and its TCB.
Audio-local buffers are bounded (320 B output, 320 B speech, 640 B stereo;
mixer stack adds 640 B accumulation and 320 B source scratch). DMA sizes come
from the board component, not these buffers.

`orb_sound_stats()` reports readiness only after the palette and output device
are ready, accepted/started/dropped requests, and last/max queue-to-first-codec-
submission latency. This timestamp is a **submission proxy, not output onset**.
The 5 ms mixer period and short DMA queue target <15 ms, but scheduler, codec,
acoustic onset, speaker loudness and interrupt behavior must be measured on the
dev board. No such device measurements were taken in this no-board stream.

Host checks:

```
make -C firmware/components/orb_sound/tests/host -j4 test
make -C firmware/components/orb_fonts/tests/host -j4 test
pytest tools/sounds/tests/test_sounds.py
pytest tools/fonts/tests/test_assets.py -k 'not loader and not rescue and not builtin'
```

The C sound suite covers signed PCM contract, 2-voice sum/headroom, ducking,
looping/ramp/stop, malformed directories, silent/alert/Always On/capture gates,
rapid mute toggles, preference clamps and concurrent leases under ASan/UBSan.
The storage suite checks worker-to-main handoff, exact debounce/coalescing,
commit races, explicit errors, and all Sound choices surviving a simulated reboot.
Assets tests verify v1/v2 bounds, overlapping sections, PSRAM copy failure and
real disposable-key SBv2 signature rejection after changing a sound-section byte.
