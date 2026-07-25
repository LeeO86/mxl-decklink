// REST client for the mxl-decklink API (SPECIFICATION.md §7.5.6).
async function request(path, options = {}) {
  const headers = { ...(options.headers || {}) };
  if (options.body != null && !headers["Content-Type"] && !headers["content-type"]) {
    headers["Content-Type"] = "application/json";
  }
  const resp = await fetch(path, { ...options, headers });
  const ct = resp.headers.get("content-type") || "";
  if (ct.includes("application/json")) {
    // Server error bodies are also JSON (`{error:…}`); callers check that field.
    return resp.json();
  }
  const text = await resp.text();
  if (!resp.ok) {
    throw new Error(`${resp.status} ${resp.statusText}: ${text.slice(0, 200)}`);
  }
  throw new Error(`Expected JSON from ${path}, got ${ct || "unknown type"}`);
}

export const api = {
  get: (path) => request(path),
  put: (path, body) => request(path, { method: "PUT", body: JSON.stringify(body) }),
  post: (path, body) => request(path, { method: "POST", body: JSON.stringify(body) }),
};

export const chKey = (idx, suffix) => `CH${idx}_${suffix}`;

export function channelIndices(config) {
  const idx = new Set();
  for (const key of Object.keys(config.values)) {
    const m = key.match(/^CH(\d+)_DIRECTION$/);
    if (m) idx.add(Number(m[1]));
  }
  return [...idx].sort((a, b) => a - b);
}

export const valueOf = (config, key) => config.values[key]?.value ?? null;
export const sourceOf = (config, key) => config.values[key]?.source ?? "default";
export const channelMeta = (config, suffix) =>
  config.schema.find((m) => m.kind === "channel" && m.key === suffix);

// Derives the §3.2.1 mode name from flow geometry for flow→output assignment.
export function modeNameForFlow(flow, options) {
  if (!flow.width || !flow.rate_numerator) return null;
  const fps =
    flow.rate_denominator && flow.rate_denominator !== 1
      ? { 24000: "2398", 30000: "2997", 60000: "5994" }[flow.rate_numerator]
      : String(flow.rate_numerator);
  if (!fps) return null;
  let name = null;
  if (flow.height === 720) name = `HD720p${fps}`;
  else if (flow.height === 1080) name = flow.interlaced ? `HD1080i${fps}` : `HD1080p${fps}`;
  else if (flow.height === 2160) name = `4K2160p${fps}`;
  else if (flow.height === 4320) name = `8K4320p${fps}`;
  return options.includes(name) ? name : null;
}

export const fmtRate = (n, d) =>
  n === null || n === undefined ? "" : `${d && d !== 1 ? (n / d).toFixed(2) : n} fps`;
