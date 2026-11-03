# gugu Agent Guide

**This file is an index, not a manual.** It carries the shape of the project and points at the file
that decides each thing. The reasoning behind a decision lives next to the code that enforces it, or,
for product decisions, in `PLAN.md`. Do not restate either here.

## Scope

gugu is an electronic pet for 3–5 year olds: an anthropomorphic homing pigeon named **咕咕 (Gugu)**
that lives on an M5Stack StopWatch (ESP32-S3R8, 466×466 round AMOLED, mic, speaker, IMU, RTC,
450 mAh). Its job is delivering letters: every few days a model-written **trip card** (`data/trips/current.json`,
planned by `life`, kept by `world`) sends it to a real place for one to three nights (a delivery, a holiday or a
visit); the screen follows where it is and the real time of day there; it mails a postcard home every day and brings
back one souvenir per trip. The child can call it any time (blue key, push-to-talk; the yellow key is for play, and
held for three seconds it toggles pairing mode). The child speaks English, Mandarin or Cantonese; Gugu answers in the
child's language with one voice.

**The application is the firmware.** There is no backend, proxy, account or app. The device talks
to the AI vendors directly over TLS with the family's own API keys, and keeps every byte of state
(settings, memory, transcripts, postcards) on its own flash. The parent configures it in pairing mode: a
hotspot and a web page served by the device (the yellow key held three seconds, on and off).

Two documents sit beside this one:

- `PLAN.md` — the owner's single planning document and dated log, written in Chinese. Product
  rules, the design canon, vendor research, decisions and their reasons all live there. Append dated
  entries under `## 日志`; revise the top sections in place. There is no `docs/` directory.
- `README.md` / `README_CN.md` — the public introduction, English and Chinese.

Most tasks fall into one of four buckets. Identify which before editing:

1. **Provider work** — a vendor's ASR / chat / TTS / S2S / image client, or a preset. Touch
   `app/provider/` and `content/providers/` only.
2. **Behaviour work** — the call state machine, the pet's instincts, the life ticks, the mind.
   Touch the owning module.
3. **Shell work** — board drivers, audio, UI scenes and animation, storage, parent web.
4. **Content work** — prompts, place packs, sound effects, art. Touch `content/` and `assets/`;
   regenerate, do not hand-edit generated files.

If a change makes you reach across many modules, you are probably violating a boundary. Stop and
re-scope.

## Architecture Overview

One ESP-IDF project. All application code is in `app/` (ESP-IDF's *main* component, renamed per the
"Renaming main component" procedure: `EXTRA_COMPONENT_DIRS` + explicit `REQUIRES`), assembled by exactly
one file, `app.c`.
Every sibling directory of `app.c` is a module. Modules depend strictly downward.

```text
                      app.c           assembles and initialises every module; owns no logic
                        │
behaviour   ├─ call      the phone call: PTT state machine, timeouts, quiet hours, fallbacks
            ├─ instinct  the pet when nobody is calling: idle life, touch and pick-up reactions
            └─ life      the pet's calendar in power terms: wake reasons, ticks, post-call jobs
                        │
mind        ├─ engine    one turn of conversation: cascade (ASR→LLM→tags→TTS) or S2S behind one interface
            ├─ mind      prompt blocks, expression tags, language policy, mood, memory books, extraction
            └─ world     trip calendar, festivals, sun times, weather, place packs
                        │
services    ├─ provider  the unified vendor protocol + minimax / qwen / qwen_realtime / volc / fake
            ├─ audio     mic capture, playback with jitter buffer, local sound effects, mp3 decode
            ├─ art       generated pictures → pixel art: JPEG decode + shrink, palettes, keyed props, time-of-day palettes, .art files
            └─ ui        LVGL scenes, the animation table (tag → clip), generated sprites, generated backdrops
                        │
platform    ├─ net       HTTPS keep-alive + SSE, WSS, TLS session reuse, connection pre-warm
            ├─ store     LittleFS files, encrypted NVS, atomic writes, logs, cost ledger, export
            ├─ board     StopWatch HAL: display, touch, keys, codec, IMU, RTC, PMIC power levels
            ├─ parent    hotspot, captive portal, HTTP API and the embedded parent web page
            └─ console   serial test console: provisioning, calls, replays, inspection (dev + acceptance)
                        │
base        foundation   small headerful components, included directly (no umbrella header)
```

| Module | Owns | May include |
|---|---|---|
| `foundation` | log macros, ring buffer, string builder, SSE line parser, hex/base64, wav header, atomic file write, monotonic clock, small typed event queue, the motion detector (`motion.h`: accelerometer samples → shake / pick-up), the log capture (`logcap.h`: every log line also kept in a 64 KB PSRAM ring, device via esp_log's vprintf hook) | — |
| `board` | `board.h`: display, touch, two keys, ES8311 + I2S, BMI270 (`board_imu_esp.c`: 50 Hz accelerometer → `foundation/motion` → shake / pick-up events; the host's S and P keys), RX8130 RTC, M5PM1 power levels and wake reasons, M5IOE1 vibration, battery; `board_host.c` stub for the host build | `foundation` |
| `net` | `net.h`: HTTP client with keep-alive and streaming body (SSE), WebSocket client, TLS session tickets, pre-warm; `net_esp.c` / `net_host.c` backends | `foundation` |
| `store` | `store.h`: LittleFS mount, encrypted NVS for keys and settings, JSON file read/write (tmp + rename), turn log, cost ledger, the event log (`store_event` → `logs/events.log`), the captured log on flash (`store_logs_flush` → `logs/console.log`, every 30 s and at hang-up) and the diagnostic export (`store_export.c`: one text bundle, secrets masked) | `foundation` |
| `audio` | `audio.h`: 16 kHz mono capture to a PSRAM ring, PCM playback with 2–3 frame pre-buffer, local SFX, mp3 decode for postcards, RMS for beak sync | `foundation` `board` `store` |
| `art` | `art.h`: a JPEG from the image role → the 116-grid backdrop (≤20 colours, sky entries, the ground strip the card asks for) or a keyed prop sprite (outlined, with a quality gate); palettes for the four times of day; the `.art` file under `data/art/<trip>/`; the prop catalogue `content/props/catalogue.json`. Decoder: LVGL's bundled TJpgDec, shrinking while decoding | `foundation` `store` |
| `ui` | `ui.h`: LVGL setup, scenes (home / away / call / parent), the animation engine driven by expression tags, sprites generated from `assets/` (scene and base sprite pre-scaled into PSRAM, beak/eyelid overlay patches), `ui_show_backdrop` for a generated scene, the object layer (`ui_scene.c`: props and the friend on the ground line, built-in or generated sprites, hit test, behaviours wiggle / toggle / wave / bounce / sound, time-of-day scheduling, the home's lamp and window pane, accessories on the pigeon) | `foundation` `board` `store` `art` |
| `provider` | `provider.h`: the vendor protocol (roles ASR / CHAT / TTS / S2S / IMAGE, session shapes, events, errors, usage, per-turn cost); `provider_minimax.c` (live-verified), `provider_qwen.c`, `provider_qwen_realtime.c`, `provider_volc.c` (written to the public protocols, not yet exercised live), `provider_fake.c`; wire codecs `provider_wire_anthropic.c`, `provider_wire_openai.c`; `provider_preset.c` for `content/providers/*.json` | `foundation` `net` `store` |
| `world` | `world.h`: the trip card (JSON under `data/trips/`), the trip phase as a pure function of card + clock, sunrise/sunset and time of day at the pigeon's location, the shiftable clock the console and tests use; festivals and weather later | `foundation` `net` `store` |
| `mind` | `mind.h`: the four prompt blocks, tag vocabulary and streaming parser, language policy, mood baseline, the three memory books (`mind_memory.c`: the journey log `memory/journey.jsonl`, `about_child.json`, `canon_grown.json`; recall by place/friend name), the trip-card and recap requests (`mind_trip.c`); post-call extraction later | `foundation` `store` `world` `provider` |
| `engine` | `engine.h`: `engine_t` interface, `engine_cascade.c`, `engine_s2s.c` (slot), per-turn timing probes t0–t5 | `foundation` `provider` `mind` |
| `call` | `call.h`: IDLE → CONNECTING → WAITING ⇄ LISTENING → THINKING → SPEAKING → ENDED, cancellation, per-stage timeouts, quiet hours, time cap, "no signal" fallback | `foundation` `engine` `mind` `audio` `ui` `store` `world` |
| `instinct` | `instinct.h`: idle animations, blink/breathe/doze, scene choice from world state (the trip's generated backdrop and sprites when they exist, built-in ones otherwise, at the time of day there), the object list and layout for a trip (friend on the right, props on the left), what a touch does (object → its behaviour + vibration + a sound; Gugu → coo), the gear bubble while a call could not start (app.c's readiness probe: no Wi-Fi, offline, no key: a thought bubble with a settings gear, Gugu staring at it worried, muttering silently), a time-of-day override for screenshots | `foundation` `board` `audio` `ui` `store` `world` `art` |
| `life` | `life.h`: the worker that plans the next trip card on the journey so far (`life_plan_trip`), writes the diary of each day away and the recap once home (`life_diary_now`, `life_recap_now`), generates the trip's pictures through the image role (`life_art_now`: backdrop, ≤3 props, the friend; one `.art` or `.fail` per piece, ledger line per picture), and the daily tick that runs them in order (`life_tick`, nightly and once after boot; planning keyed by date); wake reasons, postcards and the weekly reflection later | `foundation` `store` `world` `mind` `provider` `board` `art` |
| `parent` | `parent.h`: pairing mode: softAP with a random numeric password, the JSON API (status, settings, secrets read/write and apply-now, Wi-Fi scan, calls, trip, memories, events, the log export `/api/logs`, the speaker-microphone test) and the embedded page | `foundation` `store` `board` `net` |
| `console` | `console.h`: the serial test console (USB-Serial/JTAG on the device, stdin on the host): provisioning (`wifi`, `key`, `set`), `call`/`hang`/`replay [early]`, real-microphone checks (`hold`, `mic`), the trip and the journey (`trip [new|clear|json]`, `clock`, `tick`, `diary`, `recap`, `journey`), the pictures (`art [gen|retry]`, `tod`), the child's hands (`press yellow|shake|pickup`), `parent` (pairing mode), `scan`, `events`, `logs` (the export bundle), `selftest` (speaker → microphone), `ready auto|no|yes` (the gear bubble), `stat`, `mem`, `ls`/`cat`, `sfx`, `vol`, `sleep` | `foundation` `store` `provider` `net` `call` `audio` `board` `parent` `world` `life` `mind` `art` `instinct` |

Every module in the table is in the tree; `world` and `life` arrived with v1.1 (the trip card and its planner) and
still grow (postcards, festivals, weather, wake reasons).

## Core Rules

Each rule is stated once here and enforced in one place. Breaking one is an architecture change —
say so out loud, do not slip it in.

| # | Invariant | Enforced in |
|---|---|---|
| 1 | **`app.c` is the only assembler.** It constructs modules, wires callbacks, starts tasks. It owns no behaviour; if logic accumulates there, it belongs in a module. | `app/app.c` |
| 2 | **A module is one directory with one public header.** `app/<m>/<m>.h` is the only header another module may include; `<m>.c` implements the façade; sub-components are flat files `<m>_<part>.c` / `<m>_<part>.h` (private). No nested directories inside a module. | `tools/check_deps.py` |
| 3 | **`foundation` is the one exception**: headerful components included directly (`foundation/ringbuf.h`), no `foundation.h`. It depends on nothing above it. | `tools/check_deps.py` |
| 4 | **Dependencies point downward only.** The "May include" column above is the whole allow-list, mirrored in `app/DEPS`. Upward communication is a callback registered by the caller at init, never an include. No cycles, ever. | `app/DEPS`, `tools/check_deps.py` |
| 5 | **No backend.** Nothing in the product requires a server, proxy, account or cloud of ours. Static files on GitHub (place packs, OTA images) are content, not a service. Python exists only under `tools/` (dev helpers such as the serial monitor, replay reports, one-off evaluations); nothing in the product runs it. | code review |
| 6 | **The provider protocol is fixed and vendor-blind.** `provider.h` defines the roles and session shapes; a vendor file implements them; nothing outside `app/provider/` names a vendor. Endpoints, models, voices, language hints, credentials and regions live in `content/providers/<id>.json`, never in code. | `app/provider/provider.h`, `content/providers/` |
| 7 | **Expression tags are a closed vocabulary.** The tag list exists in exactly three places that must change together: `mind_tags.c` (parser), `ui_anim.c` (clip table), `content/prompts/tags.md` (what the model is told). A unit test diffs the three. | `test/test_tags.c` |
| 8 | **Never log secrets.** No API key, Wi-Fi password or hotspot password reaches a log line, a trace or the export bundle unmasked. | `foundation/log.h` (`LOG_MASK`), `store_export.c` |
| 9 | **Big buffers live in PSRAM; the audio path never allocates.** Frame buffers, TLS buffers, audio rings and JPEG decode go through `foundation/mem.h` PSRAM allocators; capture/playback callbacks only move bytes. | `foundation/mem.h`, `app/audio/` |
| 10 | **Every turn is cancellable.** Abort, a new key press, a disconnect or a timeout all take the same path: cancel the chat stream, `cancel` the TTS session, flush the playback queue. No stage may block without a deadline. | `app/engine/engine_cascade.c`, `app/call/call.c` |
| 11 | **Latency and cost are recorded, not estimated.** Each turn stores t0 (release) … t5 (first sample played) and vendor usage; the acceptance report is computed from those rows. | `app/engine/engine.h` (`engine_timing_t`), `store_log.c` |
| 12 | **Tests first, bugs self-fixed, evidence attached.** No milestone or feature is reported done without the test output in the report and a `踩坑` line in `PLAN.md` for each bug found and fixed. | `test/`, `tools/replay/` |
| 13 | **State is files, writes are atomic, ticks are idempotent.** JSON under LittleFS written via tmp + rename; every scheduled job keyed by date so a cold boot can redo it safely. | `store_files.c`, `app/life/` |
| 14 | **Language policy.** Code, comments, commit messages, logs and every document are English, except `README_CN.md` and `PLAN.md`, which are Chinese by the owner's choice. Prompts are per-language content files, not code. | code review |
| 15 | **Dependencies are ESP-IDF managed components only.** Each entry in `app/idf_component.yml` (LVGL, codec drivers, …) is justified in `PLAN.md`. No vendored SDKs, no CMake fetch of random repos. | `app/idf_component.yml` |

## Conventions

### Naming

- Modules are lowercase nouns (`net`, `mind`). Public symbols carry the module prefix:
  `net_http_open()`, `mind_prompt_build()`, `call_state_t`. Private symbols are `static`.
- Files inside a module: `<m>.h` (public), `<m>.c` (façade), `<m>_<part>.c`, `<m>_<part>.h`
  (private), `<m>_esp.c` / `<m>_host.c` for platform splits picked by CMake.
- Errors are `esp_err_t`; module-specific failures use `ESP_ERR_<MODULE>_BASE + n` defined in the
  public header. Vendor failures are `provider_error_t` (HTTP status, vendor code, classified kind).
- Avoid names IDF or lwIP already export (`mem_free`, `queue_*` …) for public symbols: the foundation
  memory API is `mem_alloc_big / mem_alloc_dma / mem_release / mem_avail_*` for that reason.
- Big static arrays carry `MEM_BIG_BSS` (PSRAM .bss on the device). Task stacks stay in internal RAM:
  a PSRAM stack asserts in the flash driver on the first LittleFS access, and nearly every task reads or
  writes files. Keep stacks small instead (4–12 KB) and put buffers, not stacks, in PSRAM.
- Tasks are named `gugu_<module>_<role>` so `uxTaskGetSystemState` reads like the architecture.

### Callbacks and ownership

- A module exposes `<m>_init(const <m>_config_t *)`, `<m>_start()`, `<m>_stop()`, and
  `<m>_set_listener(<m>_listener_t, void *user)` when it reports upward. Listeners are invoked
  from the module's own task; the receiver copies and returns.
- Sessions (`asr_session_t`, `chat_session_t`, `tts_session_t`, …) are opaque handles created by
  `*_open` and destroyed by `*_close`; the creator owns them.

### Configuration and content

- Secrets and Wi-Fi: encrypted NVS, written by `parent` (the pairing page, which also shows them to the owner) or the
  console during development. Everything else the parent sets: `settings.json` in LittleFS.
- **No database.** Flash holds two LittleFS partitions, `content` (read-only image built from
  `content/`) and `data` (writable), plus encrypted NVS for Wi-Fi and API keys. Records are JSON
  documents (memory books, settings) and append-only JSONL (calls, diary, ledger) under `data/`,
  laid out as `PLAN.md` → 「工程结构 v1 · 数据与存储」 describes. Filtering and search happen in the
  parent's browser, not on the device. Revisit only for a need files cannot serve (cross-month
  full-text search, random access inside multi-MB files).
- `content/` is packed into the LittleFS image at build time: `prompts/` (persona, canon seed,
  tags, per-language examples), `providers/` (presets), `places/` (place packs), `sfx/`, `web/`.
- `assets/` holds art sources and generators; `assets/gen/pixelgen.py` renders sprites, scenes, the built-in props,
  accessories and window panes, `assets/gen/export_lvgl.py` turns them into `app/ui/ui_sprites.c/.h` (with the
  accessory anchors per face; it runs `assets/gen/check_faces.py` first: round white lenses, one mark each, nothing touching a frame), `assets/gen/sfxgen.py` synthesises `content/sfx/*.wav`. Never edit a generated file.

### Memory and power

- Internal SRAM is for Wi-Fi, TLS hot paths and task stacks. Audio rings, LVGL frame buffers,
  JPEG and JSON working sets are PSRAM. A PSRAM headroom check runs in the host and device tests.
- The device sleeps at power level L1 (ESP32 off) between interactions. Every wake is a cold boot:
  `life` reads the wake reason from the PMIC and decides, and nothing assumes RAM survived.

### PLAN.md

- Top sections (`目标`, `设计总览`, `阶段规划`, `原型 v1.0`, `应用框架`, `工程结构`, `待拍板`) are
  revised in place. The log (`## 日志`) is append-only, dated `### YYYY-MM-DD · <类型>：<标题>`.
- Discussion happens in chat; `PLAN.md` records conclusions, research, decisions, and `踩坑`.

## Extension Recipes

### Add a vendor

1. Write `content/providers/<id>.json`: `id`, `name`, `regions` → URLs per role, `models`, voice
   catalog per language, `per_language` hints, `credentials` + `credential_map`, `capabilities`
   (`asr_stream`, `tts_bidi`, `s2s`, `image`). Verify voice ids and language coverage against the
   vendor's own table, not memory.
2. Add `app/provider/provider_<id>.c` implementing the roles it supports from `provider.h`; register
   it in the table in `provider.c`. Reuse `provider_wire_anthropic.c` / `provider_wire_openai.c`
   for chat SSE where the vendor is compatible.
3. Record a `fake` fixture for it under `test/fixtures/providers/<id>/` and extend the provider
   conformance test so the new file passes the same shape tests as the others.

### Add an expression tag

Change `content/prompts/tags.md`, `mind_tags.c` and `ui_anim.c` (plus `ui.h`'s `ui_emotion_t`, `call.c`'s
tag→face map and the `EMOTIONS` list in `export_lvgl.py` for a face) in one commit, draw the face or clip in
`assets/gen/pixelgen.py`, regenerate, and run `test_tags`. The vocabulary is deliberately small (19 now, soft cap
20); argue for an addition in `PLAN.md` first (the three tests: the model can pick it reliably, a child reads it on
the face, it comes up weekly).

### Change the trip card

The card's fields are decided in three places that move together: `content/prompts/trip_card.md` (what the model
is asked to write), `world_card.c` (parse, validate, serialise) and `test_world.c` / `test_life.c` (a fixture card
and a scripted planner reply). The parent page (`content/web/index.html`, `/api/trip`) reads the same JSON. Dates
are never the model's: `life` stamps `depart_date` / `return_date` / `id` / `generated_at`.

### Add a place pack

`content/places/<slug>.json`: facts suitable for a 3–5 year old, the resident animal friend, a local
phrase, the ambient sound reference. Packs are human-reviewed content; the model never invents
real-world facts, it only retells what the pack says.

### Add a module

Only if no existing module owns the concern. Create `app/<m>/<m>.h` + `<m>.c`, add the row to the
table above and to `app/DEPS`, add it to `app/CMakeLists.txt`, and wire it in `app.c`.

## Build / Verify

**One command builds and flashes from a fresh clone on any Mac:** `./gugu.sh`. It finds the StopWatch
(Espressif USB-Serial/JTAG, VID 303A PID 1001; `--port` when several devices are attached), and on the
first run clones ESP-IDF **v5.5.6** and installs its toolchain, cmake, ninja and Python environment under
`.derived_data/` next to the script (`esp-idf/`, `espressif/` = `IDF_TOOLS_PATH`, `build/`, `sdkconfig`).
Nothing goes to `$HOME`; deleting the repository deletes the environment (~5 GB; first run ~10 min, later runs ~40 s). Flashing writes bootloader,
partition table, app and the `content` image only, so NVS (Wi-Fi, keys) and the `data` partition survive.
Target `esp32s3`. The application component is `app/`, not `main/`: the root `CMakeLists.txt` adds it
through `EXTRA_COMPONENT_DIRS`, and `app/CMakeLists.txt` lists its IDF dependencies explicitly in `REQUIRES`.
After adding or renaming source files run `idf.py reconfigure` (the glob is evaluated at configure time).

To drive `idf.py` by hand, use the same environment and paths the script uses:

```bash
export IDF_TOOLS_PATH="$PWD/.derived_data/espressif"; . .derived_data/esp-idf/export.sh
idf.py -B .derived_data/build -DSDKCONFIG=.derived_data/sdkconfig build          # firmware + content image
idf.py -B .derived_data/build -p /dev/cu.usbmodem83401 flash
python3 tools/check_deps.py                   # module dependency rules (rules 2–4)
python tools/dev/monitor.py --port /dev/cu.usbmodem83401 --reset --seconds 30 --grep app:,call:   # IDF's python has pyserial
python tools/dev/serial_cmd.py --port /dev/cu.usbmodem83401 --times "stat" "ls /content/prompts"  # drive the device console
python tools/dev/trip_walk.py --port /dev/cu.usbmodem83401     # a whole trip in minutes: card, pictures, diary, recap, the next card
```

Host simulator and unit tests are a plain CMake project (not the IDF Linux target: macOS needs SDL on
the main thread, and IDF's host `tcp_transport`/`esp_timer` are mocks). It compiles every `app/`
file except `*_esp.c` against `host/shim/`, LVGL from `managed_components/` (so run `idf.py
reconfigure` once first), IDF's cJSON sources, SDL2, libcurl and OpenSSL.

```bash
cmake -S host -B host/build && cmake --build host/build -j8
./host/build/gugu_tests                                   # unit tests (test/*.c, minitest.h)
GUGU_LIVE=1 ./host/build/gugu_tests                       # + live vendor tests (needs GUGU_MINIMAX_API_KEY)
./host/build/gugu_host [--scale 2]                        # the simulator: B/Y keys, mouse = touch, stdin = console
./host/build/gugu_host --replay test/fixtures/x.wav --turns 3 --quit-after 120000   # one call, N turns, exit
python3 tools/dev/replay_report.py host/data              # P50/P90 of asr / first_token / first_tts / first_play / done
python3 tools/dev/screenshots.py --out /tmp/gugu_shots     # every scene × four times of day, the play, the shake → index.html
```

Secrets on the host come from the environment (`GUGU_MINIMAX_API_KEY`, …), conventionally sourced
from `~/.config/gugu/secrets.env` (never in the repo). On the device they live in NVS and are set over
the console: `wifi <ssid> <pass>`, `key minimax <key>`; `replay <wav> [turns]` runs the same call
flow as the simulator's `--replay`. `tools/dev/with_timeout.sh` exists because macOS has no `timeout`.

## Recommended Agent Workflow

1. Read this file, then the relevant top section of `PLAN.md` (Chinese) for the product rule you
   are touching.
2. Classify the task (provider / behaviour / shell / content) and name the owning module.
3. Write or extend the test first; run it red.
4. Implement inside the smallest set of modules; keep the dependency direction intact
   (`tools/check_deps.py`).
5. Run the host tests, then the device build; for anything that touches a turn, run the replay.
6. Fix what fails yourself; record each bug as a `踩坑` line in `PLAN.md`.
7. Report with the test output attached.

## Verification Checklist

- [ ] `app.c` only assembles; no new behaviour there.
- [ ] New files sit flat inside their module; the only cross-module include is `<m>/<m>.h`
      (or a `foundation/*.h`).
- [ ] `app/DEPS` still describes every include edge; `tools/check_deps.py` passes.
- [ ] Nothing outside `app/provider/` names a vendor; vendor facts are in `content/providers/*.json`.
- [ ] A changed tag list changed all three places and `test_tags` passes.
- [ ] No secret in logs, traces or the export bundle.
- [ ] New large buffers are PSRAM; the audio callbacks still do not allocate.
- [ ] Every new blocking call has a deadline and a cancel path.
- [ ] Turn rows still carry t0–t5 and usage.
- [ ] Tests exist, were run, and their output is in the report; bugs found were fixed and logged.
- [ ] New persisted state is a JSON file written atomically; new scheduled work is idempotent by date.
- [ ] English everywhere except `README_CN.md` and `PLAN.md`.
- [ ] No new dependency outside `app/idf_component.yml`, and that entry is justified in `PLAN.md`.
- [ ] Generated art or C arrays were regenerated, not hand-edited.
