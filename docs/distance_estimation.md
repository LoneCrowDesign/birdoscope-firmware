# Distance Estimation and Triangulation

## Single-Observation Distance Estimate

`coreRssiToDistanceM()` in `lib/birdoscope_core/core.cpp` converts one RSSI reading to a range using the log-distance path-loss model:

```
d = 10 ^ ((RSSI_1m - RSSI_measured) / (10 * n))
```

The `dst:` row in the Overview screen is this value. It stays independent of the triangulation calc and gives an approximate estimation so you have an idea of where to look when a flock is detected. Triangulation needs several observations to place the target accurately, but RSSI gives a point-to-point estimation with one reading.

The firmware applies it to `wildcard_probe` and `oui_addr2` hits only. `oui_addr1` hits are a measure between the birdoscope and an AP responding to a flock probe, so the value is shown as `dst:--` to prevent confusion.

### Calibration

The model has two values a user can set, which are adjusted with the `calibrate` command in the web console.

Running `calibrate` with no arguments reports the current model against the last real detection, so there is something concrete to judge:

```
density=medium (n=2.5) rssi_1m=-45dBm; last detection -78dBm reads ~20.9m
```

A value or step outside the accepted range is clamped. The command reports what was asked for alongside what was applied, rather than capping it silently.

#### Environment Density

Environment Density picks `n`, the rate at which signal decays with distance:

| Setting | `n`  | Surroundings                            |
|---------|------|-----------------------------------------|
| Low     | 2.0  | open ground, near line of sight         |
| Medium  | 2.5  | mixed suburban, scattered obstructions  |
| High    | 3.5  | dense urban, heavy obstruction          |

These are presets rather than inputs because `n` varies in the field and cannot be measured there. Higher density reads a given RSSI as nearer, since faster decay explains a weak signal with less distance. Medium is the default.

#### RSSI Trim

The expected RSSI at 1m is the reference level the model decays from. It absorbs everything that scales received power uniformly at all distances: antenna gain, cable loss, the board radio, and the target's transmit power. There is deliberately no separate antenna setting, since gain and this reference are the same term; fitting a different antenna means re-reading the value at 1m.

It is an absolute dBm rather than an offset, because an absolute value is something you can go and obtain. Stand a metre from a target, read the RSSI off the Detections screen, and enter that as `rssi_1m`. Free-space loss at 2.4 GHz over 1m is about 40 dB. A target transmitting at 10 to 20 dBm should therefore read near -20 to -30 dBm. Real readings come in weaker, from the near field, antenna mismatch, and the receiver's own RSSI calibration. The accepted range is -85 to -20 dBm and the default is -45.

`rssi_trim` steps that reference rather than replacing it, which is the intended adjustment when a camera is in sight and the estimate is visibly wrong. Nudge up or down until the reading matches what you can see. Positive steps read farther, since a stronger expected signal up close means a given weak reading must be more distant.

These settings persist, so once it's dialed in, you should be fairly accurate until you change the pathloss exponent which requires a re-estimation of RSSI vs distance.

| Expected RSSI at 1m | A -80 dBm hit reads (medium) |
|---------------------|------------------------------|
| -35                 | 63.1 m                       |
| -45 (default)       | 25.1 m                       |
| -55                 | 10.0 m                       |
| -65                 | 4.0 m                        |

### Accuracy

Distance is always an approximation, given the number of variables, and the controls remove systematic bias rather than noise. Multipath swings instantaneous RSSI by 6 to 10 dB, which is a 1.7 to 2.5x distance error on its own. Transmit power also varies by camera model. Read the result as an order of magnitude rather than a precise reading.

### Where the values live

Persisted to SPIFFS at `/settings.json` as `{"density":N,"rssi_1m":N}`, loaded by `coreSettingsLoad()` from `setup()`. A missing or unparseable file is not an error; the defaults stand.

`RSSI_AT_1M` in `board_config.h` seeds the default reference, and `PATH_LOSS_N_LOW`, `PATH_LOSS_N_MED`, and `PATH_LOSS_N_HIGH` override the density presets. `PATH_LOSS_N_MED` defers to `PATH_LOSS_N`, so a board that already tuned that value keeps it.

`main_tft.cpp` does not call `coreSettingsLoad()`, so saved settings are ignored on the round TFT board until that is backported. Core still computes the estimate there. See [Board parity](board_parity.md).
