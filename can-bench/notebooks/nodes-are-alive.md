---
description: Every node on the bus is still transmitting.
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]
- tzdata
params:
  window: -60s
  min_frames: 5
---

A node that stops transmitting reports nothing: not in Renode's log, not in the
interface counters, and not in the CAN extension, which has no receive-idle
detection. An absent signal looks the same as a flat one, so the absence needs
its own check.

Every message carries a counter, which is the cheapest proof of life: it
changes on every frame, whatever the node is doing.

This does not check a rate. The bench runs at about a fifth of wall clock
because three emulated Cortex-M7 cores share one host, so a rate assertion would
test the host rather than the firmware.

```python {#d539e967}
import pandas as pd
from zelos_sdk import CheckResults, connect

agent = connect()

COUNTERS = {
    "VCU_Command": "*0100_VCU_Command.Heartbeat",
    "BMS_Status": "*0200_BMS_Status.Counter",
    "BMS_Limits": "*0201_BMS_Limits.Counter",
    "BMS_CellVoltages": "*0202_BMS_CellVoltages.Counter",
    "DCDC_Status": "*0300_DCDC_Status.Counter",
}

# A node that never transmitted leaves nothing to resolve, and an unmatched
# pattern fails the whole query, so resolve through the catalog first and query
# only what it knows. Silence is this notebook's subject, not an error it
# should die on.
catalog = agent.signals()
found = {name: catalog.match(pattern) for name, pattern in COUNTERS.items()}
present = {name: str(hits[0].path) for name, hits in found.items() if len(hits)}

frame = agent.query(list(present.values()), start=params.window) if present else None
samples = frame.to_pandas() if frame is not None else pd.DataFrame()

arrived = {
    name: int(samples[path].count()) if path in samples else 0
    for name, path in ((n, present.get(n)) for n in COUNTERS)
}
span_s = (samples.index[-1] - samples.index[0]).total_seconds() if len(samples) > 1 else 0.0

pd.DataFrame(
    [
        {
            "message": name,
            "frames": arrived[name],
            "per second": round(arrived[name] / span_s, 2) if span_s else 0.0,
        }
        for name in COUNTERS
    ]
).set_index("message").rename_axis(None)
```

One counter per node, so the chart stays readable. Each is a byte, so a live
node ramps and wraps. The converter sends twice as often as the other two and
wraps twice as fast; the VCU and the BMS advance in step, so their two lines sit
on top of each other. A flat line is a node that has stopped.

```python {#c1218aa1}
charted = [present[name] for name in ("VCU_Command", "BMS_CellVoltages", "DCDC_Status") if name in present]

agent.query(charted, start=params.window).short_names().plot() if charted else "No frames arrived."
```

```python {#f7798c8d}
results = CheckResults()

for name, count in arrived.items():
    results.append(
        agent.check.that(
            count, ">=", params.min_frames,
            name=f'{name} advances in {params.window}',
        )
    )
results.raise_if_failed()
```
