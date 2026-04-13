



# thermoCat

> *The thermometer for people who love cats and hate taking their own temperature.* Point it at your forehead and instead of a cold clinical number you get a purr, a meow, or an angry hiss. Fever? The cat will let you know — loudly.

ESP32 device that plays cat sounds (**purr / meow / hiss**) based on the temperature measured by an infrared sensor, with MQTT telemetry and remote volume control.

https://github.com/user-attachments/assets/72bdd09d-ae5b-4d37-b40d-60c8baf312e8

## Features

- **Temperature-driven sound behavior** — an MLX90614 IR thermometer reads the temperature of the pointed object; the firmware plays purring, meowing, or hissing depending on the value, with smooth crossfades between registers.
- **Real PCM samples** — all sounds are real recordings (CC0 / Public Domain), stored as `int8_t PROGMEM` arrays compiled into the firmware. No synthesis, no clicks.
- **Dual-core audio** — the audio generator runs on core 1 (I2S DMA, 22050 Hz) while temperature reading and networking run on core 0.
- **WiFi with NVS provisioning** — credentials are stored in the ESP32 NVS partition. On first boot (or after a failure) the firmware prompts for SSID/password over the serial port.
- **MQTT over TLS** — publishes name, build timestamp, and object/ambient temperatures (retained); subscribes to a volume command topic (0–100).
- **Speaker response compensation** — a per-band gain table flattens the speaker's frequency response (see below).

## Temperature → sound mapping

| Temperature | Sound | Behavior |
|---|---|---|
| `< 20 °C` | Purr | Relaxed cat |
| `20 – 24 °C` | Purr + Meow (crossfade) | Transition |
| `24 – 34 °C` | Meow | Cat asking for attention |
| `34 – 38 °C` | Meow + Hiss (crossfade) | Transition |
| `> 38 °C` | Hiss | Irritated cat |

## Hardware

- **MCU:** ESP32-DevKit V1 (ESP32, dual-core)
- **IR thermometer:** MLX90614 (I²C)
- **Audio DAC / amp:** MAX98357A (I²S, 3 W Class-D)
- **Speaker:** small full-range driver (pre-characterized, see below)

### Wiring

**ESP32 → MLX90614 (I²C)**

| ESP32 | MLX90614 | Signal |
|---|---|---|
| GPIO21 | SDA | I²C data |
| GPIO22 | SCL | I²C clock |
| 3.3 V | VCC | Power |
| GND | GND | Ground |

**ESP32 → MAX98357A (I²S)**

| ESP32 | MAX98357A | Signal |
|---|---|---|
| GPIO26 | BCLK | Bit clock |
| GPIO25 | LRC | Word select |
| GPIO27 | DIN | Data out |
| 3.3 V / 5 V | VIN | Power |
| GND | GND | Ground |
| — | SD | Leave floating (keeps the amp enabled) |

See [`wiring.txt`](wiring.txt) for the plain-text summary.

## MQTT topics

The topic prefix is set via `START_TOPIC` in `src/mqttCred.h` (e.g. `esp32`).

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `<prefix>/cat/system/name` | Pub | yes | `"thermoCat"` |
| `<prefix>/cat/system/compiled` | Pub | yes | `__DATE__ __TIME__` of the build |
| `<prefix>/cat/system/Tir` | Pub | yes | Object temperature, °C (1 decimal) |
| `<prefix>/cat/system/Tambient` | Pub | yes | Ambient temperature, °C (1 decimal) |
| `<prefix>/cat/cmd/volume` | Sub | — | Integer 0–100 (ASCII) |

Temperatures are published on change (≥ 0.5 °C) or every 60 s, whichever comes first. The TLS connection uses `WiFiClientSecure::setInsecure()` (no certificate validation) on port 8883. The reconnect attempt is throttled to at most once every 5 s.

## Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
cp src/mqttCred.h.example src/mqttCred.h
# edit src/mqttCred.h with your broker details

./build.sh     # pio run -e esp32
./upload.sh    # pio run -e esp32 -t upload
```

On first boot, if no WiFi credentials are stored in NVS, the firmware will prompt for SSID and password over the serial console (115200 baud).

### Serial commands

| Key | Action |
|---|---|
| `>` | Volume +5 % |
| `<` | Volume −5 % |

Default volume is 15 %.

## Speaker characterization

The loudspeaker used in this build was characterized in a separate project. The resulting 1/3-octave response is stored in [`C2_F3a_freq_response.json`](C2_F3a_freq_response.json) and compiled into [`include/freq_compensation.h`](include/freq_compensation.h) as a per-band gain table applied at playback time to flatten the output.

## Project layout

```
thermoCat/
├── platformio.ini
├── build.sh
├── upload.sh
├── wiring.txt
├── C2_F3a_freq_response.json     # speaker measurement data
├── src/
│   ├── main.cpp
│   └── mqttCred.h.example
└── include/
    ├── freq_compensation.h
    ├── purr_sample.h              # 6 real purring samples
    ├── meow_sample.h              # 12 real meow samples
    └── hiss_sample.h              # 9 real hissing samples
```

## License

- **Code:** GPL-3.0-or-later (see [`LICENSE`](LICENSE))
- **Audio samples:** CC0 / Public Domain

## Author

ghedo (luca.ghedini@gmail.com) — 2026

Built with [Claude Code](https://claude.ai/claude-code) by Anthropic.

## Development effort

This project was built entirely through a conversation with Claude Code. The numbers below are extracted from the local session transcripts (`~/.claude/projects/.../*.jsonl`).

- **First message:** 2026-03-21 10:14 UTC
- **Last message:** 2026-04-13 19:24 UTC
- **Calendar span:** ~23 days, 2 sessions, 432 messages (153 user + 189 assistant)
- **Active conversation time: ~50 minutes**

*How active time is computed:* every message in the transcripts carries a timestamp. The timestamps are sorted and the gap between each consecutive pair is measured. Only gaps shorter than or equal to 5 minutes are summed, the rest are discarded. The idea is to count the time when a conversation was actually in progress (including natural pauses for reading, compiling, flashing, checking the hardware) while ignoring long idle periods (overnight breaks, days off, switching to other work). The 5-minute threshold is a rough but reasonable compromise; raising or lowering it shifts the total accordingly.

### Tokens

Cumulative token counts across both sessions:

| Metric | Tokens |
|---|---:|
| Input (non-cache) | 545 |
| Output | 74 995 |
| Cache write | 666 941 |
| Cache read | 9 731 021 |
| **Total** | **~10.5 M** |

Cache-read tokens dominate because every turn re-reads the existing context from the prompt cache (this is the standard accounting for cached prompts: the tokens are billed at a reduced rate but still counted). The "real" output produced by the model is ~75 k tokens; new context accumulated into the cache is ~667 k tokens.
