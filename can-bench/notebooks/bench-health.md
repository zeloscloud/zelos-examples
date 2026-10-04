---
description: Every node on the bench is talking, and what it says makes sense.
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]==0.0.12
- tzdata
params:
  window: -2m
  min_frames: 5
---

A quick health check for the whole bench. It looks at the last two minutes of
the bus and asks two things: is every node still sending, and do its values make
physical sense? It won't find subtle bugs, but it catches a node that has gone
quiet, a scaling mistake in the DBC, or a unit error in firmware.

```python
from zelos_sdk import CheckResults, connect

agent = connect()
bus = agent.query(
    [
        "*0100_VCU_Command.RequestedMode",
        "*0200_BMS_Status.PackCurrent",
        "*0200_BMS_Status.StateOfCharge",
        "*0300_DCDC_Status.OutputVoltage",
        "*0300_DCDC_Status.Temperature",
    ],
    start=params.window,
).short_names()
```

## The drive cycle

The VCU loops through standby, drive and charge. Pack current shows it best:
positive while driving, negative while charging.

```python
bus["PackCurrent"].plot()
```

## The checks

A node that stops sending doesn't report an error anywhere, so each one gets a
check that it's still talking. The rest are sanity bounds, deliberately loose.

```python
results = CheckResults([
    agent.check.that(bus["RequestedMode"].count(), ">=", params.min_frames, name="the VCU is sending"),
    agent.check.that(bus["StateOfCharge"].count(), ">=", params.min_frames, name="the BMS is sending"),
    agent.check.that(bus["OutputVoltage"].count(), ">=", params.min_frames, name="the DC-DC is sending"),
    agent.check.that(bus["StateOfCharge"], ">=", 0.0),
    agent.check.that(bus["StateOfCharge"], "<=", 100.0),
    agent.check.that(bus["OutputVoltage"], ">=", 12.8),
    agent.check.that(bus["OutputVoltage"], "<=", 14.8),
    agent.check.that(bus["Temperature"], "<=", 85.0),
])
results
```

A failed check fails the run, which is what makes this notebook a test.

```python
results.raise_if_failed()
```
