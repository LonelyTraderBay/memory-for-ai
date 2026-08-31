/* JSON-RPC client — speaks the same protocol as MCP clients via POST /rpc */

let _nextId = 1;

export class RpcError extends Error {
  constructor(
    public code: number,
    message: string,
  ) {
    super(message);
    this.name = "RpcError";
  }
}

export interface RpcOptions {
  signal?: AbortSignal;
}

export async function callTool<T = unknown>(
  name: string,
  args: Record<string, unknown> = {},
  options: RpcOptions = {},
): Promise<T> {
  const res = await fetch("/rpc", {
    method: "POST",
    headers: {
      "Accept": "application/json",
      "Content-Type": "application/json",
    },
    signal: options.signal,
    body: JSON.stringify({
      jsonrpc: "2.0",
      id: _nextId++,
      method: "tools/call",
      params: { name, arguments: args },
    }),
  });

  if (!res.ok) {
    throw new RpcError(-1, `HTTP ${res.status}: ${res.statusText}`);
  }

  let json: unknown;
  try {
    json = await res.json();
  } catch {
    throw new RpcError(-32700, "RPC endpoint returned invalid JSON");
  }

  if (!json || typeof json !== "object") {
    throw new RpcError(-32603, "RPC endpoint returned an invalid response");
  }

  const envelope = json as {
    error?: { code?: number; message?: string };
    result?: { content?: Array<{ text?: string }> };
  };

  if (envelope.error) {
    throw new RpcError(
      envelope.error.code ?? -1,
      envelope.error.message ?? "unknown",
    );
  }

  /* MCP tool results are wrapped: { result: { content: [{ text: "..." }] } } */
  const text = envelope.result?.content?.[0]?.text;
  if (text === undefined) {
    return envelope.result as T;
  }

  try {
    return JSON.parse(text) as T;
  } catch {
    throw new RpcError(-32603, "MCP tool returned invalid JSON content");
  }
}
