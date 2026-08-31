import { renderHook, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { callTool } from "../api/rpc";
import { useProjects } from "./useProjects";

vi.mock("../api/rpc", () => ({
  callTool: vi.fn(),
}));

const callToolMock = vi.mocked(callTool);

describe("useProjects", () => {
  beforeEach(() => {
    callToolMock.mockReset();
  });

  it("loads project summaries and schema counts in one request", async () => {
    callToolMock.mockResolvedValue({
      projects: [
        {
          name: "demo",
          root_path: "/tmp/demo",
          indexed_at: "2026-08-31T00:00:00Z",
          schema: {
            node_labels: [{ label: "Function", count: 4 }],
            edge_types: [{ type: "CALLS", count: 3 }],
            total_nodes: 4,
            total_edges: 3,
          },
        },
      ],
    });

    const { result } = renderHook(() => useProjects());

    await waitFor(() => expect(result.current.loading).toBe(false));

    expect(callToolMock).toHaveBeenCalledTimes(1);
    expect(callToolMock).toHaveBeenCalledWith(
      "list_projects",
      { include_details: true },
      { signal: expect.any(AbortSignal) },
    );
    expect(result.current.projects[0]?.schema?.node_labels[0]).toEqual({
      label: "Function",
      count: 4,
    });
  });
});
