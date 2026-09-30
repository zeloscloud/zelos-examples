---
description: The DC-DC converter stays within the pack allowance the BMS gives it.
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]
- tzdata
params:
  window: -5m
  settle_samples: 2
  tolerance_a: 0.02
  wait_s: 240
  zoom_before_s: 4
  zoom_after_s: 12
  defect_trace: /traces/dcdc-defect.trz
---

`BMS_Limits.AuxCurrentLimit` is the pack current the DC-DC converter may draw.
The BMS lowers it when the vehicle starts charging, and the converter has to
respect the new value promptly.

**The rule.** The draw may sit above the allowance for at most `settle_samples`
samples per reduction.

Some grace is unavoidable. The two signals live in different messages at
different rates, so when the allowance drops there is always a draw sample
already in flight under the old one. The grace is counted in samples rather
than milliseconds, because the bench runs slower than wall clock and samples
measure the converter's response in its own terms.

One check, against the live bus: the build on the bench must keep to the rule.
`traces/dcdc-defect.trz`, a recording of the defective firmware, is here to show
what breaking it looks like. Run `just flash defect` and the check goes red.

```python {#e0beda7e}
from datetime import timedelta

from zelos_sdk import CheckResults, connect

agent = connect()

LIMIT = "*0201_BMS_Limits.AuxCurrentLimit"
DRAW = "*0300_DCDC_Status.InputCurrent"


def signals(source, **window):
    """Both signals on one clock.

    They arrive at different rates, so the join is sparse: one row per distinct
    timestamp, null where the other message had no sample. ffill() carries the
    last allowance forward, which is how the converter reads it too, and
    dropna() drops the leading rows with nothing to hold yet.
    """
    frame = source.query([LIMIT, DRAW], **window).ffill().dropna().short_names()
    return frame["AuxCurrentLimit"], frame["InputCurrent"]


def stepped_down(limit):
    """Every sample where the allowance is lower than the one before it."""
    return limit.diff().to_pandas() < -params.tolerance_a


def first_step(limit):
    """When the allowance first steps down, which is what the plot centres on."""
    steps = stepped_down(limit)
    return steps.index[steps][0]


def zoom(source, at):
    """The same two signals around one instant.

    A chart embeds its data, so plotting five minutes at full rate makes a
    megabyte-and-a-half page in which the fault is one pixel wide.
    """
    return source.query(
        [LIMIT, DRAW],
        start=at - timedelta(seconds=params.zoom_before_s),
        end=at + timedelta(seconds=params.zoom_after_s),
    ).ffill().dropna().short_names()


```

## The defective firmware

`traces/dcdc-defect.trz` is a recording of the bench running the build that
applies its rate cap to reductions as well as increases. On a trace the file is
the clock, so this reads the same every time.

Where the allowance steps down, the draw should drop with it. It walks instead.

```python {#4363f754}
trace = agent.trace(params.defect_trace)
limit, draw = signals(trace, start="start")
zoom(trace, first_step(limit)).plot()
```

## The firmware on the bench

The same two signals, live. The allowance drops once per drive cycle, so this
waits for one rather than assuming the bench has been running long enough.

```python {#2dfba5bb}
import time

deadline = time.monotonic() + params.wait_s
while True:
    limit, draw = signals(agent, start=params.window)
    if stepped_down(limit).any() or time.monotonic() > deadline:
        break
    print("waiting for the BMS to lower the allowance...")
    time.sleep(10)

zoom(agent, first_step(limit)).plot()
```

```python {#6e988a05}
excess = (draw - limit).rename("InputCurrent over AuxCurrentLimit")
steps = int(agent.check.count(limit.diff(), "<", -params.tolerance_a))
over = int(agent.check.count(excess, ">", params.tolerance_a))

CheckResults([
    agent.check.that(over, "<=", params.settle_samples * steps, name="the bench keeps to the rule"),
]).raise_if_failed()
```
