from zelos_sdk import connect

with connect() as agent:
    result = agent.actions.execute("bench/read_voltage", {"rail": "5v0"})
    print(result.ok, result.value)
