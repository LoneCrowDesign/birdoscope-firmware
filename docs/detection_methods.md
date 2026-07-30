# Detection Methods — Frame Semantics, Geometry, and Backtrace Logic

## 802.11 Address Fields

Every 802.11 management frame carries four address fields in the MAC header. Their meaning depends on the frame type, but for the frames this firmware watches:

| Field   | Probe Request (from camera)   | Probe Response (to camera)                   |
|---------|-------------------------------|----------------------------------------------|
| `addr1` | Broadcast `ff:ff:ff:ff:ff:ff` | Camera MAC (the station that sent the probe) |
| `addr2` | Camera MAC (transmitter)      | AP MAC (the access point replying)           |
| `addr3` | Broadcast `ff:ff:ff:ff:ff:ff` | AP BSSID (same as addr2 for a basic AP)      |

The 802.11 convention is: addr1 = receiver, addr2 = transmitter. For infrastructure mode, addr3 carries the BSSID of the network. The firmware reads all three from `wifi_ieee80211_hdr_3addr_t` (`hdr->addr1`, `hdr->addr2`, `hdr->addr3`).

---

## 2. Flock Detection Methods

### `wildcard_probe` (DeFlockJoplin signature)

**Frame type:** Probe Request (management subtype 4)  
**Trigger:** `addr2` OUI matches a Flock target AND the SSID IE has zero length

**What it means:** The camera is transmitting. Wildcard probe requests (empty SSID) are how a station discovers all available networks on a channel — Flock cameras do this on channel hop to find nearby devices. Zero-length SSID is the distinguishing feature; a camera probing for a specific network would have a non-empty SSID.

**RSSI geometry:** The RSSI is the camera's own transmission received at the scanner. This is the only detection type where `RSSI ∝ 1/distance-to-camera` is geometrically correct. Path-loss triangulation is valid.

**`mac` logged:** `addr2` — the camera MAC.  
**`ap_mac` logged:** `null` — the camera is transmitting, not an AP.

**Confidence:** Highest. A Flock OUI + zero-length SSID combination on a probe request is the most precise available signature. Field false positive rate: ~17% in DeFlockJoplin's Joplin, MO tests (2/12 detections were non-Flock).

---

### `oui_addr2` (transmitter-side catch)

**Frame type:** Any management or data frame  
**Trigger:** `addr2` OUI matches a Flock target (but frame is not a probe request, so not `wildcard_probe`)

**What it means:** A Flock OUI device is transmitting a non-probe frame — likely a data frame, association request, or null frame during an active connection. Less common than probes.

**RSSI geometry:** Same as `wildcard_probe` — the camera is the transmitter, so RSSI reflects camera→scanner path loss. Triangulation is valid.

**`mac` logged:** `addr2` — the camera MAC.  
**`ap_mac` logged:** `null`.

---

### `oui_addr1` (receiver-side / sleeping camera catch)

**Frame type:** Probe Response (management subtype 5) or unicast data frame  
**Trigger:** `addr1` OUI matches a Flock target

**What it means:** The camera's MAC appears as the destination of a frame. For probe responses this is the standard AP behaviour: the AP received the camera's wildcard probe request and replied with a unicast probe response addressed to the camera. The scanner receives this AP-to-camera transmission because it is also on that channel.

**Why this is valuable:** A camera that is in a sleep cycle between network check-ins may not transmit at all during the capture window. The `wildcard_probe` and `oui_addr2` methods both require the camera to be the transmitter and will miss it entirely. But nearby APs will still send probe responses to the camera's MAC if they received a probe from it at any point recently (APs cache probe request MACs briefly). `oui_addr1` catches those responses, providing evidence that a Flock camera is in the area even when it is silent.

**RSSI geometry — why it diverges:**

```
 [Flock camera]  ──probe request──>  [AP]  ──probe response──>  [Birdoscope]
       │                               │                              │
   unknown                         addr2 of                     records
   position                        the frame                    this RSSI
```

The firmware records `pkt->rx_ctrl.rssi`, which is the received signal strength of the AP's probe response at the scanner's antenna. This value reflects **AP → scanner path loss**. The camera is at the other end of a separate link (camera → AP) with completely independent geometry.

The path-loss triangulation model assumes `RSSI ∝ 1/distance-to-camera`. For `oui_addr1` the actual relationship is `RSSI ∝ 1/distance-to-AP`. The solver either converges to the AP's location (wrong target) or diverges — in practice, a stationary scanner will record nearly constant RSSI for the same AP regardless of the camera's exact position, and the solver produces nonsense. Camera position is unconstrained by these observations alone.

**`mac` logged:** `addr1` — the camera MAC (as a destination address).  
**`ap_mac` logged:** `addr2` — the MAC of the AP that sent the probe response. This is zero in captures before the addr2 fix.

---

### `oui_addr3` (BSSID-field catch)

**Frame type:** Any frame where `addr3` carries a network BSSID  
**Trigger:** `addr3` OUI matches a Flock target

**What it means:** A Flock OUI appears as the BSSID in addr3. This would indicate a Flock camera acting as an access point — hosting a network rather than joining one. This is not standard operating behaviour and has not been observed in the field. The method is included as a completeness check and to catch non-standard firmware modes or misconfigured devices.

**`mac` logged:** `addr3`.  
**`ap_mac` logged:** `null`.

---

### `ssid_keyword` (SSID text match)

**Frame type:** Probe Request or Beacon  
**Trigger:** SSID IE in the frame body contains a keyword from the configured list

**What it means:** The device is probing for or advertising a network whose name matches a Flock-specific keyword (e.g. `FLOCK`, `FlockSafety`, or deployment-specific names). This is a secondary enrichment channel — it may identify cameras probing for their configured network name, or it may reveal the network name of the infrastructure they connect to.

**RSSI geometry:** Depends on frame type. If from a probe request, the camera is the transmitter (correct geometry). If from a beacon, an AP is broadcasting the matched SSID (wrong geometry, same issue as `oui_addr1`).

**`mac` logged:** `addr2` — the transmitting station.  
**`ap_mac` logged:** `null`.

---

## 3. Axon Detection Methods

### Axon Enterprise and Subsidiary OUIs

| Prefix | Block | Organization | Relation to Axon | 
|---|---|---|---|---|
| `00:25:DF` | MA-L | Axon Enterprise, Inc. | Primary. Registered 2010-01-05 as TASER International; renamed with the company in 2017. | 
| `FC:01:9E` | MA-L | VIEVU | Body-camera maker, acquired by Axon 2018 | 
| `7C:83:34:4` | MA-M (/28) | Fusus | Real-time crime center / camera aggregation, acquired by Axon Jan 2024 | 
| `84:B3:86:5` | MA-M (/28) | Fusus | Second Fusus block, registered 2022-10-07 | 

Bluetooth SIG company ID `845` (`0x034D`) = "TASER International, Inc." — still the registered name;
Axon never re-registered under the new company name.

Caveats:

- `reference/flockyou-oui.txt` is an MA-L-only dump (`OUI/MA-L` header, 6 MA-M/MA-S rows total), so
  the Fusus /28 blocks cannot resolve from it. `84:B3:86` appears there only as its MA-M parent,
  "IEEE Registration Authority", which corroborates the /28 sub-allocation.
- No IEEE registration found for Dedrone (acquired by Axon 2024); likely ships on a contract
  manufacturer's OUI.

---

### Axon 5 GHz Characteristics

- SSID hidden (empty) 
- Auth `[WPA2_PSK]`
- Channel 149 / 153 / 157 / 161 / 165 only — 5 GHz UNII-3
- MAC sub-ranges: `6d:xx`–`70:xx` (the bulk), plus `82`–`86`, `a1`, `a6`.

Visually confirmed and OUI-matched, but further data capture and behavioral analysis needed.

---

### Axon 2.4 GHz Characteristics

- Not seen on 2.4 GHz, further data capture and behavioral analysis needed.
- Firmware updated to include Axon OUIs as targets while scanning in any mode

---

### Axon BLE Characteristics

- Public AD payloads start with the same 3 bytes: `4D 03` (company ID 845, little-endian) then `02`,
followed by a 9-byte ASCII serial beginning `X`.

- Some payloads show empty manufacturer data, an OUI-only match is what catches them

- AD payloads encode serial number information about the device. Axon's format is X + 2-digit model prefix + 6 alphanumeric, 9 characters total. Confirmed with scan data.

- Observed in the wild:
      - X87: Signal Vehicle Unit
      - X99: Unknown mobile device, not infrastructure but collocated with X87s

- No BLE camera traffic observed, fixed installations seems to broadcast on 5GHz only

---

## 3. The addr2 Backtrace — Resolving oui_addr1 Camera Positions

### Background

`oui_addr1` detections are the only way to confirm a sleeping camera's presence during a capture window. Without them, cameras that happen to not transmit during the drive-by would go undetected. But because the RSSI is geometrically wrong, these are the lowest-quality location estimates in the dataset.

Before the addr2 fix, the full information chain for an `oui_addr1` detection was:
- Camera MAC: known (addr1 of the frame)
- Camera position: unknown — only the scanner's GPS is recorded
- Which AP sent the probe response: unknown — addr2 was discarded

This left no path to a camera location better than "somewhere within ~200m of where the scanner was when it heard this."

### The addr2 fix

With `ap_mac` in the log, the analysis pipeline can perform a **backtrace** with companion wardriving data:

1. The logged `ap_mac` identifies exactly which AP sent this probe response
2. Look up that MAC in `wd3_wifi` (the wardriving scanner's AP database)
3. If the AP has been observed from multiple scanner positions, `triangulated_positions` has a path-loss-fitted location for it
4. The camera must be within that AP's coverage radius to have received a probe response from it — typically 30–150m depending on AP power and environment
5. Use the triangulated AP position as a **bounded location proxy** for the camera

This replaces "somewhere within 200m of the scanner" with "within ~100m of AP at [known coordinates]". It is not as precise as `wildcard_probe` triangulation (which is geometrically direct), but it bounds the search area meaningfully and is the only available method for cameras that never transmit.

**Dependency:** The backtrace only works if the AP was observed by the wardriving scanner. If the AP is absent from `wd3_wifi`, only the scanner's GPS is available as a location bound. This is still the same quality as before the fix, so the fix is never worse.

**For the same camera MAC that also has `oui_addr2` or `wildcard_probe` detections** (i.e., it transmitted at some point during the session), those detections provide a direct geometric fix and take precedence. The addr2 backtrace is the fallback for cameras that only appear via `oui_addr1`.

---

## Method Priority for Location Estimation

When a camera MAC has been detected by multiple methods, prefer in this order:

| Priority | Method                        | Why                                                                                    |
|----------|-------------------------------|----------------------------------------------------------------------------------------|
| 1        | `wildcard_probe`              | Camera is transmitting; RSSI is geometrically correct; highest-confidence signature    |
| 2        | `oui_addr2`                   | Camera is transmitting; RSSI is correct; slightly lower confidence than wildcard probe |
| 3        | `oui_addr1` + addr2 backtrace | AP position proxies camera position; bounded but not direct                            |
| 4        | `oui_addr1` centroid only     | Scanner GPS centroid; only useful for confirming approximate area                      |

A camera MAC with only `oui_addr1` hits and no matching AP in `wd3_wifi` falls into priority 4 and should be flagged as `position_quality=low` in output.

---

## Single-Observation Distance Estimate

A single RSSI reading also yields a coarse range, independent of the triangulation above and answering a different question: triangulation places a target on a map from many GPS-anchored observations, whereas this gives a distance from wherever the scanner stood, from one reading.

It applies to `wildcard_probe` and `oui_addr2` hits only, priorities 1 and 2. For `oui_addr1` the RSSI describes the AP link instead, so no distance is reported, for the same reason the solver cannot use those observations.

The model, its two calibration settings, and their accuracy limits are covered in [Distance estimation](distance_estimation.md).

---

## Why the SSID Field Is Empty for Most Detections

The `ssid` field in the log is only populated for `ssid_keyword` detections. For `wildcard_probe`, the SSID IE is zero-length by definition (it's the wildcard). For `oui_addr1` and `oui_addr2`, the firmware does not parse the frame body — it only inspects the MAC header. This is intentional: full frame body parsing in the IRAM callback is expensive and the OUI/addr matching is sufficient for all primary detection methods.
