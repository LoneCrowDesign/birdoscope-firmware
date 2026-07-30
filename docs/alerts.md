# Alert behavior: chirp and flash reference

What the LED and buzzer do, and when. Every timing and tone value below is a
per-board define due to buzzer/LED differences. The numbers given are the common case, and
`include/boards/<board>/board_config.h` is authoritative for any specific board.

## Runtime mute and LED gates

On a board with controls, the Alerts carousel screen (see [Menu UX](menu_ux.md))
lets you mute/unmute

- **Buzzer, Muted** (`coreBuzzerEnabled = false`): silences the new-detection
  chirp. That chirp is the only automatic sound the firmware produces, so this
  gate silences the device entirely.
- **LED, Off** (`coreLedEnabled = false`): suppresses the detection and
  heartbeat LED flashes.

Both default to enabled every boot. They are session-only and not persisted.

The gates cover only the automatic detection and heartbeat feedback below. The
boot jingle, the boot RGB cycle, the SD-init blink, and the on-demand `chirp`
and `jingle` verbs all run regardless.

## Alert Glossary

| Situation              | LED                              | Buzzer              |
|------------------------|----------------------------------|---------------------|
| Boot                   | White, after an R/G/B cycle      | Six-note jingle     |
| SD card not found      | Blue, five flashes               | Silent              |
| New MAC or rediscovery | Vendor color, two pulses         | Two ascending beeps |
| Repeat within cooldown | None                             | Silent              |
| Repeat after cooldown  | Vendor color, one pulse          | Silent              |
| Heartbeat              | Purple                           | Silent              |

The heartbeat is silent by design, an LED-only pulse.

## Detection color and pulse count

A detection encodes two facts at once. Color carries the vendor, so the fleet is
readable without looking at the panel:

| Vendor                 | Color  | Define              |
|------------------------|--------|---------------------|
| Flock Safety           | Blue   | `LED_COLOR_FLOCK_*` |
| Axon Enterprise        | Yellow | `LED_COLOR_AXON_*`  |
| No vendor attributed   | Green  | `LED_COLOR_*`       |

Pulse count carries whether the MAC is new: two pulses for a first sighting or a
rediscovery, one for a repeat after cooldown. Each pulse is `LED_FLASH_MS` on and
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

The display also shows an "SD Card Not Found / Saving to SPIFFS" notice. On a
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

Most boards use 2000 Hz then 2800 Hz at 55 ms each. The Analyze r0.1 (ESP32-S3)
scales both down, to 1125 Hz and 1575 Hz, to suit its larger 12 mm piezo.

This fires when a MAC is seen for the first time in a session, or when a
previously seen MAC reappears after `REDISCOVER_MS` (30 s) of silence, meaning
it left RF range and came back.

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

## Heartbeat

The heartbeat is a periodic "still scanning" pulse that confirms the device is
running and sniffing. It fires every `HB_BEEP_INTERVAL_MS` (10 s), starts only
after at least one target has been seen this session, and stops when nothing has
been heard within `HB_DEVICE_ACTIVE_MS` (3 s), so it does not run continuously
when nothing is around.

| LED                                   | Buzzer |
|---------------------------------------|--------|
| Purple flash, `LED_FLASH_MS` (120 ms) | Silent |

The heartbeat produces no sound. Only the LED pulses, gated by `coreLedEnabled`.

## Where the values come from

| Define                                   | Controls                                            |
|------------------------------------------|-----------------------------------------------------|
| `LED_FLASH_MS`                           | length of each detection pulse, and of the heartbeat pulse |
| `LED_COLOR_FLOCK_*`                      | Flock detection color (blue)                        |
| `LED_COLOR_AXON_*`                       | Axon detection color (yellow)                       |
| `LED_COLOR_*`                            | detection color when no vendor is attributed (green) |
| `LED_COLOR_NEW_*`                        | no longer used; new versus repeat is pulse count now |
| `LED_COLOR_HB_*`                         | heartbeat color (purple)                            |
| `LED_COLOR_BOOT_*`                       | boot confirmation color (white)                     |
| `NEW_CHIRP_LO_HZ` / `NEW_CHIRP_HI_HZ`    | the two chirp tones                                 |
| `NEW_CHIRP_NOTE_MS` / `NEW_CHIRP_GAP_MS` | chirp note length and gap                           |
| `ALERT_COOLDOWN_MS`                      | repeat-suppression window                           |
| `REDISCOVER_MS`                          | silence after which a known MAC counts as new again |
| `HB_BEEP_INTERVAL_MS`                    | heartbeat pulse interval                            |
| `HB_DEVICE_ACTIVE_MS`                    | how long a target counts as still in range          |

A board without a buzzer (`USE_BUZZER` unset) is silent throughout, and a board
without an addressable LED (`USE_LED` unset) produces no flashes. Both paths
compile out rather than no-op at runtime.
