---
description: The pack and the converter report physically sensible values.
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]
- tzdata
params:
  window: -3m
  grid: 1s
  cell_spread_limit_mv: 120
  temperature_limit_c: 85
  rail_nominal_v: 13.8
  rail_tolerance_v: 1.0
---

Liveness says a node is talking. This asks whether what it says makes sense: a
state of charge inside its range, cells within a plausible spread of each other,
a converter that is not implausibly hot, and a low-voltage rail holding near its
setpoint.

These are the checks that catch a scaling mistake in a DBC or a unit error in
firmware, which are the two most common ways CAN data goes quietly wrong. The
bounds are deliberately generous — this is a sanity test, not a specification.

```python {#aa0cb934}
import pandas as pd
from zelos_sdk import CheckResults, connect

agent = connect()
results = CheckResults()


def sampled(paths, grid=params.grid):
    """Signals on a regular grid, labelled by signal name rather than full path.

    These signals are slow next to their message rate, so a resampled grid says
    the same thing in far fewer points.
    """
    frame = agent.query(paths, start=params.window).ffill().dropna()
    return frame.short_names().resample(grid, "mean")
```

## The drive cycle

The VCU walks the same cycle on every run: stand still, drive, charge. Pack
current is the clearest view of it — positive under load, negative while
charging.

```python {#c8605b7e}
sampled(["*0200_BMS_Status.PackCurrent"]).plot()
```

```python {#82ac8839}
soc = sampled(["*0200_BMS_Status.StateOfCharge"])
soc.plot()
```

```python {#c0268f69}
results += [
    agent.check.that(soc["StateOfCharge"], ">=", 0.0),
    agent.check.that(soc["StateOfCharge"], "<=", 100.0),
]
```

## The pack

Four cells, one of them the weakest. A real pack has one, and a spread that
grows is how you find it.

```python {#023b7f14}
cells = sampled([f"*0202_BMS_CellVoltages.CellVoltage{n}" for n in (1, 2, 3, 4)])
cells.plot()
```

```python {#d9ce176a}
measured = cells.to_pandas()
spread_mv = (measured.max(axis=1) - measured.min(axis=1)) * 1000.0

pd.DataFrame(
    {"min_v": measured.min(), "mean_v": measured.mean().round(4), "max_v": measured.max()}
)
```

The spread is the widest gap across all four cells at one instant, so it is a
number this notebook derives rather than a signal the pack sends.

```python {#f652c7b5}
results.append(
    agent.check.that(
        round(float(spread_mv.max()), 1), "<=", float(params.cell_spread_limit_mv),
        name=f"0202 CellVoltage spread stays under {params.cell_spread_limit_mv} mV",
    )
)
```

## The converter

The DC-DC warms up with the current it passes, and holds the low-voltage rail
near its setpoint while it does.

```python {#3388199a}
temperature = sampled(["*0300_DCDC_Status.Temperature"])
temperature.plot()
```

```python {#6c082a21}
rail = sampled(["*0300_DCDC_Status.OutputVoltage"])
deviation = (rail["OutputVoltage"] - params.rail_nominal_v).rename("deviation from setpoint")
deviation.plot()
```

```python {#94d4c35f}
results += [
    agent.check.that(temperature["Temperature"], "<=", params.temperature_limit_c),
    agent.check.that(deviation.abs(), "<=", params.rail_tolerance_v),
]
results.raise_if_failed()
```
