# orb_board

The hardware under the OS. One implementation: Waveshare
ESP32-S3-Touch-AMOLED-1.75C (`board_ws175c.c`): CO5300 AMOLED over QSPI,
CST9217 touch, ES8311 + ES7210 audio, AXP2101 power chip, QMI8658 motion
sensor, PWR and BOOT.

It depends on the panel, touch and codec drivers directly
(`espressif/esp_lcd_co5300`, `waveshare/esp_lcd_touch_cst9217`,
`espressif/esp_codec_dev`), not on Waveshare's BSP, which also pulled in an
LVGL adapter, FreeType, libpng, a JPEG decoder and USB; pins and the touch and
audio set-up follow that BSP (Apache-2.0).

## API

`include/orb_board.h`:

- `orb_board_init()`: I2C, buttons, power chip, motion sensor.
- **Panel.** `orb_board_panel_start(max_transfer, done, arg)` runs the vendor
  init sequence through esp_lcd's CO5300 driver, then hands the chip select to
  Orb's own SPI device. `orb_board_panel_write(x1, y1, x2, y2, px, bytes)`
  queues a window as one chain (column address, row address, pixels in chunks
  of up to 32 KB with the chip held selected) and returns at once; `done` runs
  in an interrupt when the pixels are out. esp_lcd sent each command as two
  blocking transactions of ~45 us each, seven per band: 1.5-2 ms of a frame's
  wire time, now gone (a full frame went from 15.0 to 10.7 ms).
  `orb_board_panel_brightness(pct)`.
- `orb_board_touch_new(&tp, on_report, arg)`: the CST9217 (mounted turned
  half a turn, as the panel); `on_report` runs in an interrupt when it has a
  new report.
- `orb_board_audio_new(rate, &speaker, &mic)`: one duplex I2S port, the
  ES8311 (speaker, amplifier on GPIO 46) and the ES7210 (two microphones), as
  esp_codec_dev handles. Brought up and opened on the device; not used yet.
- `orb_board_buttons()`, `orb_board_power()`, `orb_board_power_off()`.
- `orb_board_accel()`: specific force in g on screen axes (+x right, +y up,
  +z out of the screen), pointing away from Earth at rest. Downhill gravity
  opposes this vector; the shared glass light uses it as the up vector.

## The motion sensor

- **Scale.** The QMI8658 read ~0.46 g lying flat although CTRL2 said +-4 g
  (8192 LSB/g, as the datasheet's sensitivity table and register map have it,
  CTRL2 bits 6:4 = 001). The power chip keeps the sensor powered across MCU
  resets and reflashes, so state the previous firmware left in it survived;
  restoring the CTRL registers the old firmware had used (CTRL3 0x43, CTRL5
  0x03, gyro on) didn't reproduce the error, so it sits outside them (the
  datasheet's CTRL9 commands, e.g. 0x09 accelerometer host delta offset, keep
  state until a reset). `qmi8658_init()` now soft-resets the sensor first
  (0xB0 to register 0x60, then waits for 0x80 in 0x4D, at most 15 ms): the same
  board reads 0.93 g (z 0.90).
- **Axes.** The sensor sits half a turn about z from the screen, like the
  panel and touch (both mirrored in x and y): `orb_imu_to_screen()` negates x
  and y. That's what the glass highlight, checked by eye with the device
  upright, needed; lying face up, z reads +1 g.

`orb_imu.c` (no ESP-IDF): decoding and axis mapping, tested on the host.

### Sound and senses hardware support

`orb_board_motion()` reads acceleration and gyro together. CTRL2=0x15,
CTRL3=0x55, CTRL7=0x03 select ±4g, ±512°/s and 224.2Hz in 6DOF mode.
`orb_senses_update()` is pure C and allocation-free (88-byte state on the
host). Feed it each existing 50Hz poll, with the current touch state. Its
12° detents belong to the **same rotary owner as the rim**. It emits only
an action-free `double_tap` observation: the shell must explicitly allow a
safe stop/dismiss action. No card, Home, or voice-send callback belongs here.

`orb_board_motion_configure(active, taps)` selects that full profile for an
intentional Active view. Otherwise the gyro is disabled. With no tap consumer,
CTRL2=0x1d and CTRL7=0x01 select the 21Hz low-power accelerometer, preserving the
20Hz pick-up/face-down stream. When a screen, spoken reply, or finished timer
can consume a safe double-tap, the accelerometer alone stays at 250Hz: QMI8658A
Rev A §10.4 recommends >200Hz for tap detection, and its sample-count windows
are rescaled to retain their durations. This is an explicit exception to the
low accelerometer rate; the gyro remains off. Dark idle without those consumers
disables the tap engine. This never synthesizes an action or wake contact.

Mode writes disable both sensors through CTRL7 first and allow up to 2/ODR for
them to stop. Fresh samples are withheld for 150ms after accel changes and
200ms after gyro enable. A generation change requires consumers to discard
gesture accumulation and restart bias learning before accepting twist/game
input. A failed savings reconfiguration attempts the known full-rate profile;
failure to restore is reported as Unknown and retried after one second. Failed
samples cannot prove pick-up dwell. No host test establishes hardware settling,
real tap sensitivity, or the resulting current.

PWR and BOOT any-edge ISRs retain timestamped physical masks in a 32-entry ring
and notify the service task. The task drains/replays those edges through its
existing debounce recognizer, so a whole short press between 50ms IMU polls is
retained. The PMU's ten-second hardware power cut is unchanged.

### Idle audio transport

`board_audio.c` owns the existing codec/I2S lifetime. The sound task now sends
a 100ms silence tail after the last cue or speech, digitally mutes the speaker,
turns off the PA, disables both I2S channels and detaches the external MCLK GPIO.
It blocks on producer notifications while idle. Resume restores the MCLK route,
starts the channels and primes silent DMA before unmuting the PA (about 5ms of
sample capacity at 32kHz, with a finite 10ms write deadline). Recording still
uses its exclusive audio lease and normal constructor/teardown.

The codecs' analog bias and ALDO1 supply remain configured while sound is idle.
Full analog suspend/resume needs measured pop and short-cue latency validation;
this change implements the bounded PA/I2S/MCLK portion only. Host tests verify
dispatch/order and resource ownership, not electrical clock state or audibility.

The QMI8658A tap engine runs between host polls. Its CTRL9 configuration
uses the polled CmdDone handshake, so unwired interrupt pins are not an
obstacle. STATUS1/TAP_STATUS distinguish double taps. A guarded sampled
detector remains available when a sensor revision rejects tap configuration.
Both paths require desk qualification and quiet, reject touch and movement,
and apply a refractory interval. Desk transfer response and gyro sign still
require the lead's board validation; synthetic traces are not that evidence.

`orb_board_battery_detail()` reads the five ADC channels, source settings,
charge direction, and configured charge current/voltage. ADC masks are
14-bit, including high register bits 5:0. TS exposes microvolts and NTC
resistance. The schematic identifies RP4 as a 10k thermistor but omits its
curve; `orb_battery_temperature()` converts only when the caller supplies
verified R25 and beta. `temperature_valid` remains false otherwise.

**Audit correction:** AXP2101's public map has no instantaneous battery
current magnitude, cycle count, coulomb counter, or full-charge capacity
readout. Those health/current values remain unavailable with explicit flags;
charge-current *setting* is not charge-current telemetry. Preserve raw
voltage, SOC and TS history for a future calibrated health model.

The opt-in `orb_board_set_desk_charge_limit()` setting persists in NVS.
Call `orb_board_battery_desk_poll()` from the board service at least once a
second with the last resting state. After two hours continuously resting on
VBUS, the policy disables cell charging at ≥80%, resumes at ≤75% or when
picked up/unplugged, and preserves other PMU bits and vendor safety limits.
An ownership marker restores charging across MCU resets only when this
policy previously suspended it. This is a software SOC limit, not a PMU
hardware 80% termination setting; gauge accuracy and reboot reset of the
two-hour observation window affect it. Private static PMU and IMU mutexes
serialize service/devtools calls, including each read/modify/write sequence.

Primary register references:

- [QST QMI8658A Rev A](https://files.waveshare.com/wiki/common/QMI8658A_Datasheet_Rev_A.pdf),
  tables 22/24/25/36, sections 5.10.4 and 10: gyro, tap configuration and polling.
- [X-Powers AXP2101 V1.4](https://datasheet.lcsc.com/datasheet/pdf/4f32bb3c83a61e426b4e09c536b24324.pdf?productCode=C3036461),
  sections 6.10/6.11 and registers 0x18/0x30/0x34..0x3d/0x50/0x62/0x64.
- [Waveshare 1.75C schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75C/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf),
  page 1: RP4 10k NTC and two-wire battery connector.

## Required sdkconfig

None of its own beyond the board's (`firmware/sdkconfig.defaults`: octal
PSRAM, 32 MB flash).

## Integration

Call `orb_board_init()` first. `orb_board_panel_start()` must run on the task
that will write to the panel (the SPI interrupt lands on its core);
`orb_display_start()` does this.

## Tests

- Host: `make -C tests/host test` (macOS clang).
- Build: `test_app/` (`idf.py -C test_app set-target esp32s3 build`), which
  logs buttons, battery and gravity once a second.
