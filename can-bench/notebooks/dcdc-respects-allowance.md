---
description: The DC-DC converter stays under the current the BMS allows it.
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]==0.0.12
- tzdata
params:
  window: -5m
  settle_samples: 2
  tolerance_a: 0.02
  wait_s: 240
---

`AuxCurrentLimit` in `BMS_Limits` is the pack current the DC-DC converter may
draw. The BMS lowers it when charging starts, and the converter should drop its
draw straight away.

The two signals come from different messages at different rates, so when the
allowance steps down there can be a draw sample already in flight under the old
value. The rule allows `settle_samples` of those per step, and no more.

```python
import time
from datetime import timedelta

from zelos_sdk import CheckResults, connect

agent = connect()
SIGNALS = ["*0201_BMS_Limits.AuxCurrentLimit", "*0300_DCDC_Status.InputCurrent"]


def query(**window):
    """Both signals on one clock, each holding its last value between samples."""
    return agent.query(SIGNALS, **window).ffill().dropna().short_names()


def cuts(frame):
    """Every sample where the allowance steps down."""
    return frame["AuxCurrentLimit"].diff().to_pandas() < -params.tolerance_a


# The allowance drops once per drive cycle, so wait for a cut rather than
# assume the bench has been up long enough.
deadline = time.monotonic() + params.wait_s
while not cuts(both := query(start=params.window)).any():
    if time.monotonic() > deadline:
        raise TimeoutError("the BMS never lowered the allowance")
    time.sleep(10)
```

## The last cut

The draw should drop with the allowance. On the defective build it walks down
after it instead.

```python
steps = cuts(both)
at = steps.index[steps][-1]
query(start=at - timedelta(seconds=3), end=at + timedelta(seconds=6)).plot()
```

## The check

```python
limit, draw = both["AuxCurrentLimit"], both["InputCurrent"]
over = int(agent.check.count(draw - limit, ">", params.tolerance_a))

results = CheckResults([
    agent.check.that(over, "<=", params.settle_samples * int(steps.sum()), name="the DC-DC stays under its allowance"),
])
results
```

A failed check fails the run, which is what makes this notebook a test.

```python
results.raise_if_failed()
```
