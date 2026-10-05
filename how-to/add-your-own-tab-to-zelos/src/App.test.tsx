import { render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

const row = (source: string, message: string, signal: string, value: string) => ({
  source,
  message,
  signal,
  value,
});

vi.mock("@zeloscloud/app-extension-sdk", async (actual) => ({
  ...(await actual<typeof import("@zeloscloud/app-extension-sdk")>()),
  agents: { list: async () => ({ bench: true }) },
  query: {
    liveQueryLatestMulti: async () => [
      row("bus0", "BMS_message/cells", "cell_0", "3.68"),
      row("bus0", "BMS_message/cells", "cell_3", "2.91"),
      row("limits", "threshold", "value", "3"),
    ],
  },
}));

vi.mock("@zeloscloud/app-extension-sdk/react", () => ({
  useZelosBridge: () => ({ status: "ready", bridge: {} }),
}));

import { App } from "./App";

describe("App", () => {
  it("shows the weakest cell below the threshold", async () => {
    render(<App />);

    expect(await screen.findByText("cell_3 2.91 V")).toBeInTheDocument();
    expect(screen.getByText("Below the alarm threshold")).toBeInTheDocument();
  });
});
