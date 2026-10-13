---
description: Bring up a motor controller over its serial console, recorded by the Serial extension.
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]==0.0.12
- tzdata
params:
  port: motor
---

A NUCLEO-H723ZG runs a brushed DC motor speed controller. Its only debug
interface is its serial console: Zephyr's log and shell on the ST-LINK's
virtual COM port at 921600 baud. The motor is simulated on the chip, since none
is wired to the board; the control loop, watchdog, flash, crash handling and
sensors are real.

The Serial extension records the console: each line a log event with its level
and module, each printed value a signal with its unit, all on the board's own
clock. This notebook drives the board from its shell, as you would at a bench,
and checks what Serial recorded. The last cell fails the run if any check
failed.

```python {#9305f069}
import re
import time
from datetime import datetime, timezone

from zelos_sdk import CheckResults, connect

agent = connect()
S = f"Serial/{params.port}"
results = CheckResults()


def shell(text):
    """Run a command on the board's shell, through Serial."""
    return agent.actions.execute("Serial/command", {"port": params.port, "text": text})


# Start from the gains the unit ships with, and the motor stopped.
shell("pid defaults")
shell("pid save")
shell("motor stop")
agent.latest(f"{S}/motor.*")
```

## Boot

A cold reboot. The firmware logs its version, why the board reset, and the
tuning it loaded from flash. Serial notes the restart, because the board's
clock went back to zero.

```python {#30516bb8}
since = datetime.now(timezone.utc)
shell("kernel reboot cold")
time.sleep(3)

log = agent.query(f"{S}/log.message", start=since)[f"{S}/log.message"]
boot = CheckResults([
    agent.check.that(log, "contains", "[serial] device restarted", temporal="ever", name="Serial saw the restart"),
    agent.check.that(log, "contains", "reset=software", temporal="ever", name="the boot names its reset cause"),
    agent.check.that(log, "contains", "tuning loaded from flash kp=0.0020 ki=0.100", temporal="ever", name="the boot loads the shipped gains"),
])
results.extend(boot)
boot
```

## The step with the shipped gains

A step from 0 to 1500 rpm. The firmware measures the step response on the chip
and logs it from module `tune`. On a freshly started Serial, the `tune` signals
appear 2 s after its first line, hence the longer wait.

```python {#ac1115e2}
since = datetime.now(timezone.utc)
shell("motor target 1500")
time.sleep(4)

step = agent.query(f"{S}/tune.*", start=since)
results.append(agent.check.that(step[f"{S}/tune.overshoot"], ">", 30, name="the shipped gains overshoot"))
agent.query([f"{S}/motor.speed", f"{S}/motor.target"], start=since).short_names().plot()
```

## Tuned live

More proportional gain and less integral gain, then a step to 3000 rpm.

```python {#1e80b23c}
shell("pid kp 0.008")
shell("pid ki 0.05")
since = datetime.now(timezone.utc)
shell("motor target 3000")
time.sleep(1)

step = agent.query(f"{S}/tune.*", start=since)
results.extend([
    agent.check.that(step[f"{S}/tune.overshoot"], "<", 5, name="the tuned step overshoots under 5 %"),
    agent.check.that(step[f"{S}/tune.settle"], "<", 150, name="the tuned step settles within 150 ms"),
])
agent.query([f"{S}/motor.speed", f"{S}/motor.target"], start=since).short_names().plot()
```

## Saved across a reboot

`pid save` writes the gains to flash, and the next boot loads them. Serial
records them as the signals `config.kp`, `ki` and `kd`.

```python {#0dc82a6e}
shell("pid save")
since = datetime.now(timezone.utc)
shell("kernel reboot cold")
time.sleep(3)

log = agent.query(f"{S}/log.message", start=since)[f"{S}/log.message"]
results.append(agent.check.that(
    log, "contains", "tuning loaded from flash kp=0.0080 ki=0.050", temporal="ever", name="the boot loads the saved gains",
))
agent.query(f"{S}/config.*", start=since).short_names()
```

## A load step

A 0.1 N·m load at 1500 rpm. The speed dips, and the integral term brings it
back; the current shows the torque it takes.

```python {#4b18e3ca}
shell("motor target 1500")
time.sleep(1)
since = datetime.now(timezone.utc)
shell("motor load 0.1")
time.sleep(1)

load = agent.query([f"{S}/motor.speed", f"{S}/motor.current"], start=since)
results.extend([
    agent.check.that(load[f"{S}/motor.speed"], ">", 1100, name="the dip stays above 1100 rpm"),
    agent.check.that(agent.signal(f"{S}/motor.speed"), "is_close", 1500, abs_tol=15, name="the speed recovers to 1500 rpm"),
])
shell("motor load 0")
load.short_names().plot()
```

## A crash

An undefined instruction: a real CPU exception, the kind a corrupted function
pointer causes. Zephyr prints its fault dump, the board restarts, and the next
boot reports the crash it kept in RAM that survives the reset.

```python {#5cadf5cb}
since = datetime.now(timezone.utc)
shell("fault crash")
time.sleep(3)

crash = agent.query(f"{S}/log.*", start=since)
messages = crash[f"{S}/log.message"].to_list()
faulted = next(re.search(r"\(r15/pc\): (0x[0-9a-f]+)", m)[1] for m in messages if "r15/pc" in m)
recorded = next(re.search(r"pc=(0x[0-9a-f]+)", m)[1] for m in messages if "previous run crashed" in m)
results.append(agent.check.that(recorded, "==", faulted, name="the crash record names the faulting address"))
crash[crash[f"{S}/log.level"] == "error"].short_names()
```

## A watchdog reset

The control loop stops feeding the hardware watchdog, which resets the board
100 ms later.

```python {#bb93b58b}
since = datetime.now(timezone.utc)
shell("fault hang")
time.sleep(3)

log = agent.query(f"{S}/log.message", start=since)[f"{S}/log.message"]
watchdog = CheckResults([
    agent.check.that(log, "contains", "stopped feeding the watchdog", temporal="ever", name="the loop stopped feeding the watchdog"),
    agent.check.that(log, "contains", "reset=watchdog", temporal="ever", name="the boot blames the watchdog"),
])
results.extend(watchdog)
watchdog
```

## The board's own clock

Serial places each line at the board's uptime, so the 100 Hz telemetry lands
10 ms apart however USB delivered it. The median spacing leaves out the gap of
the watchdog reset above.

```python {#bd5871bd}
speed = agent.query(f"{S}/motor.speed", start="-5s")[f"{S}/motor.speed"]
spacing_ms = speed.index.to_series().diff().median().total_seconds() * 1000
results.append(agent.check.that(spacing_ms, "is_close", 10, abs_tol=0.5, name="samples 10 ms apart"))
results[-1]
```

## The chip

Once a second, module `sys` logs what the chip measures of itself: CPU load,
the control loop's worst execution time, the die temperature and VDDA.

```python {#78ea8589}
results.extend([
    agent.check.that(agent.signal(f"{S}/sys.cpu"), "<", 50, name="CPU load under 50 %"),
    agent.check.that(agent.signal(f"{S}/sys.loop_max"), "<", 100, name="control loop under 100 us"),
    agent.check.that(agent.signal(f"{S}/sys.vdda"), "is_close", 3.3, abs_tol=0.15, name="VDDA near 3.3 V"),
])
agent.latest(f"{S}/sys.*")
```

## The result

The board goes back as it ships, then every check above, and the gate.

```python {#8d48b627}
shell("pid defaults")
shell("pid save")
shell("motor stop")
results
```

```python {#48d540d9}
results.raise_if_failed()
print(f"all {len(results)} checks passed")
```
