# Zelos examples

Runnable benches for working with embedded systems data. Each one runs in Docker
and is checked in CI.

| Bench | What it is |
|---|---|
| [`can-bench`](can-bench/) | Three emulated microcontrollers running Zephyr firmware, talking CAN on a virtual bus, decoded live. Ships two builds of one node so a fault can be found in the data and then fixed. |

Each directory is self-contained. Nothing is shared between them until a second
bench needs it.

## Licence

MIT. See [LICENSE](LICENSE).
