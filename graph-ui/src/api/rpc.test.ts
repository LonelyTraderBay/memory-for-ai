import { afterEach, describe, expect, it, vi } from "vitest";
import { callTool, RpcError } from "./rpc";

describe("callTool", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("sends a JSON-RPC request and unwraps MCP text content", async () => {
    const fetchMock = vi.fn(async () => ({
      ok: true,
      json: async () => ({
        result: { content: [{ text: JSON.stringify({ projects: [] }) }] },
      }),
    }));
    vi.stubGlobal("fetch", fetchMock);
    const controller = new AbortController();

    await expect(
      callTool("list_projects", {}, { signal: controller.signal }),
    ).resolves.toEqual({ projects: [] });

    expect(fetchMock).toHaveBeenCalledWith(
      "/rpc",
      expect.objectContaining({
        method: "POST",
        signal: controller.signal,
      }),
    );
  });

  it("reports malformed endpoint JSON as an RPC error", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => ({
      ok: true,
      json: async () => {
        throw new SyntaxError("bad json");
      },
    })));

    await expect(callTool("list_projects")).rejects.toMatchObject({
      code: -32700,
      message: "RPC endpoint returned invalid JSON",
    });
  });

  it("preserves JSON-RPC error codes", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => ({
      ok: true,
      json: async () => ({ error: { code: -32601, message: "not found" } }),
    })));

    const error = await callTool("missing").catch((value) => value);
    expect(error).toBeInstanceOf(RpcError);
    expect(error).toMatchObject({ code: -32601, message: "not found" });
  });
});
