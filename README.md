# gugu

**A travelling pigeon that lives in a tiny round window.**

gugu (咕咕) is an electronic pet for children aged 3–5. It is an anthropomorphic homing pigeon living
in a birdhouse on the child's windowsill, played by an M5Stack StopWatch: a 52 mm round AMOLED with a
microphone, a speaker, a motion sensor and a battery. Gugu leaves for a trip every few days, mails a
postcard home each day it is away, can be phoned any time, and comes back with one small souvenir and
a story. The child speaks English, Mandarin or Cantonese; Gugu answers in the child's language, in one
warm, slightly husky voice.

[中文说明 → README_CN.md](README_CN.md)

> Status: **prototype v1.0 under acceptance.** The full push-to-talk conversation runs end to end on the
> StopWatch over home Wi-Fi against MiniMax in three languages (first sound P50 2.7 s, target 2 s) and in
> the host simulator. The planning document and the full decision log live in [`PLAN.md`](PLAN.md) (Chinese).

## What makes it different

- **Idle by design.** Like Tabikaeru, the fun is in waiting and being surprised. No tasks, no streaks,
  no numbers, no levels. Gugu never dies and never nags.
- **One voice, three languages.** The child talks by holding a key; Gugu replies in colloquial
  Cantonese, Mandarin or English with the same voice and the same personality.
- **A story that grows.** Gugu has a seed canon (a grandmother who gave it the little suitcase, animal
  friends in the cities it visits) and a slowly growing one: whatever it says out loud becomes true,
  one or two new facts a week, never contradicting itself.
- **It remembers the child.** Names, friends, fears, new skills, brought up naturally days later.
- **Parents see everything, without an account.** Every call transcript, postcard and memory is on
  the device; a hotspot and a web page let a parent read, export or delete it.

## How it is built

The whole product is **firmware plus the AI vendors' APIs**. There is no server, proxy, account or app.

```text
 StopWatch (ESP32-S3, ESP-IDF C) ──TLS──▶ MiniMax · Alibaba Qwen · Volcengine
   ├─ push-to-talk call: ASR → LLM → expression tags → TTS, first sound ≤ 2 s
   ├─ the pet's life: trip calendar, postcards, souvenirs, memory books, all on LittleFS
   ├─ pixel-art home on a 466×466 round display, 13 expressions driven by the model
   └─ parent mode: hotspot + captive portal + web page served by the device
```

- **Hardware:** M5Stack StopWatch (SKU C152): ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 466×466 AMOLED,
  ES8311 codec, BMI270, RX8130 RTC, M5PM1 power management, 450 mAh.
- **Firmware:** ESP-IDF v5.5, C, LVGL. One `app.c` assembles flat modules with strictly downward
  dependencies; see [`CLAUDE.md`](CLAUDE.md) for the architecture contract.
- **Vendors (bring your own keys):** a single provider protocol with MiniMax first (verified live) and
  Alibaba Bailian (Qwen, Qwen realtime) and Volcengine as alternates (implemented to their public
  protocols, awaiting keys), selectable per role (ASR / chat / TTS / speech-to-speech / image) from
  declarative presets in `content/providers/`.
- **Mind without a framework:** four fixed loops (a conversation turn, a post-call extraction, a
  daily life tick, a weekly reflection), three small memory books kept in the prompt. No agent
  framework, no vector database.

## Repository layout

```text
app/            firmware: app.c + one directory per module (board, net, store, audio, ui,
                provider, world, mind, engine, call, instinct, life, parent) + foundation/
host/           the same core built for the Mac/Linux host, with a terminal front-end
test/           unit tests and fixtures (speech samples, vendor replays, golden tags)
content/        prompts, provider presets, place packs, sound effects, parent web → LittleFS image
assets/         art sources and the pixel-art generator; assets/samples/ is the current review set
tools/          dependency checker, replay harness, asset converters, one-off evaluation scripts
PLAN.md         planning document and dated log (Chinese)
```

## Building

Device (ESP-IDF v5.5, macOS or Linux):

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3 && idf.py build          # firmware + content partition image
idf.py -p /dev/cu.usbmodem83401 flash              # then over the USB console:
python3 tools/dev/serial_cmd.py "wifi <ssid> <password>" "key minimax <api key>" "replay /content/test/zh_01.wav"
```

A Mac without ESP-IDF flashes from a bundle made on the build machine: `tools/release/make_bundle.sh`
produces `dist/gugu-<version>.zip`; unzip it anywhere and run `./flash.sh` (needs only Python 3; keeps
Wi-Fi, keys and data), then `./console.sh "stat"`.

Simulator and tests (plain CMake; needs SDL2, libcurl, OpenSSL, and one `idf.py reconfigure` so LVGL is downloaded):

```bash
cmake -S host -B host/build && cmake --build host/build -j8
./host/build/gugu_tests                                           # unit tests
GUGU_MINIMAX_API_KEY=… ./host/build/gugu_host                     # the pigeon in a window; B/Y keys, mouse = touch
GUGU_MINIMAX_API_KEY=… ./host/build/gugu_host --replay test/fixtures/speech/zh_01.wav --turns 3 --quit-after 120000
python3 tools/dev/replay_report.py host/data                      # latency / cost report
```

See [`CLAUDE.md`](CLAUDE.md) for the module rules and the full tool list.

## Roadmap

| Milestone | Goal |
|---|---|
| M0 | Toolchain, repository scaffold, host build with unit tests — done |
| M1 | One full push-to-talk turn against MiniMax on the host build; tags, prompts, three languages; replay report — done |
| M2 | Real device: concurrent TLS connections, pre-warm, keep-alive reuse; RAM and latency numbers — done (reuse verified, in-call internal RAM ~104 KB steady, 22 KB low-water) |
| M3 | The same core running a turn on the device, with the serial test console — done (10 live turns in three languages) |
| M4 | The shell: idle behaviours, animation table, local sounds, keys — done |
| M5 | Alternate vendors, config switching, cost ledger, minimal parent page — done (Bailian vendors compiled, not live-tested) |
| M6 | Acceptance run: first sound ≤ 2 s, one voice across three languages, tag hit rate, resource headroom — device numbers in PLAN.md: first sound P50 2.67 s / P90 3.99 s (target 2 / 3), tags 10/10 valid, headroom steady but transiently low; voice blind test pending |

## License

To be decided by the owner.
