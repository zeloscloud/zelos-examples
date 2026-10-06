# The shared bench script

A stand-in bench supply that serves actions and streams rail voltages. Used by [Run actions on your bench from the app](https://zeloscloud.io/blog/send-commands-from-the-app), [Write your own action in Python](https://zeloscloud.io/blog/write-your-own-action-in-python), [Build your own tab with live readouts and an action button](https://zeloscloud.io/blog/add-your-own-tab-to-zelos), [Pick the right panel for each signal](https://zeloscloud.io/blog/pick-the-right-panel).

Install [uv](https://docs.astral.sh/uv/), start the Zelos app, then:

```bash
uv run --with zelos-sdk bench.py
```
