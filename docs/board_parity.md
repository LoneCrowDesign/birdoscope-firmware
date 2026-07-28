# Board parity: functional sync versus UX divergence

Birdoscope variations have two display families sharing one detection engine:

- `src/main_oled.cpp`: the u8g2 status-line OLED boards, including the Heltec
  V4, the S3 DevKitC builds, and the Analyze r0.1 (ESP32-S3).
- `src/main_tft.cpp`: the round GC9A01 TFT board (`esp32round`), a richer and
  differently laid out UI.

Both are thin presentation layers over `lib/birdoscope_core`, which holds the
detection engine and the shared state. The two behave identically but present
that state differently, and either can be built without the other.

Implementation details are below, it is highly recommended not to break from
this convention if you modify the code so that it remains portable across
different hardware.

## How behavior and rendering are split

1. **Behavior lives in core.** Anything that changes what the device does is
   implemented in `lib/birdoscope_core`: detection, persistence, radio handoff,
   storage fallback, time sync, alert semantics, and board-agnostic signaling.
   Core is the single source of truth, so device behavior doesn't drift between
   builds.
2. **'main_XXX' files only render.** Each `src/main_*.cpp` draws core state on
   its display and wires that board's peripherals to core hooks. It holds no
   behavior of its own.
3. **New capability goes into core first.** A feature that needs per-board
   rendering gets its functional hook in core, so every board behaves the same
   as soon as the feature exists. A board whose on-screen rendering is not
   finished still gets the behavior, and the missing interface is listed below
   as UX debt.

A change affecting only how something looks on one display needs no matching
change on the other. Notable differences below.

## UX parity table

The OLED and TFT columns show whether that board renders the capability: yes,
pending where the interface is still unfinished, or n/a where there is nothing
to draw.

| Capability                         | Shared behavior                                   | OLED | TFT     | Note |
|------------------------------------|---------------------------------------------------|------|---------|------|
| SD-not-found fallback to SPIFFS    | yes, both board files                             | yes  | pending | 1    |
| Startup jingle and RGB cycle       | yes, `coreNotifyBoot()`                           | n/a  | n/a     | 2    |
| Detection chirp, new versus repeat | yes, core                                         | yes  | yes     |      |
| Admin (SoftAP) mode screen         | yes, core hop logic                               | yes  | yes     |      |
| Word-based serial commands         | yes, core tokenizer plus per-board verbs          | yes  | yes     | 3    |
| Semantic nav layer (`NavEvent`)    | yes, core                                         | yes  | n/a     | 4    |
| Eight-screen carousel, four menus  | yes, core state (`ScreenId`, `MenuState`)         | yes  | n/a     | 5    |
| Runtime alert gates                | yes, core (`coreBuzzerEnabled`, `coreLedEnabled`) | yes  | yes     | 6    |
| Runtime scan-mode switch           | yes, core (`coreSetScanMode`)                     | yes  | yes     | 7    |
| Runtime target switch              | yes, core (`coreSetVendorMask`)                   | yes  | n/a     | 8    |

1. The OLED path blinks blue five times and shows "SD Card Not Found / Saving to
   SPIFFS", then blocks until Confirm on a board with buttons so the missing
   card cannot pass unnoticed. A board without buttons holds the notice and
   carries on. `esp32round` has no LED and still needs an on-screen notice.
2. Audio and LED only, so neither board file renders anything.
3. `nav`, `dump`, and `prev` live in core. `status`, `inject`, `log`, and `help`
   are per board file.
4. Gated on `NAV_SCHEME_3BTN`, so only the three-button Analyze r0.1 reads
   physical buttons through it. The serial injector works on every board, and
   boards with two buttons keep `coreInputTick()`.
5. Covers menu drill-in: Scan Mode with its channel picker, Targets, Alerts, and
   Web Config. The round TFT board shares the core screen and menu state but
   renders its own round-screen UX, which is divergent by design rather than
   pending work. Other OLED boards show the single status view until they gain
   controls. See [Menu UX](menu_ux.md).
6. The gates live in core, so every board's chirp and flash obey them. Only the
   three-button OLED board can toggle them live. Held in RAM, resetting to
   enabled on reboot.
7. Both board files already show the mode name via `channelModeName()`. Only the
   three-button OLED board can change it live. Held in RAM, resetting to the
   board default on reboot.
8. The mask is consulted inside `matchOuiRaw()`, so every board's detections obey
   it, but only the three-button OLED board has a Targets screen and neither
   board file displays the active target. Held in RAM, resetting to All on
   reboot.
