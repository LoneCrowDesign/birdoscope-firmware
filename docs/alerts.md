# Alert Behavior, Chirp and Flash Reference

What the LED and buzzer do, and when. Each board can override every timing and
tone value below to suit its buzzer and LED. The numbers given are the common
case, and `include/boards/<board>/board_config.h` is authoritative for any
specific board.

## Runtime Mute and LED Gates

On a board with controls, the Alerts carousel screen (see [Menu UX](menu_ux.md))
holds two gates:

- `Buzzer: Muted` (`coreBuzzerEnabled = false`) silences the new-detection
  chirp and the proximity chirp. The firmware makes no other automatic sound,
  so this gate silences the device.
- `LED: Off` (`coreLedEnabled = false`) suppresses the detection and proximity
  LED pulses.

Both reset to enabled at every boot. The firmware does not persist them.

The gates cover only the automatic detection feedback below. The boot sound,
the boot RGB cycle, the SD-init blink, and the on-demand `chirp`, `prox`,
`jingle`, `crow` and `hawk` verbs all run regardless.

## Alert Glossary

| Situation              | LED                              | Buzzer                 |
|------------------------|----------------------------------|------------------------|
| Boot                   | White, after an R/G/B cycle      | Boot call              |
| SD card not found      | Blue, five flashes               | Silent                 |
| New MAC or rediscovery | Vendor color, two pulses         | Two ascending beeps    |
| Repeat within cooldown | None                             | Silent                 |
| Repeat after cooldown  | Vendor color, one pulse          | Silent                 |
| Crossed the prox ring  | Vendor color, three pulses       | Three descending beeps |

A new-MAC chirp fires only for detection methods that report a scanner-to-camera
RSSI, which excludes `oui_addr1`. The LED pulses for every method. See
[Detection color and pulse count](#detection-color-and-pulse-count).

An infra match, such as a cellular router, logs at most one row per device
every `INFRA_DEDUPE_MS`. It stays off the panel and fires no LED, buzzer or
proximity alert.

In BLE mode a target match alerts the same way, with no proximity ring. A
device keeps one detection across address changes when its advertisement
contains a serial, as Axon advertisements do.

Screen models run no heartbeat. See
[Heartbeat](#heartbeat-unused-on-screen-models).

## Detection Color and Pulse Count

A detection encodes two facts at once. The color encodes the vendor, so you can
tell the fleet without looking at the panel.

| Vendor                 | Color  | Define                |
|------------------------|--------|-----------------------|
| Flock Safety           | Blue   | `LED_COLOR_FLOCK_*`   |
| Axon Enterprise        | Yellow | `LED_COLOR_AXON_*`    |
| Axis Communications    | Teal   | `LED_COLOR_AXIS_*`    |
| Utility                | Purple | `LED_COLOR_UTILITY_*` |
| Any other, or none     | Green  | `LED_COLOR_*`         |

The pulse count encodes whether the MAC is new. A first sighting or a
rediscovery pulses twice, and a repeat after cooldown pulses once. Each pulse
holds `LED_FLASH_MS` on and the same off, so a two-pulse train takes about
360 ms at the common 120 ms.

Green also marks an `ssid_match` hit, which matches on network name and has no
OUI to attribute to a vendor. See [Detection methods](detection_methods.md).

## Startup

| Event | LED                                                         | Buzzer    |
|-------|-------------------------------------------------------------|-----------|
| Boot  | R, G, B cycle at 200 ms each, then a white flash for 200 ms | Crow call |

The RGB cycle is a hardware sanity check that exposes a dead or miswired channel
at boot.

`BOOT_SOUND` selects the call, one of `BOOT_SOUND_CROW` (default),
`BOOT_SOUND_HAWK`, or `BOOT_SOUND_JINGLE` for a six-note descending motif. The
`crow` and `hawk` verbs play their calls regardless of the selection, so you can
compare the two on the real element without a reflash.

A piezo emits one square wave, so neither call is a reproduction. A corvid caw
is broadband, and its fundamental falls below the range a bending-disc element
radiates usefully.

The tones reproduce the cadence and the pitch glide within each syllable, which
identify a call to a listener. A fast frequency wobble stands in for the rasp.
The firmware keeps frequencies within the element's efficient band, trading
realism for audibility. The hawk call is closer to a pure descending glide, so
the element loses less of it.

## SD Card Init

| Event             | LED                                  | Buzzer |
|-------------------|--------------------------------------|--------|
| SD card not found | Five blue flashes, 150 ms on and off | Silent |

An OLED board's display also shows an "SD Card Not Found / Saving to SPIFFS"
notice, naming the onboard SPI Flash File System the capture falls back to. On
a board with nav buttons (`NAV_BTN_COUNT`), boot then waits until you press
Confirm, so a missing card cannot pass unnoticed. A board without buttons holds
the notice for 1500 ms and continues. The `esp32round` build logs the missing
card to serial and continues with neither signal.

## Detection Events

All detection alerts follow the same path. The WiFi sniffer callback enqueues an
alert and `drainAlertQueue()` in `loop()` drains it, which keeps the
interrupt-context callback clear of the serial, LED, and buzzer output.

### New Detection, First Sighting of a MAC

| LED                                          | Buzzer                                                             |
|----------------------------------------------|--------------------------------------------------------------------|
| Vendor color, two pulses of `LED_FLASH_MS`   | Two fast ascending beeps, `NEW_CHIRP_LO_HZ` then `NEW_CHIRP_HI_HZ` |

The Analyze r0.1 runs lower than the dev-board default to suit its larger piezo.

This fires on the first sighting of a MAC in a session, or when a known MAC
reappears after `REDISCOVER_MS` (30 s) of silence, meaning it left RF range and
came back.

The LED fires for every detection method. The chirp fires only for methods that
report a scanner-to-camera RSSI, which excludes `oui_addr1`.

The sniffer reads an addr1 hit off the AP that answered the camera's probe, so
its RSSI describes the path to that AP, and the camera itself may be any
distance away. A chirp on it says nothing about whether the camera is near.

The detection still counts, logs, and lights the LED. The proximity ring
applies the same exclusion.

### Repeat Detection, Within Cooldown

The firmware emits no LED pulse, no chirp, no serial line and no JSON. The
detection table still increments the count for that MAC, and the SD log still
records the frame.

The suppression window is `ALERT_COOLDOWN_MS` (5 s). After it expires the MAC
emits again, as a single vendor-color pulse with no chirp, until `REDISCOVER_MS`
passes and it becomes chirp-worthy again.

### Repeat Detection, After Cooldown

| LED                                         | Buzzer |
|---------------------------------------------|--------|
| Vendor color, one pulse of `LED_FLASH_MS`   | Silent |

### Proximity Ring, Closing on a Known Target

| LED                                          | Buzzer                                                                |
|----------------------------------------------|-----------------------------------------------------------------------|
| Vendor color, three pulses of `LED_FLASH_MS` | Three descending beeps, `PROX_CHIRP_HI_HZ` down to `PROX_CHIRP_LO_HZ` |

The chirps above all key off whether a MAC is new. A camera first heard at the
edge of range chirps once, then stays silent for as long as it keeps
transmitting. Nothing sounds again as it gets closer, so the one alert you get
arrives when the target is furthest away and least useful to look for.

The proximity ring closes that gap. When a tracked target crosses inside the
ring, the device chirps again. The default ring is 25 m. You set it from the
Alerts menu (Off, 10 m, 25 m, 50 m, 100 m), and the firmware persists it to
`/settings.json` as `prox_m`.

Three rules keep one ring from becoming a stream of chirps:

- An exponential moving average, alpha 1/4 (`PROX_EMA_SHIFT`), smooths
  per-MAC RSSI, so one multipath null cannot cross the boundary on its own.
- A latch holds each crossing, so a target sitting at the ring chirps once.
- The latch clears only beyond `PROX_HYST_PCT` (130%) of the ring, so the jitter
  left after smoothing cannot re-arm it.

The ring skips `oui_addr1` hits, which read `dst:via AP` for the same reason.

#### What the Constants Have to Absorb

Each rule covers a case the others do not. Smoothing alone still crosses the
ring on a sustained fade. The latch alone still re-arms on jitter at the
boundary.

The smoothing factor balances two failure modes that pull in opposite
directions. Too responsive, and a target parked near the ring chatters on
multipath alone. Too slow, and the average lags a real approach until the chirp
arrives well inside the ring. Multipath swings instantaneous RSSI by roughly 6
to 10 dB, a 1.7 to 2.5 times distance error on a single sample, so the
smoothing has to cover at least that much without lagging an approach at
vehicle speed.

`proximityEvaluate()` floors the averaging step at 1 dB. A plain integer shift
truncates any difference under 2^`PROX_EMA_SHIFT` to zero, which parks the
average short of the true reading and leaves a stationary target just inside
the ring silent. A drive-by test cannot catch this, because closing distance
keeps the differences large. Re-check any change to the smoothing against a
stationary target just inside the ring.

Before setting a ring:

- The ring is a distance estimate, so it inherits that model's accuracy. Read
  [Distance estimation](distance_estimation.md) first, and use `rssi_trim` to
  align the chirp with what you can see, the same knob that aligns the `dst:`
  row.
- Smoothing lags on approach, so a moving target crosses the ring before the
  chirp fires. The size of that lag depends on the camera's transmit rate and
  your closing speed, neither of which the firmware knows, and a fast approach
  on a rarely heard camera pushes the chirp closest. Set the ring wider than
  the range you want a warning at.

A first sighting already inside the ring latches silently, because the
new-detection chirp is firing for that same frame.

The ring adds a sound and an LED pulse and nothing else. `coreHandleAlert()`
evaluates it after writing the detection table and the SD row, ahead of the
repeat-suppression gate and outside it, so it emits no serial line, no JSON, and
no log row. The capture is identical whether the ring is 25 m or Off.

## Heartbeat, Unused on Screen Models

The heartbeat is a periodic "still scanning" purple LED pulse every
`HB_BEEP_INTERVAL_MS` (10 s). It starts once the device sees a target and stops
after `HB_DEVICE_ACTIVE_MS` (3 s) of silence. It makes no sound, despite the
`HB_BEEP_*` names.

`coreNotifyTick()` does not call it, so no board runs it. On a screen model the
carousel already shows the detection count, RSSI and estimated distance, and the
pulse would compete with the detection flash for the same LED.

`heartbeatTick()` stays in `core.cpp` for a board with no screen. To enable it,
call `heartbeatTick()` from `coreNotifyTick()`. `HB_BEEP_INTERVAL_MS`,
`HB_DEVICE_ACTIVE_MS` and `LED_COLOR_HB_*` still apply.

## Where the Values Come From

| Define                                     | Controls                                                   |
|--------------------------------------------|------------------------------------------------------------|
| `LED_FLASH_MS`                             | length of each detection pulse, and of the heartbeat pulse |
| `LED_COLOR_FLOCK_*`                        | Flock detection color (blue)                               |
| `LED_COLOR_AXON_*`                         | Axon detection color (yellow)                              |
| `LED_COLOR_AXIS_*`                         | Axis detection color (teal)                                |
| `LED_COLOR_UTILITY_*`                      | utility detection color (purple)                           |
| `LED_COLOR_*`                              | detection color for any other vendor or none (green)       |
| `LED_COLOR_NEW_*`                          | unused, pulse count marks new versus repeat                |
| `LED_COLOR_HB_*`                           | heartbeat color (purple)                                   |
| `LED_COLOR_BOOT_*`                         | boot confirmation color (white)                            |
| `NEW_CHIRP_LO_HZ` / `NEW_CHIRP_HI_HZ`      | the two chirp tones                                        |
| `NEW_CHIRP_NOTE_MS` / `NEW_CHIRP_GAP_MS`   | chirp note length and gap                                  |
| `PROX_CHIRP_HI_HZ` / `_MID_HZ` / `_LO_HZ`  | the three proximity tones, defaulting off `NEW_CHIRP_*`    |
| `PROX_CHIRP_NOTE_MS` / `PROX_CHIRP_GAP_MS` | proximity note length and gap                              |
| `PROX_RING_M`                              | default proximity ring in meters, 0 for off                |
| `PROX_HYST_PCT`                            | percent of the ring the latch clears beyond                |
| `PROX_EMA_SHIFT`                           | RSSI smoothing, alpha = 1 / 2^shift                        |
| `ALERT_COOLDOWN_MS`                        | repeat-suppression window                                  |
| `BOOT_SOUND`                               | which boot call plays, crow, hawk, or the legacy jingle    |
| `BIRD_RASP_PCT`                            | bird-call wobble depth, the stand-in for rasp              |
| `BIRD_STEP_MS`                             | bird-call sweep step, which also sets the wobble rate      |
| `REDISCOVER_MS`                            | silence after which a known MAC counts as new again        |
| `HB_BEEP_INTERVAL_MS`                      | heartbeat pulse interval                                   |
| `HB_DEVICE_ACTIVE_MS`                      | how long a target counts as still in range                 |

A board without a buzzer (`USE_BUZZER` unset) is silent throughout, and a board
without an addressable LED (`USE_LED` unset) produces no flashes. Both paths
compile out rather than no-op at runtime.
