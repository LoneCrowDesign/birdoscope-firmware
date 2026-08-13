# Alert behavior: chirp and flash reference

What the LED and buzzer do, and when. Every timing and tone value below is a
per-board define due to buzzer/LED differences. The numbers given are the common case, and
`include/boards/<board>/board_config.h` is authoritative for any specific board.

## Runtime mute and LED gates

On a board with controls, the Alerts carousel screen (see [Menu UX](menu_ux.md))
lets you mute and unmute:

- **Buzzer, Muted** (`coreBuzzerEnabled = false`): silences the new-detection
  chirp and the proximity chirp, which are the only automatic sounds the
  firmware produces, so this gate silences the device entirely.
- **LED, Off** (`coreLedEnabled = false`): suppresses the detection LED flashes.

Both default to enabled every boot. They are session-only and not persisted.

The gates cover only the automatic detection feedback below. The boot jingle,
the boot RGB cycle, the SD-init blink, and the on-demand `chirp`, `prox` and
`jingle` verbs all run regardless.

## Alert glossary

| Situation              | LED                              | Buzzer                 |
|------------------------|----------------------------------|------------------------|
| Boot                   | White, after an R/G/B cycle      | Six-note jingle        |
| SD card not found      | Blue, five flashes               | Silent                 |
| New MAC or rediscovery | Vendor color, two pulses         | Two ascending beeps    |
| Repeat within cooldown | None                             | Silent                 |
| Repeat after cooldown  | Vendor color, one pulse          | Silent                 |
| Crossed the prox ring  | Vendor color, three pulses       | Three descending beeps |

A new-MAC chirp fires only for detection methods that report a scanner-to-camera
RSSI, which excludes `oui_addr1`. The LED pulses for every method. See
[Detection color and pulse count](#detection-color-and-pulse-count).

The heartbeat is retired on screen models and no longer appears in this table.

## Detection color and pulse count

A detection encodes two facts at once. The color encodes the vendor, so the
fleet is readable without looking at the panel:

| Vendor                 | Color  | Define              |
|------------------------|--------|---------------------|
| Flock Safety           | Blue   | `LED_COLOR_FLOCK_*` |
| Axon Enterprise        | Yellow | `LED_COLOR_AXON_*`  |
| No vendor attributed   | Green  | `LED_COLOR_*`       |

The pulse count encodes whether the MAC is new: two pulses for a first sighting
or a rediscovery, one for a repeat after cooldown. Each pulse is `LED_FLASH_MS`
on and
the same off, so a two-pulse train takes about 360 ms at the common 120 ms.

The green fallback applies to an `ssid_keyword` hit. That method matches on
network name, so it yields no OUI to attribute to a vendor. See
[Detection methods](detection_methods.md).


## Startup

| Event | LED                                                         | Buzzer                    |
|-------|-------------------------------------------------------------|---------------------------|
| Boot  | R, G, B cycle at 200 ms each, then a white flash for 200 ms | Six-note descending motif |

The RGB cycle is a hardware sanity check that exposes a dead or miswired channel
at boot.

## SD card init

| Event             | LED                                  | Buzzer |
|-------------------|--------------------------------------|--------|
| SD card not found | Five blue flashes, 150 ms on and off | Silent |

The display also shows an "SD Card Not Found / Saving to SPIFFS" notice, naming
the onboard SPI Flash File System the capture falls back to. On a
board with a Confirm button (`NAV_SCHEME_3BTN`) boot then pauses until Confirm
is pressed, so a missing card cannot pass unnoticed. A board without buttons
holds the notice for 1500 ms and continues.

## Detection events

All detection alerts follow the same path. The WiFi sniffer callback enqueues an
alert and `drainAlertQueue()` in `loop()` drains it, which keeps the
interrupt-context callback clear of the serial, LED, and buzzer output.

### New detection, first sighting of a MAC

| LED                                          | Buzzer                                                             |
|----------------------------------------------|--------------------------------------------------------------------|
| Vendor color, two pulses of `LED_FLASH_MS`   | Two fast ascending beeps, `NEW_CHIRP_LO_HZ` then `NEW_CHIRP_HI_HZ` |

Frequencies are per board: a board scales them to suit its buzzer, so the
Analyze r0.1 runs lower than the dev-board default to drive its larger piezo.
`board_config.h` holds the values in effect.

This fires when a MAC is seen for the first time in a session, or when a
previously seen MAC reappears after `REDISCOVER_MS` (30 s) of silence, meaning
it left RF range and came back.

The LED fires for every detection method. The chirp fires only for methods that
report a scanner-to-camera RSSI, which excludes `oui_addr1`.

An addr1 hit is read off the AP that answered the camera's probe, so its RSSI
describes the path to that AP, and the camera itself may be any distance away.
Chirping on it would mean a sound with no bearing on whether there is anything
near you to look for.

The detection still counts, still logs, and still lights the LED. It makes no
sound. The proximity ring applies the same exclusion, for the same reason.

### Repeat detection, within cooldown

Suppressed entirely: no LED, no chirp, no serial output. The detection table
still increments the count for that MAC.

The suppression window is `ALERT_COOLDOWN_MS` (5 s). After it expires the MAC
emits again, as a single vendor-color pulse with no chirp, until `REDISCOVER_MS`
passes and it becomes chirp-worthy again.

### Repeat detection, after cooldown

| LED                                         | Buzzer |
|---------------------------------------------|--------|
| Vendor color, one pulse of `LED_FLASH_MS`   | Silent |

### Proximity ring, closing on a known target

| LED                                          | Buzzer                                                                |
|----------------------------------------------|-----------------------------------------------------------------------|
| Vendor color, three pulses of `LED_FLASH_MS` | Three descending beeps, `PROX_CHIRP_HI_HZ` down to `PROX_CHIRP_LO_HZ` |

The chirps above all key off whether a MAC is new. A camera first heard at the
edge of range chirps once, then stays silent for as long as it keeps
transmitting. Nothing sounds again as it gets closer, so the one alert you get
arrives when the target is furthest away and least useful to look for. A
detection that is working correctly reads as a miss.

The proximity ring closes that gap. When a target already being tracked crosses
inside the ring, it chirps again. The default ring is 25 m, set from the Alerts
menu (Off, 10 m, 25 m, 50 m, 100 m) and persisted to `/settings.json` as
`prox_m`.

Three rules keep one ring from becoming a stream of chirps:

- Per-MAC RSSI is smoothed with an exponential moving average, alpha 1/4
  (`PROX_EMA_SHIFT`), so one multipath null cannot cross the boundary on its own.
- The crossing is latched, so a target sitting at the ring chirps once, not once
  per frame.
- The latch clears only beyond `PROX_HYST_PCT` (130%) of the ring, so the jitter
  left after smoothing cannot re-arm it.

`oui_addr1` hits are excluded: their RSSI measures the AP that answered a probe,
not the target, which is the same reason they read `dst:--`.

#### What the constants have to absorb

All three rules are needed, and each covers a case the others do not. Smoothing
alone still crosses the ring on a sustained fade. The latch alone still re-arms
on jitter sitting at the boundary.

The smoothing factor is chosen against two failure modes that pull in opposite
directions. Too responsive and a target parked near the ring chatters, firing
repeatedly on multipath alone. Too slow and the average lags a real approach far
enough that the chirp arrives well inside the ring, which defeats the point of
setting one. Multipath swings instantaneous RSSI by roughly 6 to 10 dB, a 1.7 to
2.5 times distance error on a single sample, so the smoothing has to cover at
least that much without lagging an approach at vehicle speed.

The averaging step is floored at 1 dB, which is load-bearing rather than a
rounding nicety. An integer average with a plain shift truncates any difference
smaller than the shift to zero, so the average parks short of the true reading
and stays there indefinitely. A stationary target a little inside the ring then
holds its estimate outside the ring and never chirps, which is precisely the case
the ring exists to catch. A continuous-approach test cannot reach this failure,
because closing distance keeps feeding differences large enough to survive the
shift. Any change to the smoothing needs re-checking against a stationary target
just inside the ring, not only against a drive-by.

A per-MAC minimum re-chirp interval was considered and dropped. It can only fire
after the latch has already cleared, which requires leaving the hysteresis band,
so it would add per-detection state to guard a case the latch already covers.

Before setting a ring:

- The ring is a distance estimate, so it inherits that model's accuracy. Read
  [Distance estimation](distance_estimation.md) first, and use `rssi_trim` to
  align the chirp with what you can see, the same knob that aligns the `dst:`
  row.
- Smoothing lags on approach, so a moving target crosses the ring before the
  chirp fires. The size of that lag depends on the camera's transmit rate and
  your closing speed, neither of which the firmware knows, and a fast approach
  on a rarely-heard camera pushes the chirp nearest. Set the ring wider than
  the range you want to be warned at.

A first sighting already inside the ring latches silently, because the
new-detection chirp is firing for that same frame. One event, one sound.

The ring adds a sound and an LED pulse and nothing else. It is evaluated after
the detection table and the SD row are written, ahead of the repeat-suppression
gate but outside it, so it emits no serial line, no JSON, and no log row. What
gets captured is identical whether the ring is set to 25 m or Off.

## Heartbeat (retired on screen models)

The heartbeat was a periodic "still scanning" pulse confirming the device was
running and sniffing: a purple LED flash every `HB_BEEP_INTERVAL_MS` (10 s),
starting once a target had been seen and stopping after `HB_DEVICE_ACTIVE_MS`
(3 s) of silence. It never made a sound, despite the `HB_BEEP_*` names.

It is no longer called on any board with a display. The carousel already shows
the detection count, RSSI and estimated distance, so the pulse restated what the
panel was displaying while competing with the detection flash for the same LED.
Feedback is now events only: a new detection, and a proximity ring crossing.

The implementation stays in `core.cpp`, uncalled, because it remains the right
behavior for a board with no screen. Re-enable it by calling `heartbeatTick()`
from `coreNotifyTick()`; `HB_BEEP_INTERVAL_MS`, `HB_DEVICE_ACTIVE_MS` and
`LED_COLOR_HB_*` all still apply.

## Where the values come from

| Define                                     | Controls                                                   |
|--------------------------------------------|------------------------------------------------------------|
| `LED_FLASH_MS`                             | length of each detection pulse, and of the heartbeat pulse |
| `LED_COLOR_FLOCK_*`                        | Flock detection color (blue)                               |
| `LED_COLOR_AXON_*`                         | Axon detection color (yellow)                              |
| `LED_COLOR_*`                              | detection color when no vendor is attributed (green)       |
| `LED_COLOR_NEW_*`                          | no longer used; new versus repeat is pulse count now       |
| `LED_COLOR_HB_*`                           | heartbeat color (purple)                                   |
| `LED_COLOR_BOOT_*`                         | boot confirmation color (white)                            |
| `NEW_CHIRP_LO_HZ` / `NEW_CHIRP_HI_HZ`      | the two chirp tones                                        |
| `NEW_CHIRP_NOTE_MS` / `NEW_CHIRP_GAP_MS`   | chirp note length and gap                                  |
| `PROX_CHIRP_HI_HZ` / `_MID_HZ` / `_LO_HZ`  | the three proximity tones, defaulting off `NEW_CHIRP_*`    |
| `PROX_CHIRP_NOTE_MS` / `PROX_CHIRP_GAP_MS` | proximity note length and gap                              |
| `PROX_RING_M`                              | default proximity ring in metres, 0 for off                |
| `PROX_HYST_PCT`                            | percent of the ring the latch clears beyond                |
| `PROX_EMA_SHIFT`                           | RSSI smoothing, alpha = 1 / 2^shift                        |
| `ALERT_COOLDOWN_MS`                        | repeat-suppression window                                  |
| `REDISCOVER_MS`                            | silence after which a known MAC counts as new again        |
| `HB_BEEP_INTERVAL_MS`                      | heartbeat pulse interval                                   |
| `HB_DEVICE_ACTIVE_MS`                      | how long a target counts as still in range                 |

A board without a buzzer (`USE_BUZZER` unset) is silent throughout, and a board
without an addressable LED (`USE_LED` unset) produces no flashes. Both paths
compile out rather than no-op at runtime.
