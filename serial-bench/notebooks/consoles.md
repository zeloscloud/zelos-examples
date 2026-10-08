---
description: What the Serial extension records from two boards' consoles.
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]==0.0.12
- tzdata
params:
  window: -2m
  history: -15m
---

The bench has two boards whose only interface is a console: a Zephyr node that
logs a DC-DC converter at 50 Hz and takes shell commands, and Renode's stock
Linux. The Serial extension records both. This notebook shows what it made of
them: log events, values in volts and amps, and command replies.

```python
import time
from datetime import datetime, timedelta, timezone

from zelos_sdk import connect

agent = connect()
node = agent.query("Serial/dut/log.*", start=params.window).short_names().to_pandas()
```

## The node's log

Every line the node prints is a log event with its Zephyr level and module.

```python
node.groupby(["level", "name"]).size().rename("lines").to_frame()
```

Every 5 s the `bms` module warns about cell imbalance.

```python
node[node["name"] == "bms"].tail()
```

## The rail

`rail=13.64V` in a `dcdc` line becomes a value in volts. Its time stamp comes
from the node's own clock, so 50 Hz on the device is 20 ms between samples.

```python
rail = agent.query("Serial/dut/dcdc.rail", start="-10s")["Serial/dut/dcdc.rail"]
interval_ms = rail.index.to_series().diff().median().total_seconds() * 1000
print(f"{len(rail)} samples in {rail.unit}, {interval_ms:.1f} ms apart")
rail.plot()
```

## A shell command

A command returns the node's reply once its prompt comes back. This cell lowers
the input current limit for a second, then sets it back to 4.5 A. Each command
also lands on the node's log as a `tx` row.

```python
sent = datetime.now(timezone.utc)
replies = []
for text in ("dcdc limit set 2.0", "dcdc limit set 4.5"):
    replies.append(agent.actions.execute("Serial/command", {"port": "dut", "text": text}).value)
    time.sleep(1)
replies
```

The input current follows the limit down and back up.

```python
agent.query(
    ["Serial/dut/dcdc.in", "Serial/dut/dcdc.limit"], start=sent - timedelta(seconds=1)
).short_names().plot()
```

## Linux's boot

Linux prints its boot log and stops at a login prompt. Renode's stock image
prints the kernel log without time stamps, so its lines are plain log events,
after the firmware's. The login prompt is not logged.

```python
boot = agent.query("Serial/linux/log.*", start=params.history).short_names().to_pandas()
start = boot.message.str.contains("Booting Linux").idxmax()
boot.loc[start:, ["level", "message"]].head(20)
```
