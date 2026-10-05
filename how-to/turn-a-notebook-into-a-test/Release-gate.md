---
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]
params:
  cell_floor: 3
  current_max: 40
  window: -3m
---

Does every cell stay above its floor, and the pack current under its limit?

```python {#f378deb1}
from zelos_sdk import CheckResults, connect

agent = connect()
cells = agent.query("bus0/BMS_message/cells.*", start=params.window)
cells.short_names().plot(title="Cell voltages")
```

```python {#95973877}
current = agent.query("bus0/BMS_message/status.pack_current", start=params.window)
results = CheckResults(
    agent.check.that(series, ">", params.cell_floor)
    for series in cells.short_names().values()
)
pack = current.short_names()["pack_current"]
results.append(agent.check.that(pack, "<", params.current_max))
results
```

```python {#2jv410tx}
results.raise_if_failed()
```
