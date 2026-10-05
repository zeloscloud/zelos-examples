import type { BridgeTransport } from "@zeloscloud/app-extension-sdk";
import { actions, agents, query, signalPath } from "@zeloscloud/app-extension-sdk";
import { useZelosBridge } from "@zeloscloud/app-extension-sdk/react";
import { useEffect, useState } from "react";

const CELLS = [0, 1, 2, 3, 4, 5, 6, 7].map((i) => `bus0/BMS_message/cells.cell_${i}`);
const THRESHOLD = "limits/threshold.value";
const PRESETS = [3.0, 3.2, 3.4];

type Pack = { agent: string; values: Record<string, number> };

async function readPack(bridge: BridgeTransport): Promise<Pack> {
  const live = await agents.list(bridge);
  const agent = Object.keys(live).find((a) => live[a]);
  if (!agent) throw new Error("No agent connected");

  const end = new Date();
  const start = new Date(end.getTime() - 5_000);
  const rows = await query.liveQueryLatestMulti(bridge, {
    agentSignals: { [agent]: [...CELLS, THRESHOLD] },
    start: start.toISOString(),
    end: end.toISOString(),
  });
  return {
    agent,
    values: Object.fromEntries(rows.map((row) => [signalPath(row), Number(row.value)])),
  };
}

export function App() {
  const { status, bridge } = useZelosBridge();
  const [pack, setPack] = useState<Pack>();
  const [error, setError] = useState("");
  const [note, setNote] = useState("");

  useEffect(() => {
    if (!bridge) return;
    const poll = () =>
      readPack(bridge).then(
        (next) => {
          setPack(next);
          setError("");
        },
        (failure: Error) => setError(failure.message),
      );
    poll();
    const timer = setInterval(poll, 500);
    return () => clearInterval(timer);
  }, [bridge]);

  async function setThreshold(volts: number) {
    if (!bridge || !pack) return;
    const res = await actions.execute(bridge, {
      agent: pack.agent,
      action: "bench/set_threshold",
      params: { volts },
    });
    setNote(`Set Threshold: ${res.status}, ${volts.toFixed(1)} V`);
  }

  if (status !== "ready") {
    return <p className="p-8 text-sm text-muted-foreground">Connecting to Zelos…</p>;
  }

  const values = pack?.values ?? {};
  const limit = values[THRESHOLD];
  const cells = CELLS.filter((path) => path in values).map((path) => ({
    name: path.slice(path.lastIndexOf(".") + 1),
    volts: values[path] ?? 0,
  }));
  const weakest = cells.reduce<(typeof cells)[number] | undefined>(
    (a, c) => (!a || c.volts < a.volts ? c : a),
    undefined,
  );
  const low = (volts: number) => limit !== undefined && volts < limit;
  const alarm = weakest !== undefined && low(weakest.volts);
  const red = (on: boolean) => (on ? "text-destructive" : "");

  return (
    <div className="flex max-w-2xl flex-col gap-5 p-6">
      <div className={`flex flex-col gap-2 rounded-xl border bg-card p-6 ${red(alarm)}`}>
        <p className="text-base text-muted-foreground">Weakest cell</p>
        <p className="text-5xl font-semibold tabular-nums">
          {weakest ? `${weakest.name} ${weakest.volts.toFixed(2)} V` : "No data"}
        </p>
        <p className="text-sm text-muted-foreground">
          {alarm ? "Below the alarm threshold" : "Every cell is above the alarm threshold"}
        </p>
      </div>

      <div className="grid grid-cols-4 gap-3">
        {cells.map((cell) => (
          <div key={cell.name} className={`rounded-lg border bg-card p-3 ${red(low(cell.volts))}`}>
            <p className="text-sm text-muted-foreground">{cell.name}</p>
            <p className="text-2xl tabular-nums">{cell.volts.toFixed(2)} V</p>
          </div>
        ))}
      </div>

      <div className="flex items-center gap-3 rounded-xl border bg-card p-4">
        <p className="flex-1 text-base">Alarm threshold {limit?.toFixed(2) ?? "–"} V</p>
        {PRESETS.map((volts) => (
          <button
            key={volts}
            type="button"
            onClick={() => setThreshold(volts).catch((failure: Error) => setNote(failure.message))}
            className={`rounded-md border px-3 py-1.5 text-base ${limit === volts ? "bg-primary text-primary-foreground" : "hover:bg-accent"}`}
          >
            {volts.toFixed(1)} V
          </button>
        ))}
      </div>

      <p className="text-sm text-muted-foreground">{error || note}</p>
    </div>
  );
}
