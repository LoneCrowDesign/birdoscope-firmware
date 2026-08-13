# Roost Logging

Birdoscope writes its captures through roost, a shared logging contract used by
every device in the fleet. The contract itself lives in the vendored library at
[`vendor/jellybeans/roost_logging`](../vendor/jellybeans/roost_logging), which is
authoritative for the record layouts, field vocabulary, and value encodings. This
document covers what Birdoscope does with it, and the two constraints that are
not visible from reading either the library or the firmware alone.

## SD Structure

A session is a directory, not a file. It holds one CSV per record type plus a
`manifest.json` describing the device and the session.

The manifest is the reason the CSVs are interpretable. It records what the build
was capable of capturing, so a consumer can tell an empty column that was never
capturable from one the device could have filled and did not. An absent value is
only meaningful when the capture states its own limits, which is why capability
declarations are compile-time and a board that omits one fails to build rather
than defaulting to zero.

## Capture Performance

The queue depth and flush figures reported by `status` exist to size two
decisions against measured behaviour rather than an estimate.

### Why the alert queue cannot grow freely

Information elements have to travel through the alert queue. The driver owns the
frame buffer and reclaims it once the sniffer callback returns, so nothing can be
parsed later; whatever a row needs must be copied out while the callback runs.

The cost that matters is not the memory. A larger per-entry copy lengthens the
callback, and callback duration is itself what drops frames, so an over-generous
entry makes the capture worse in the way the queue is meant to protect. Both IE
lists are bounded, and truncation raises `buffer_full` rather than writing a short
list that would look like a complete one.

`coreQueueDepthMax` records the deepest the ring has been and `status` reports it
as `qmax=N/32`. A full ring is detected one entry short, so the maximum
observable value is one below the queue size and reaching it means saturation. A
run that never leaves single digits says the entry can afford to grow. One that
approaches saturation, or any non-zero `qdrop`, says the opposite, and says it
before the entry is enlarged.

### Flush Cadence and Reasoning

Flushing every row is the safest policy for power loss and the most expensive
one for throughput. On FAT, flush cost is dominated by the metadata update rather
than the payload. It does not shrink with row size, and a session writing
several files at once multiplies the cost.

Worst-case flush latency is a property of the card rather than of the load. It
comes from an erase-block stall, so it occurs at any row rate and sits in the
path of every row under a per-row policy. A stall of that kind can exceed the
time the alert queue takes to fill, in which case a single flush costs more
frames than the queue can hold, and the loss is silent except for a counter.

The trade on the other side is power loss. The device rides a vehicle and is
unplugged rather than shut down, so a cadence longer than the row rate loses the
tail of every session. The policy is chosen against both bounds, and
`status` reports `rows`, `avg_row` and `worst_flush` so the cost of a flush at
the current row width is a measurement rather than an assumption.

The properties the buffered writer has to hold are asserted host-side in
`test/test_sdlog`, against a memory backend, because none of them are observable
on the device.

## Component attribution

Every `config_change` row names the component the setting governs. It is never
attributed to `sys` by default.

Settings that describe a radio carry that radio's component id: the observation
mode, channel plan, regulatory domain, dwell, and scan period. `sys` carries only
settings that have no component to name, such as storage, buffer, boot and
vocabulary events.

A device with one radio reads the same under either convention, which is what
makes the rule easy to drop. A device with two radios cannot express itself at
all without it, because a reader has no way to tell which radio changed. The
convention is fleet-wide for that reason, not local to this firmware.

Two further rules follow from the record being sparse:

- **Re-applying a value writes nothing.** A settings screen that reconfirms the
  current value, or a menu path that reasserts the current mode, produces no row.
  The held value is tracked per component and per setting, so two radios reporting
  the same setting do not suppress each other.
- **A value is only held once its row reached the card.** Holding it earlier
  leaves the device believing it recorded a value the artifact never carried, and
  suppressing every later write of it.

Both settings this build can change mid-session write their row from inside the
setter rather than from the menu call sites, so a new path to a setter cannot
reintroduce the gap.

## Values that do not fit

A value too large for its buffer is refused, not truncated. A shortened channel
list reads as a narrower plan that was deliberately chosen, which is a different
and entirely plausible capture, with nothing in the artifact marking it as
damage. Refusing loses the row and raises the refusal path, which is visible.

The renderings for the `list` and `map` types are implemented once, in the
library's `runtime/roost_value.h`, and are not reimplemented per device. A type
declaration alone does not fix a rendering, and separate implementations of one
declared type cannot be parsed without knowing which device wrote the row.
