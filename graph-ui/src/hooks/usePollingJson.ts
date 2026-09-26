import { useCallback, useEffect, useRef, useState } from "react";

/** One request per mounted endpoint; failed refreshes retain the last snapshot. */
export function usePollingJson<T>(url: string, intervalMs: number) {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<string | null>(null);
  const active = useRef<AbortController | null>(null);

  const refresh = useCallback(async () => {
    if (active.current) return;
    const controller = new AbortController();
    active.current = controller;
    const timeout = setTimeout(() => controller.abort(), 15_000);
    try {
      const response = await fetch(url, { signal: controller.signal });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const next: T = await response.json();
      if (active.current === controller) {
        setData(next);
        setError(null);
      }
    } catch (cause) {
      if (active.current === controller) {
        setError(controller.signal.aborted ? "Request timed out" :
          cause instanceof Error ? cause.message : "Refresh failed");
      }
    } finally {
      clearTimeout(timeout);
      if (active.current === controller) active.current = null;
    }
  }, [url]);

  useEffect(() => {
    setData(null);
    setError(null);
    void refresh();
    const timer = setInterval(() => void refresh(), intervalMs);
    return () => {
      clearInterval(timer);
      active.current?.abort();
      active.current = null;
    };
  }, [refresh, intervalMs]);

  return { data, error, refresh };
}
