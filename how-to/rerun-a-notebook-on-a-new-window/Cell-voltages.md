---
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]
params:
  window: -10m
---

```python {#b045a676}
from zelos_sdk import connect

agent = connect()
agent
```

Do all eight cells hold their voltage through a discharge cycle?

```python {#0p536hpm}
cells = agent.query("bus0/BMS_message/cells.*", start=params.window)
cells
```

```python {#qjc178ok}
cells.short_names().plot(title=f"Cell voltages, {params.window}")
```

```python {#h2kyhjnd}
weak = cells["bus0/BMS_message/cells.cell_3"]
(weak < 3.0).sum()
```
