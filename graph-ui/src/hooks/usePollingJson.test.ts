import { act, cleanup, renderHook } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { usePollingJson } from "./usePollingJson";

function deferred() {
  let resolve!: (value: Response) => void;
  const promise = new Promise<Response>((done) => { resolve = done; });
  return { promise, resolve };
}
const json = (value: unknown) => new Response(JSON.stringify(value));

describe("usePollingJson", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => { cleanup(); vi.useRealTimers(); vi.unstubAllGlobals(); });

  it("does not overlap slow polls or rapid manual refreshes", async () => {
    const pending = deferred();
    const fetcher = vi.fn().mockReturnValueOnce(pending.promise).mockResolvedValue(json({ value: 2 }));
    vi.stubGlobal("fetch", fetcher);
    const { result } = renderHook(() => usePollingJson("/metrics", 1000));
    await act(async () => {
      vi.advanceTimersByTime(5000);
      void result.current.refresh();
      void result.current.refresh();
    });
    expect(fetcher).toHaveBeenCalledTimes(1);
    await act(async () => pending.resolve(json({ value: 1 })));
    expect(result.current.data).toEqual({ value: 1 });
    await act(async () => vi.advanceTimersByTime(1000));
    expect(fetcher).toHaveBeenCalledTimes(2);
  });

  it("aborts the old endpoint and ignores its late response", async () => {
    const old = deferred();
    const fetcher = vi.fn().mockReturnValueOnce(old.promise).mockResolvedValueOnce(json({ value: 2 }));
    vi.stubGlobal("fetch", fetcher);
    const { result, rerender } = renderHook(({ url }) => usePollingJson(url, 1000), {
      initialProps: { url: "/old" },
    });
    await act(async () => rerender({ url: "/new" }));
    expect(fetcher.mock.calls[0][1].signal.aborted).toBe(true);
    await act(async () => old.resolve(json({ value: 1 })));
    expect(result.current.data).toEqual({ value: 2 });
  });

  it("reports HTTP failures while retaining data and recovers on refresh", async () => {
    const fetcher = vi.fn().mockResolvedValueOnce(json({ value: 1 }))
      .mockResolvedValueOnce(new Response("unavailable", { status: 503 }))
      .mockResolvedValueOnce(json({ value: 3 }));
    vi.stubGlobal("fetch", fetcher);
    const { result } = renderHook(() => usePollingJson("/metrics", 1000));
    await act(async () => {});
    await act(async () => result.current.refresh());
    expect(result.current.data).toEqual({ value: 1 });
    expect(result.current.error).toBe("HTTP 503");
    await act(async () => result.current.refresh());
    expect(result.current.data).toEqual({ value: 3 });
    expect(result.current.error).toBeNull();
  });

  it("times out a hung request and allows the next refresh", async () => {
    const fetcher = vi.fn().mockImplementationOnce((_url, { signal }) => new Promise((_resolve, reject) => {
      signal.addEventListener("abort", () => reject(new DOMException("Aborted", "AbortError")));
    })).mockResolvedValueOnce(json({ value: 2 }));
    vi.stubGlobal("fetch", fetcher);
    const { result } = renderHook(() => usePollingJson("/metrics", 1000));
    await act(async () => vi.advanceTimersByTime(15_000));
    expect(result.current.error).toBe("Request timed out");
    await act(async () => result.current.refresh());
    expect(result.current.data).toEqual({ value: 2 });
    expect(result.current.error).toBeNull();
  });

  it("cancels requests and timers on unmount", async () => {
    const pending = deferred();
    const fetcher = vi.fn().mockReturnValue(pending.promise);
    vi.stubGlobal("fetch", fetcher);
    const { unmount } = renderHook(() => usePollingJson("/logs", 1000));
    unmount();
    expect(fetcher.mock.calls[0][1].signal.aborted).toBe(true);
    await act(async () => {
      pending.resolve(json({ value: 1 }));
      vi.advanceTimersByTime(5000);
    });
    expect(fetcher).toHaveBeenCalledTimes(1);
  });
});
