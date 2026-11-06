# gugu

**A travelling pigeon that lives in a tiny round window.**

gugu (咕咕) is an electronic pet for children aged 3–5. It is an anthropomorphic homing pigeon living
in a birdhouse on the child's windowsill, played by an M5Stack StopWatch: a 52 mm round AMOLED with a
microphone, a speaker, a motion sensor and a battery. Gugu leaves for a trip every few days, mails a
postcard home each day it is away, can be phoned any time, and comes back with one small souvenir and
a story. The family picks English, Mandarin or Cantonese; Gugu listens and answers in it, in one
warm, slightly husky voice.

[中文说明 → README_CN.md](README_CN.md)

> Status: **v1.1 "the pigeon that goes out" is code-complete and awaiting the owner's acceptance** (v1.0, the
> talking pigeon, runs on the device since 2026-11-03). v1.1 adds: a story engine that writes a **trip card** every
> few days (a real place, a reason that follows from the last trip, the day plan, child-sized facts, a friend, a
> souvenir, postcards) and a **journey log** Gugu remembers and can be asked about; pictures generated per trip and
> turned into pixel art on the device (a backdrop with four times of day, keyed props, the friend); a touchable
> object layer (lamp, window, props); the yellow gift key gives Gugu something (it falls, Gugu picks it up with its wings and reacts; five quick presses and Gugu puts on a show, a song on a disco stage); ask for a story and Gugu tells one in three parts, with a question for the child between them; the walkie-talkie sounds; 19 expressions; the
> IMU (shake → a bouncing ball, then dizzy, then cross; pick-up → a look). The planning document and the full
> decision log live in [`PLAN.md`](PLAN.md) (Chinese).

## What makes it different

- **Idle by design.** Like Tabikaeru, the fun is in waiting and being surprised. No tasks, no streaks,
  no numbers, no levels. Gugu never dies and never nags.
- **Three languages, one personality.** The child talks by holding a key; the family picks the language
  (Hong Kong Cantonese, Mandarin or English) and the voice for it on the pairing page, where any voice can be tried out
  first. Gugu understands all three and answers only in the chosen one.
- **A story that grows.** Gugu has a seed canon (a grandmother who gave it the little suitcase, animal
  friends in the cities it visits) and a slowly growing one: whatever it says out loud becomes true,
  one or two new facts a week, never contradicting itself.
- **Its job is the mail.** Every few days the model plans a trip card: a letter to deliver in a real city
  (or, now and then, a holiday or a visit to a relative), one to three nights, a plan for each day, five to
  eight facts a four-year-old can hold, the animal friend there, the souvenir. Departure, arrival and the
  return follow the real calendar; the picture follows the real sunrise and sunset where Gugu is. On a call
  Gugu may only retell what the card says. Parents can read and delete the card.
- **It remembers the child.** Names, friends, fears, new skills, brought up naturally days later.
- **Parents see everything, without an account.** Every call transcript, postcard and memory is on
  the device; holding the yellow gift key for three seconds enters pairing mode: a hotspot whose name and password
  show on the screen, and a web page where a parent picks the Wi-Fi, enters the keys, reads, exports or deletes everything.

## How it is built

The whole product is **firmware plus the AI vendors' APIs**. There is no server, proxy, account or app.

```text
 StopWatch (ESP32-S3, ESP-IDF C) ──TLS──▶ MiniMax · Alibaba Qwen · Volcengine · OpenAI · Google Gemini
   ├─ push-to-talk call: ASR → LLM → expression tags → TTS, first sound ≤ 2 s
   ├─ the pet's life: model-planned trip cards, real-time calendar and sun, postcards, souvenirs, memory books, all on LittleFS
   ├─ pixel-art home and generated places on a 466×466 round display, 19 expressions driven by the model
   ├─ pictures per trip: the image role → JPEG decoded and shrunk on the ESP32 → 116-grid palette art, four times of day
   └─ pairing mode: hotspot + web page served by the device: Wi-Fi picker, a vendor per role with its keys (saved and tested at once), the voice per language (try it out), calls, the trip card, the memory books, the event log
```

- **Hardware:** M5Stack StopWatch (SKU C152): ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 466×466 AMOLED,
  ES8311 codec, BMI270, RX8130 RTC, M5PM1 power management, 450 mAh.
- **Firmware:** ESP-IDF v5.5, C, LVGL. One `app.c` assembles flat modules with strictly downward
  dependencies; see [`CLAUDE.md`](CLAUDE.md) for the architecture contract.
- **Vendors (bring your own keys):** one provider protocol, a vendor chosen per role (listening, talking,
  speaking, drawing) on the pairing page: MiniMax (verified live), Alibaba Bailian (Qwen, Qwen realtime),
  Volcengine, OpenAI and Google Gemini (implemented to their public docs; OpenAI and Gemini requests checked
  against the real endpoints, awaiting keys). Endpoints, models, voices, regions and keys are declarative presets in
  `content/providers/`; the code outside the provider module names no vendor. OpenAI and Gemini do not serve mainland
  China, Hong Kong or Macao.
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

Device, from a fresh clone on any Mac (only the Xcode command line tools are needed):

```bash
./gugu.sh                                          # finds the StopWatch, builds, flashes, shows the boot log
```

The first run clones ESP-IDF v5.5.6 and its toolchain into `.derived_data/` inside the repository (~5 GB on disk,
about 10 minutes); nothing is installed elsewhere, and deleting the repository removes the environment. Flashing
keeps the Wi-Fi credentials, API keys and recorded calls already on the device. Then, over the USB console:

```bash
.derived_data/espressif/python_env/*/bin/python tools/dev/serial_cmd.py --port /dev/cu.usbmodem* \
    "wifi <ssid> <password>" "key minimax <api key>" "replay /content/test/zh_01.wav"
```

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
| v1.1 | The pigeon that goes out: trip cards and the journey log (A, A2), the generated-art pipeline (C), the object layer (B), keys and sounds (E), the IMU (D), the screenshot and trip-walk self-tests (F) — code complete 2026-11-03, acceptance report in PLAN.md |

## License

To be decided by the owner.
