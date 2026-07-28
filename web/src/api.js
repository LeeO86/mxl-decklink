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

export const NIL_UUID = "00000000-0000-0000-0000-000000000000";

export const chKey = (idx, suffix) => `CH${idx}_${suffix}`;
export const afKey = (chIdx, afIdx, field) => `CH${chIdx}_AF${afIdx}_${field}`;

export function newUuid() {
  if (typeof crypto !== "undefined" && crypto.randomUUID) return crypto.randomUUID();
  // Fallback for older browsers / non-secure contexts.
  return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, (c) => {
    const r = (Math.random() * 16) | 0;
    const v = c === "x" ? r : (r & 0x3) | 0x8;
    return v.toString(16);
  });
}

/** Contiguous AF indices present for a channel (draft or config values). */
export function audioFlowIndices(draftOrValues, chIdx) {
  const keys = draftOrValues && typeof draftOrValues === "object"
    ? (draftOrValues.values ? Object.keys(draftOrValues.values) : Object.keys(draftOrValues))
    : [];
  const set = new Set();
  const re = new RegExp(`^CH${chIdx}_AF(\\d+)_FLOW_ID$`);
  for (const k of keys) {
    const m = k.match(re);
    if (m) set.add(Number(m[1]));
  }
  // Contiguous from 0.
  const out = [];
  for (let i = 0; i < 16; ++i) {
    if (!set.has(i)) break;
    out.push(i);
  }
  return out;
}

export function identityMap(count) {
  return [...Array(Math.max(0, count)).keys()].join(",");
}

/** Grouphint Studio-A + channel index 0 → Studio-A-1 */
export function suggestChannelLabel(groupHint, chIdx) {
  const hint = (groupHint || "").trim();
  if (!hint) return `ch${chIdx}`;
  return `${hint}-${chIdx + 1}`;
}

export function suggestVideoFlowLabel(channelLabel) {
  return `${channelLabel}-video1`;
}

export function suggestAudioFlowLabel(channelLabel, afIdx) {
  return `${channelLabel}-audio${afIdx + 1}`;
}

export function suggestAncFlowLabel(channelLabel) {
  return `${channelLabel}-anc1`;
}

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
export const audioFlowMeta = (config, field) =>
  config.schema.find((m) => m.kind === "audio_flow" && m.key === field);

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
