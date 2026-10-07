# Board Parity, Functional Sync and UX Divergence

Birdoscope boards fall into two display families over one detection engine.

- `src/main_oled.cpp` drives the u8g2 status-line OLED boards, including the
  Heltec V4, the S3 DevKitC builds, and the Analyze r0.1 and r0.4 boards.
- `src/main_tft.cpp` drives the round GC9A01 TFT board (`esp32round`), a richer
  and differently laid out UI.

Both are thin presentation layers over `lib/birdoscope_core`, which holds the
detection engine and the shared state. The two behave identically but present
that state differently, and either one builds without the other.

Keep this split when you modify the code, since it keeps the firmware portable
across boards.

## Splitting Behavior from Rendering

1. **Behavior lives in core.** `lib/birdoscope_core` implements anything that
   changes what the device does, including detection, persistence, radio
   handoff, storage fallback, time sync, alert semantics, and board-agnostic
   signaling.
2. **`src/main_*.cpp` files only render.** Each one draws core state on
   its display and wires that board's peripherals to core hooks.
3. **New capability goes into core first.** A feature that needs per-board
   rendering gets its functional hook in core, so every board behaves the same
   as soon as the feature exists. A board without finished on-screen rendering
   still gets the behavior, and the table below marks its interface pending.

A change affecting only how something looks on one display needs no matching
change on the other.

## UX Parity Table

The OLED and TFT columns show whether that board renders the capability, as
yes, pending where the board has no interface for it yet, or n/a where there
is nothing to draw.

| Capability                         | Shared behavior                                   | OLED | TFT     | Note |
|------------------------------------|---------------------------------------------------|------|---------|------|
| SD-not-found fallback to SPIFFS    | yes, both board files                             | yes  | pending | 1    |
| Startup jingle and RGB cycle       | yes, `coreNotifyBoot()`                           | n/a  | n/a     | 2    |
| Detection chirp, new versus repeat | yes, core                                         | yes  | yes     |      |
| Admin (SoftAP) mode screen         | yes, core hop logic                               | yes  | yes     |      |
| Word-based serial commands         | yes, core tokenizer plus per-board verbs          | yes  | yes     | 3    |
| Semantic nav layer (`NavEvent`)    | yes, core                                         | yes  | n/a     | 4    |
| Ten-screen carousel, six menus     | yes, core state (`ScreenId`, `MenuState`)         | yes  | n/a     | 5    |
| Runtime alert gates                | yes, core (`coreBuzzerEnabled`, `coreLedEnabled`) | yes  | yes     | 6    |
| Runtime scan-mode switch           | yes, core (`coreSetScanMode`)                     | yes  | yes     | 7    |
| Runtime target switch              | yes, core (`coreSetVendorMask`)                   | yes  | n/a     | 8    |
| RSSI distance estimate             | yes, core (`coreRssiToDistanceM`)                 | yes  | pending | 9    |
| Distance calibration, persisted    | yes, core (`coreSetEnvDensity`, `coreSetRssiAt1mDbm`) | n/a | pending | 10 |
| Vendor-colored detection blink     | yes, core (`notifyDetection`)                     | n/a  | n/a     | 11   |
| Proximity ring, persisted          | yes, core (`coreSetProxRingM`)                    | yes  | pending | 12   |
| Device wipe and power off          | yes, core (`coreDeviceWipe`, `corePowerOff`)      | yes  | pending | 13   |
| Runtime radio switch, BLE capture  | yes, core (`coreSetRadioMode`)                    | yes  | n/a     | 14   |

1. The OLED path blinks blue five times and shows "SD Card Not Found / Saving to
   SPIFFS". A board with nav buttons then waits for Confirm, so the missing
   card cannot pass unnoticed. A board without them holds the notice for 1.5
   seconds and carries on. `esp32round` has no LED and still needs an on-screen
   notice.
2. Audio and LED only, so neither board file renders anything.
3. `nav`, `dump`, and `prev` live in core. `status`, `inject`, `log`, and `help`
   are per board file.
4. `NAV_BTN_COUNT` gates it, so only an Analyze board running `NAV_SCHEME_3BTN`
   or `NAV_SCHEME_4BTN` reads physical buttons through it. The serial injector
   works on every board, and boards with two buttons keep `coreInputTick()`.
5. Covers menu drill-in for Scan Mode with its channel picker, Targets, Radio,
   Alerts, Web Config, and Device Wipe with its confirmation. Scan Mode exists
   only while 802.11 is the selected radio, and the carousel skips it in BLE
   mode. The round TFT board shares the core screen and menu state but
   renders its own round-screen UX, by design. OLED boards without nav buttons
   show a single status view. See [Menu UX](menu_ux.md).
6. The gates live in core, so every board's chirp and flash obey them. Only the
   Analyze boards can toggle them live. Core holds them in RAM, and they reset
   to enabled on reboot.
7. Both board files show the mode name through `channelModeName()`. Only the
   Analyze boards can change it live. Core holds it in RAM, and it resets to the
   board default on reboot.
8. Both matchers, `matchOuiRaw()` and `coreBleMatch()`, read the mask, so every
   board's detections obey it, but only the Analyze boards have a Targets screen
   and neither board file displays the active target. Core holds the mask in
   RAM, and it resets to All on reboot.
9. `coreHandleAlert()` reports it as `CoreAlertResult::distM` for every board.
   The OLED Overview screen shows it as `dst:`, and the round TFT has no row for
   it yet. Every 802.11 hit except `oui_addr1` gets a value, since `oui_addr1`
   RSSI describes the AP link. See
   [Distance estimation](distance_estimation.md).
10. Environment Density and the expected RSSI at 1 m, both set from the web
    console's `calibrate` command. Core persists them to SPIFFS at
    `/settings.json`, so unlike the scan-mode and alert gates above they survive
    a reboot.

    You set them once for a site and antenna, so they have no screen. Neither
    board file draws them, hence n/a for the OLED.

    `main_tft.cpp` is pending because it never calls `coreSettingsLoad()`, so it
    ignores the saved values. See
    [Distance estimation](distance_estimation.md).
11. Blue for Flock, yellow for Axon, and pulse count for new versus repeat.
    Core's NeoPixel path drives it, so it needs no board rendering. A board
    without `USE_LED` compiles it out. `esp32round` has
    no LED at all. See [Alert behavior](alerts.md).
12. The chirp when a tracked target closes inside the ring. Core evaluates it in
    `coreHandleAlert()`, so the alert fires on both board files. Only the OLED
    boards have the Alerts menu row that sets the range. `main_tft.cpp` is
    pending for the same reason as note 10, so it ignores the persisted range
    and uses the compiled-in `PROX_RING_M`. See [Alert behavior](alerts.md).
13. Core runs the erase on any board, but only `main_oled.cpp` reaches it. The
    menu row lives on the shared carousel, and the interrupted wipe check runs
    in that board's `setup()`. `main_tft.cpp` renders its own UX with no
    carousel, so it can neither start a wipe nor resume one. See
    [Menu UX](menu_ux.md).
14. Only boards with `HAS_BLE_SCAN` build BLE capture. They need a Bluetooth 5
    controller for extended advertising, which `esp32round`'s classic ESP32
    lacks, so that board compiles BLE out and refuses the switch. Of the boards
    with a carousel, only the Analyze boards declare BLE logging. Core holds the
    mode in RAM, and it resets to 802.11 on reboot. See [Menu UX](menu_ux.md).
