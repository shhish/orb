# Push-to-talk service

`orb_voice_start()` starts one core-0 worker after `orb_model_init()` and before
`orb_link_start()`. Input calls `orb_voice_boot(held)` on sampled BOOT edges.
The service makes no LVGL calls. A 350 ms hold starts capture to the model's
current voice focus (default `dot`); release stops. A shorter tap cancels a
pending recording or transcript. UI controls enqueue START/STOP/CANCEL/CONFIRM
with `orb_model_voice_request()`. START can name a session or Dot and an optional
reply-enabled card ID. The shell must set focus when navigation changes and
render `snapshot.voice` (phase, persistent recording indicator, 0–100 level,
transcript, bounded error code and safe error copy) with explicit Confirm and Cancel controls. Nothing
sends merely because transcription finished, and confirmation never expires.

The ES7210 reads both microphone slots at 16 kHz, signed interleaved PCM16LE.
`mic_dsp_mono()` filters DC and treble on each channel, averages both channels,
and applies a soft adaptive noise gate before upstream framing. Each 20 ms chunk
contains 640 sample bytes and gets the protocol's seven-byte network-order
header. The service only reads capture after `voice.begin` succeeds. Each new
recording drops the first 3,200 sample bytes (100 ms) to remove the ES7210
power-up click before framing or metering. Short reads count toward that sample
budget, and a read crossing the boundary retains its remaining PCM with sequence
zero. Audio frames
and `voice.end` use the same FIFO transport; each submission names its original
connection generation. Capture stops at 60 seconds/1,920,000 PCM bytes and
transcription gets a 130 second local budget (120 second hub result deadline
plus request transit). Reads have a 40 ms driver deadline so cancellation
remains responsive if the clock stops. A short read remains valid even PCM;
read failures, queue saturation, invalid UTF-8, disconnect or hub errors cancel
and fail closed. BOOT release while begin is still pending cancels empty capture.

The microphone and unopened speaker wrapper share a board-owned graph.
Construction failures unwind immediately. The worker deletes wrappers before
codec/control/data interfaces, then stops and deletes both I2S channels when
capture stops. Therefore transcription and confirmation retain no audio DMA.
The board configures two descriptors of 80 stereo frames per direction for a
short output queue; inspect `board_ws175c.c` for the current shared DMA budget.
Driver descriptors and state consume additional internal memory. The worker has
a 4,096 byte internal task stack. Capture/serialized-frame scratch, the core
state, and the eight-entry callback event payload queue are explicitly in PSRAM;
the link separately owns its bounded PSRAM audio copies. This is an allocation
budget, not measured device heap or latency evidence.

Cancellation retries when local queue admission fails or its acknowledgment
fails. Another recording waits until cancellation is acknowledged or the
connection ends. Late replies/results are discarded by connection and stream
identity. A confirmed card reply transfers to the model's durable answer queue;
a session/Dot confirmation sends `message.send` once. An ambiguous message
result retains its transcript with “Delivery unconfirmed” and never silently
replays a new turn. Errors and diagnostic phase logs contain no transcript,
provider content, target/card IDs or local paths. Hub `voice.error` and failed
request codes cross the worker queue into `snapshot.voice.error_code`; provider
messages are never copied. `no_speech` keeps the retryable error phase with
“Hold and speak”; unsupported, timeout, not-listening and connection failures
retain distinct safe copy. A new recording clears the old error code. Capture preempts system playback; completing transcription never sends a request
or starts a consequential action.

Host verification:

```sh
make -C firmware/components/orb_voice/tests/host test
make -C firmware/components/orb_board/tests/host test_audio \
  CFLAGS='-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined'
```

`voice_core.c` is the same platform-free state machine compiled into firmware.
The fake source tests cover frame boundaries/header bytes, level, capture and
cleanup, confirm-only sending, card handoff, cancel retry, stale callbacks,
reconnect, failed initialization/read/transport, UTF-8 limits and both deadlines.
They also verify exact ADC warm-up trimming across short/empty reads, per-start
reset, no-speech retry, bounded error codes and production callback mapping.
Real microphone sensitivity, the ES7210 slot, heap under combined OTA/voice,
Wi-Fi timing and Whisper remain board end-to-end validation for the lead.

### Microphone level profile (2026-10-28)

The lead's raw two-mic recording at PGA 30 dB / digital 0 dB was clean but
quiet: speech RMS about -52 dBFS, peaks about -31 dBFS. All three consumers
(push-to-talk, spoken-reply interruption and `devtools.micrecord`) now call
`orb_board_audio_set_mic_gain()` after opening the ADC. It sets both PGAs to
36 dB and both digital gains to +16 dB. The additional 22 dB projects that
same recording to roughly -30 dBFS RMS / -9 dBFS peak, leaving 9 dB headroom.
The analogue increase uses most of the available gain without selecting its
highest setting; fixed digital gain supplies the rest without AGC pumping.
No software amplification follows the ADC.

The [Everest ES7210 rev21 register map](https://files.waveshare.com/wiki/common/ES7210_DS.pdf)
(pp16,18-19,22) defines PGA bits 3:0 at 0x43/44 (code 13 = 36 dB) and digital
gain at 0x1E/1D for ADC1/2 (0xDF = +16 dB). ADC12 ALC is disabled, unrelated
bits and ADC3/4 are preserved, and direct gain uses a soft ramp. This is the
analogue microphone path; the DMIC boost register does not apply.

`orb_mic_levels.h` also supplies the 12.589× amplitude ratio to DSP. Noise
floor/regularization and the echo coupling bound scale with that ratio; noise
starts at the existing minimum rather than an assumed room level. The old
-32 dBFS RMS interruption floor exceeded the measured speech. Its replacement
is -34.5 dBFS at the new gain (4.5 dB below the projected speech RMS), plus the
existing 4× noise floor, learned echo energy margin, both-mic check and 120 ms
sustain requirement. Host fixtures cover the target peak range, projected
speech RMS, echo-only playback and a typing transient.

These are level projections, not a new acoustic measurement. The lead must
repeat the raw stereo capture with ordinary speech at arm's length, inspect
both channels for -12 to -6 dBFS peaks and zero clipped samples, then check
louder speech and talk-over/typing at actual speaker volumes before release.
That physical gate cannot be established by a simulator or a synthetic signal.

## Spoken replies

`orb_voice_speech_status()` provides captions independently of capture state;
`orb_voice_speech_stop(ORB_SPEECH_STOP / ORB_SPEECH_DOUBLE_TAP / ORB_SPEECH_TALK)`
cancels locally without waiting for the hub. No UI or action is implemented here.
Captions remain available in Silent; a reconnect clears them. Never treat a
caption as a card answer, Home write, or outgoing message.

The hello speech descriptor and `voice.speak` wire contract are in
`docs/protocol.md`. The link reassembles bounded 1031-byte audio messages and
queues them in order with JSON begin/end events. `speech_core.c` uses a 24,000
byte PSRAM PCM ring (500 ms at 24 kHz; admission also caps 16 kHz at 500 ms),
150 ms prebuffer, sequence checks and linear resampling to the sound worker's
32 kHz output. Underruns output silence and rebuffer; one second without input
fails closed. Short ended replies can start below the prebuffer threshold.

`orb_sound` owns duplex output and ducks palette sounds while this source runs.
Its microphone callback provides both 32 kHz slots and the known output RMS.
No ESP-SR/AEC component is present in the offline managed dependencies. The
fallback requires 120 ms of excess microphone energy above a learned output
coupling, after 200 ms calibration. This is conservative talk-to-interrupt,
not echo cancellation; speaker coupling, changing volume and real desk typing
must be validated acoustically on the dev board.

`Link.speech.start(async_pcm_chunks, rate=16000|24000, text=caption)` is the hub
transport integration point. It accepts Mac-rendered PCM (a voice provider is
outside this device stream), paces packets, and cancels a producing task when a
matching stopped notification arrives. It never starts a TTS process or touches
running owner sessions. Host tests use deterministic PCM only.

`speech_core_t` is 25,088 bytes on the host (including ring/caption); the device
ABI may differ. No device latency or heap measurement has been made. The pure
speech/DSP tests cover prebuffering, underruns, sequence errors, bounds, stop,
UTF-8, resampling, both-slot mixing, DC removal, echo-only input, typing bursts
and sustained talk. `hub/tests/test_speech.py` checks exact binary wire vectors,
cancellation and format bounds.
