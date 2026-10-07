# Detection Methods

How the firmware matches captured frames to target devices, what each match says
about where the target is, and how analysis turns matches into positions.

## 802.11 Address Fields

Every 802.11 management frame contains three address fields, and their meaning
depends on the frame type.

| Field   | Probe Request (from camera)   | Probe Response (to camera)                   |
|---------|-------------------------------|----------------------------------------------|
| `addr1` | Broadcast `ff:ff:ff:ff:ff:ff` | Camera MAC (the station that sent the probe) |
| `addr2` | Camera MAC (transmitter)      | AP MAC (the access point replying)           |
| `addr3` | Broadcast `ff:ff:ff:ff:ff:ff` | AP BSSID (same as addr2 for a basic AP)      |

addr1 is always the receiver and addr2 the transmitter. The firmware reads all
three from `wifi_ieee80211_mac_hdr_t`.

Each `wifi_obs` row in the roost log holds `mac`, the address that matched,
beside the header's `addr1`, `addr2` and `addr3`. The serial JSON detection line
follows the upstream flock-you schema instead, with the replying AP in `ap_mac`
for an `oui_addr1` hit.

## Flock Detection Methods

### `wildcard_probe` (DeFlockJoplin Signature)

- Frame type: Probe Request (management subtype 4)
- Trigger: `addr2` OUI matches a target and the SSID IE has zero length

Stations send wildcard probes to discover every network on a channel, and Flock
cameras send them on each channel hop. The RSSI measures the camera's own
transmission, so path-loss triangulation is valid.

- `mac` logged: `addr2`, the camera MAC.

This is the highest-confidence signature. In DeFlockJoplin's Joplin, MO field
tests, 2 of 12 detections were non-Flock, about 17% false positives.

### `directed_probe` (Named Probe)

- Frame type: Probe Request (management subtype 4)
- Trigger: `addr2` OUI matches a target and the SSID IE is non-empty

The probed name identifies the backhaul network the camera joins.

RSSI geometry matches `wildcard_probe`.

- `mac` logged: `addr2`, the camera MAC.

### `oui_addr2` (Transmitter-Side Catch)

- Frame type: Any captured management or data frame
- Trigger: `addr2` OUI matches a target, and the frame is neither a wildcard
  nor a directed probe request

Usually a data frame, association request or null frame during an active
connection, all less common than probes. RSSI geometry matches
`wildcard_probe`.

- `mac` logged: `addr2`, the camera MAC.

### `oui_addr1` (Receiver-Side, Sleeping Camera Catch)

- Frame type: Probe Response (management subtype 5) or unicast data frame
- Trigger: `addr1` OUI matches a target

An AP that recently heard the camera's probe replies with a unicast probe
response, and the scanner overhears it on the same channel. This catches a
camera sleeping between check-ins that sends nothing during the capture window,
which every direct method misses.

```text
 [Flock camera]  ──probe request──>  [AP]  ──probe response──>  [Birdoscope]
       │                               │                              │
   unknown                         addr2 of                     records
   position                        the frame                    this RSSI
```

The RSSI measures the AP-to-scanner link and says nothing about where the camera
sits. The path-loss solver converges on the AP or diverges, so these
observations alone leave the camera unplaced.

- `mac` logged: `addr1`, the camera MAC.
- The row's `addr2` holds the AP that replied.

### `oui_addr3` (BSSID-Field Catch)

- Frame type: Management frame, with `CHECK_ADDR3` set
- Trigger: `addr3` OUI matches a target

addr3 names the network, not the transmitter. The camera sends the frame when it
is the AP or transmits under a randomized `addr2`, but a client joining the
camera's network sends it otherwise, so RSSI geometry varies by frame. Every
current board leaves `CHECK_ADDR3` off.

- `mac` logged: `addr3`.

### `ssid_match` (SSID Text Match)

- Frame type: Probe Request, Probe Response, or Beacon, with
  `ENABLE_SSID_MATCH` set
- Trigger: SSID IE in the frame body contains a keyword from the configured
  list

A match on a Flock network name such as `FLOCK` or `FlockSafety` is secondary
enrichment. It can identify a camera probing for its configured network, or the
infrastructure it connects to.

RSSI geometry depends on frame type. In a probe request the camera is the
transmitter, so the geometry is correct. In a beacon an AP broadcasts the
matched SSID, which has the same wrong geometry as `oui_addr1`.

- `mac` logged: `addr2`, the transmitting station.

### Target OUI Table Provenance

Flock Safety holds one IEEE assignment, the MA-L block `B4:1E:52`, but builds
most of its hardware from third-party modules that will not match it. The
other Flock prefixes in `lib/birdoscope_core/core.cpp` come from field
observation of those modules. Most resolve to one contract module manufacturer,
the rest to a handful of silicon and module vendors. One resolves to no registry
and sits one hex digit from a live block, likely a transcription error, so
confirm it against its source. One more has the locally administered bit set,
the mark of a derived virtual-interface address, so no registry lists it.

The Axon entries are all vendor registrations, including acquired subsidiaries,
listed in the table below.

Keep two risks of the field-observed entries in mind when you read a capture.

- **They fail together.** A module supplier change, a hardware revision on a
  different block, or a move to a registered prefix retires most of the Flock
  table at once. A fleet-wide drop in Flock matches while the rest of the
  detection path works more likely means this than an absence of cameras.
- **They match broadly.** A module vendor's MA-L matches every device built on
  that module. That is the bulk of the known false positives, and the
  wildcard-probe signature is the OUI-independent second check.

You can re-audit the table against the IEEE registry, which publishes the MA-L,
MA-M and MA-S assignments as CSV at `standards-oui.ieee.org`. A name search for
"Axon" also returns several unrelated networking companies alongside Axon
Enterprise.

## Axon Detection Methods

### Axon Enterprise and Subsidiary OUIs

| Prefix | Block | Organization | Relation to Axon |
|---|---|---|---|
| `00:25:DF` | MA-L | Axon Enterprise, Inc. | Primary. TASER International registered it 2010-01-05, and it took the company's new name in 2017. |
| `FC:01:9E` | MA-L | VIEVU | Body-camera maker, acquired by Axon 2018 |
| `7C:83:34:4` | MA-M (/28) | Fusus | Real-time crime center and camera aggregation, acquired by Axon Jan 2024 |
| `84:B3:86:5` | MA-M (/28) | Fusus | Second Fusus block, registered 2022-10-07 |

The Bluetooth SIG lists Axon's company ID `845` (`0x034D`) under its old name,
"TASER International, Inc."

Caveats:

- The upstream flock-you OUI dump covers MA-L only, so the Fusus /28 blocks do
  not resolve from it. `84:B3:86` appears there as its MA-M parent, "IEEE
  Registration Authority", which corroborates the /28 sub-allocation.
- The IEEE registry lists no assignment for Dedrone (acquired by Axon 2024),
  which likely ships on a contract manufacturer's OUI.

### Axon 5 GHz Characteristics

- SSID hidden (empty)
- Auth `[WPA2_PSK]`
- Channel 149, 153, 157, 161 or 165 only, the 5 GHz UNII-3 band
- MAC sub-ranges `6d:xx` to `70:xx` (the bulk), plus `82` to `86`, `a1`, `a6`

Visual sighting and OUI match confirm these. Their behavior needs more capture
and analysis.

### Axon 2.4 GHz Characteristics

- No sighting on 2.4 GHz yet. This needs more capture and behavioral analysis.
- The firmware matches Axon OUIs as targets in every scan mode.

### Axon BLE Characteristics

- Public AD payloads start with `4D 03` (company ID 845, little-endian) and
  `02`, then a 9-character ASCII serial made of `X`, a 2-digit model prefix and
  6 alphanumerics. Scan data confirms the format.
- Some payloads contain empty manufacturer data, and only an OUI match catches
  them.
- Prefixes seen in the wild:
  - X87: Signal Vehicle Unit
  - X99: Unknown mobile device, not infrastructure but collocated with X87s
- No BLE camera traffic seen yet. Fixed installations seem to broadcast on
  5 GHz only.

## The addr2 Backtrace for `oui_addr1` Camera Positions

On its own, an `oui_addr1` hit places the camera within about 200 m of where the
scanner heard it. The row's `addr2` names the AP that replied, and the analysis
pipeline narrows the bound with companion wardriving data:

1. Look up the row's `addr2` in `wd3_wifi`, the wardriving scanner's AP
   database.
2. If wardriving scanners saw the AP from several positions,
   `triangulated_positions` holds a path-loss-fitted location for it.
3. The camera heard the AP's reply, so it sits within the AP's coverage radius,
   typically 30 to 150 m depending on AP power and environment.
4. Use the AP's position as a bounded location proxy for the camera.

The result is coarser than direct triangulation but is the only bound for a
camera that never transmits. If no wardriving scanner saw the AP, only the
scanner's GPS bounds the location.

## Method Priority for Location Estimation

When multiple methods detect a camera MAC, prefer them in this order.

| Priority | Method                        | Why                                  |
|----------|-------------------------------|--------------------------------------|
| 1        | `wildcard_probe`              | Direct, highest-confidence signature |
| 2        | `oui_addr2`                   | Direct, slightly lower confidence    |
| 3        | `oui_addr1` + addr2 backtrace | The AP's position bounds the camera  |
| 4        | `oui_addr1` centroid only     | Confirms the approximate area only   |

The output marks a priority-4 position `position_quality=low`.

## Direct, Indirect and Conditional

Each detection method falls into one of three kinds, split by whether the target
itself transmitted.

| Kind        | Methods                                         | Meaning                                                           |
|-------------|-------------------------------------------------|-------------------------------------------------------------------|
| Direct      | `wildcard_probe`, `directed_probe`, `oui_addr2` | The target transmitted the frame, so RSSI describes the target    |
| Indirect    | `oui_addr1`                                     | An AP answered a target's probe, so RSSI describes the AP link    |
| Conditional | `oui_addr3`, `ssid_match`                       | The transmitter depends on the frame, so RSSI may describe either |

Only direct detections have a usable scanner-to-camera range, so only they fire
the new-detection chirp, seed the proximity ring, and produce a `dst:` reading.
An indirect detection still counts, logs and flashes the LED, and reads
`dst:via AP`. The firmware currently handles conditional detections as direct,
pending logic that tells the cases apart. See [Alert behavior](alerts.md).

### Devices on the Panel

The Detections screen shows `dir:` and `ind:` as camera counts, deduped per
camera per direction. APs never count.

The two overlap. A camera seen directly and later through an AP reply counts in
both, so `dir + ind` can exceed the `devices:` total above them. Each answers an
independent question. `dir` is roughly what you can visually locate from where
you are, and `ind` is what is in the area with no usable range.

### Frames Off the Panel

The OLED boards' `status` line and the web portal console report the raw frame
tallies `direct` and `indirect`, which count matching frames instead of devices.
`coreHandleAlert()` increments them ahead of the repeat-suppression gate and
independently of every alert setting, so they keep counting while the alert
path stays quiet for a target it has already announced.

The same two consoles report `seen`, every frame the radio delivered, and
`cand`, the frames left after the type, length and RSSI filters. Zero devices
with `seen` climbing is a quiet area, and zero devices with `seen` flat is a
deaf radio. The portal stops the sniffer, so its figures hold their last value
while it runs. The TFT board's `status` line reports none of these.

The Detections screen's bottom row shows the same traffic count with its rate
and the uptime, as `seen:` on 802.11 and `adv:` (advertisements) on BLE. A full
detection table takes that row for its `FULL! missed:` warning.

## Single-Observation Distance Estimate

A single RSSI reading also gives a coarse distance from wherever the scanner
stood, separate from the map triangulation above. It applies to every direct
method, and the firmware reports none for `oui_addr1`.

[Distance estimation](distance_estimation.md) covers the model, its two
calibration settings, and their accuracy limits.

## Receiver Sensitivity Floor

The sniffer discards any frame below `RSSI_MIN` before it reaches the capture.
On the Analyze boards it is -100 dBm, deliberately below the roughly -95 dBm at
which the radio stops reporting usefully. Other boards default to -95 dBm.

Analysis can filter a weak frame the device wrote but can never recover one it
dropped, so a cutoff below the usable floor keeps the marginal tail of real
detections and leaves the quality decision to analysis.

The cost is more noise. Treat detections near the floor as evidence of presence
only, since their RSSI is too unreliable for a distance estimate.

## Why the SSID Field Is Empty for Most Detections

The firmware captures the SSID IE from every management frame that contains
one, whatever method matched, and logs it in the `ssid` field. A
`wildcard_probe` has a zero-length SSID IE by definition, and a data frame has
no SSID IE at all, so either one logs an empty `ssid`.
