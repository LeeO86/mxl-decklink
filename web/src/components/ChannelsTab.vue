<script setup>
import { computed, onMounted, reactive, ref } from "vue";
import {
  NIL_UUID,
  afKey,
  api,
  audioFlowIndices,
  audioFlowMeta,
  chKey,
  channelIndices,
  channelMeta,
  identityMap,
  newUuid,
  sourceOf,
  suggestAncFlowLabel,
  suggestAudioFlowLabel,
  suggestChannelLabel,
  suggestVideoFlowLabel,
  valueOf,
} from "../api.js";
import SettingField from "./SettingField.vue";

const CH_FIELD_ORDER = [
  "DIRECTION", "SUBDEVICE_INDEX", "VIDEO_MODE", "PIXEL_FORMAT", "MXL_VIDEO_FLOW_ID",
  "AUDIO_ENABLE", "AUDIO_CHANNEL_COUNT", "AUDIO_SAMPLE_TYPE", "VIDEO_ANC_ENABLE", "MXL_ANC_FLOW_ID", "LABEL",
  "MXL_VIDEO_FLOW_LABEL", "MXL_GROUP_HINT", "MXL_DEVICE_ID", "MXL_SOURCE_ID",
  "ALLOW_FORMAT_CONVERSION", "GRAIN_COUNT", "AUDIO_BUFFER_MS", "COMMIT_BATCH_HINT", "OUTPUT_PREROLL_GRAINS", "READER_TIMEOUT_MS",
];

const AF_FIELDS = ["FLOW_ID", "CHANNEL_COUNT", "MAP", "LABEL"];

const config = ref(null);
const error = ref("");
const drafts = reactive({}); // idx -> { key -> string }
const messages = reactive({}); // idx -> { kind, text }
const extraPanels = ref([]); // newly added channel indices not yet in config
/** Last auto-suggested values per channel — only overwrite fields that still match. */
const lastSuggest = reactive({}); // idx -> { label, video, anc, audio: {afIdx: label} }

function afCount(idx) {
  const d = drafts[idx] || {};
  let n = 0;
  for (let af = 0; af < 16; ++af) {
    if ((d[afKey(idx, af, "FLOW_ID")] ?? "").trim() !== "" || (d[afKey(idx, af, "CHANNEL_COUNT")] ?? "").trim() !== "") {
      n = af + 1;
    } else {
      break;
    }
  }
  return n;
}

function initDraft(cfg, idx) {
  const d = {};
  for (const suffix of CH_FIELD_ORDER) {
    const key = chKey(idx, suffix);
    d[key] = valueOf(cfg, key) ?? "";
  }
  // Load contiguous audio flows from config values.
  const fromCfg = [];
  for (let af = 0; af < 16; ++af) {
    const id = valueOf(cfg, afKey(idx, af, "FLOW_ID"));
    if (id == null && !valueOf(cfg, afKey(idx, af, "CHANNEL_COUNT"))) break;
    fromCfg.push(af);
  }
  // Also scan raw keys in case only MAP/LABEL present (shouldn't happen).
  const scanned = audioFlowIndices(cfg, idx);
  const afs = fromCfg.length ? fromCfg : scanned;
  for (const af of afs) {
    for (const field of AF_FIELDS) {
      const key = afKey(idx, af, field);
      d[key] = valueOf(cfg, key) ?? "";
    }
  }
  drafts[idx] = d;
  seedSuggestBaseline(idx);
}

function seedSuggestBaseline(idx) {
  const d = drafts[idx];
  const label = (d[chKey(idx, "LABEL")] || "").trim();
  const audio = {};
  for (let af = 0; af < afCount(idx); ++af) {
    audio[af] = (d[afKey(idx, af, "LABEL")] || "").trim();
  }
  lastSuggest[idx] = {
    label,
    video: (d[chKey(idx, "MXL_VIDEO_FLOW_LABEL")] || "").trim(),
    anc: "", // no anc label key; reserved
    audio,
  };
}

/** Refresh auto labels when grouphint / channel label / AF count changes. */
function refreshSuggestions(idx, { forceLabel = false } = {}) {
  const d = drafts[idx];
  if (!d) return;
  const hint = (d[chKey(idx, "MXL_GROUP_HINT")] || "").trim();
  const prev = lastSuggest[idx] || { label: "", video: "", audio: {} };
  const nextLabel = suggestChannelLabel(hint, idx);
  const curLabel = (d[chKey(idx, "LABEL")] || "").trim();
  if (forceLabel || curLabel === "" || curLabel === prev.label) {
    d[chKey(idx, "LABEL")] = nextLabel;
  }
  const effectiveLabel = (d[chKey(idx, "LABEL")] || "").trim() || nextLabel;

  const nextVideo = suggestVideoFlowLabel(effectiveLabel);
  const curVideo = (d[chKey(idx, "MXL_VIDEO_FLOW_LABEL")] || "").trim();
  if (curVideo === "" || curVideo === prev.video) {
    d[chKey(idx, "MXL_VIDEO_FLOW_LABEL")] = nextVideo;
  }

  const nextAudio = {};
  for (let af = 0; af < afCount(idx); ++af) {
    const sug = suggestAudioFlowLabel(effectiveLabel, af);
    const cur = (d[afKey(idx, af, "LABEL")] || "").trim();
    if (cur === "" || cur === (prev.audio?.[af] ?? "")) {
      d[afKey(idx, af, "LABEL")] = sug;
    }
    nextAudio[af] = (d[afKey(idx, af, "LABEL")] || "").trim();
  }

  lastSuggest[idx] = {
    label: (d[chKey(idx, "LABEL")] || "").trim(),
    video: (d[chKey(idx, "MXL_VIDEO_FLOW_LABEL")] || "").trim(),
    anc: suggestAncFlowLabel(effectiveLabel),
    audio: nextAudio,
  };
}

function applyDirectionDefaults(idx, direction) {
  const d = drafts[idx];
  if (!d) return;
  const isInput = direction === "input";
  if (isInput) {
    if (!(d[chKey(idx, "MXL_VIDEO_FLOW_ID")] || "").trim() || d[chKey(idx, "MXL_VIDEO_FLOW_ID")] === NIL_UUID) {
      d[chKey(idx, "MXL_VIDEO_FLOW_ID")] = newUuid();
    }
    if ((d[chKey(idx, "VIDEO_MODE")] || "") === "") d[chKey(idx, "VIDEO_MODE")] = "auto";
  } else {
    // Outputs start unassigned (nil UUID) until a flow is picked.
    if (!(d[chKey(idx, "MXL_VIDEO_FLOW_ID")] || "").trim()) {
      d[chKey(idx, "MXL_VIDEO_FLOW_ID")] = NIL_UUID;
    }
    if ((d[chKey(idx, "VIDEO_MODE")] || "") === "" || d[chKey(idx, "VIDEO_MODE")] === "auto") {
      d[chKey(idx, "VIDEO_MODE")] = "HD1080p50";
    }
  }
  // Ensure at least AF0 when audio enabled.
  ensureDefaultAudioFlow(idx, isInput);
  refreshSuggestions(idx, { forceLabel: true });
}

function ensureDefaultAudioFlow(idx, isInput) {
  const d = drafts[idx];
  const enabled = (d[chKey(idx, "AUDIO_ENABLE")] || "true") !== "false";
  if (!enabled) return;
  if (afCount(idx) > 0) {
    // Fill missing UUIDs on existing slots.
    for (let af = 0; af < afCount(idx); ++af) {
      const idKey = afKey(idx, af, "FLOW_ID");
      if (!(d[idKey] || "").trim()) {
        d[idKey] = isInput ? newUuid() : NIL_UUID;
      }
      if (!(d[afKey(idx, af, "CHANNEL_COUNT")] || "").trim()) {
        d[afKey(idx, af, "CHANNEL_COUNT")] = "2";
      }
      if (!(d[afKey(idx, af, "MAP")] || "").trim()) {
        d[afKey(idx, af, "MAP")] = identityMap(Number(d[afKey(idx, af, "CHANNEL_COUNT")]) || 2);
      }
    }
    return;
  }
  d[afKey(idx, 0, "FLOW_ID")] = isInput ? newUuid() : NIL_UUID;
  d[afKey(idx, 0, "CHANNEL_COUNT")] = "2";
  d[afKey(idx, 0, "MAP")] = "0,1";
  d[afKey(idx, 0, "LABEL")] = "";
}

function addAudioFlow(idx) {
  const d = drafts[idx];
  const af = afCount(idx);
  if (af >= 16) return;
  const isInput = (d[chKey(idx, "DIRECTION")] || "input") === "input";
  const deckWidth = Number(d[chKey(idx, "AUDIO_CHANNEL_COUNT")] || 16);
  // Default map: next unused contiguous pair starting at 2*af, clamped.
  const start = Math.min(af * 2, Math.max(0, deckWidth - 1));
  const count = Math.min(2, deckWidth - start) || 1;
  const map = [...Array(count).keys()].map((i) => start + i).join(",");
  d[afKey(idx, af, "FLOW_ID")] = isInput ? newUuid() : NIL_UUID;
  d[afKey(idx, af, "CHANNEL_COUNT")] = String(count);
  d[afKey(idx, af, "MAP")] = map;
  d[afKey(idx, af, "LABEL")] = "";
  refreshSuggestions(idx);
}

function removeAudioFlow(idx, af) {
  const d = drafts[idx];
  const n = afCount(idx);
  // Shift later slots down to keep contiguous.
  for (let i = af; i < n - 1; ++i) {
    for (const field of AF_FIELDS) {
      d[afKey(idx, i, field)] = d[afKey(idx, i + 1, field)];
    }
  }
  for (const field of AF_FIELDS) {
    delete d[afKey(idx, n - 1, field)];
  }
  refreshSuggestions(idx);
}

function onChannelCountChange(idx, af) {
  const d = drafts[idx];
  const count = Number(d[afKey(idx, af, "CHANNEL_COUNT")] || 0);
  if (count < 1 || count > 64) return;
  const mapKey = afKey(idx, af, "MAP");
  const parts = (d[mapKey] || "").split(",").map((s) => s.trim()).filter(Boolean);
  if (parts.length === count) return;
  // Grow/shrink preserving prefix; fill with sequential indices.
  const deckWidth = Number(d[chKey(idx, "AUDIO_CHANNEL_COUNT")] || 16);
  while (parts.length < count) {
    const next = parts.length ? Number(parts[parts.length - 1]) + 1 : 0;
    parts.push(String(Math.min(next, deckWidth - 1)));
  }
  d[mapKey] = parts.slice(0, count).join(",");
}

async function load() {
  try {
    config.value = await api.get("/api/config");
    error.value = "";
    extraPanels.value = [];
    for (const idx of channelIndices(config.value)) initDraft(config.value, idx);
  } catch (e) {
    error.value = String(e);
  }
}

function allKeys(idx) {
  const keys = CH_FIELD_ORDER.map((sfx) => chKey(idx, sfx));
  for (let af = 0; af < afCount(idx); ++af) {
    for (const field of AF_FIELDS) keys.push(afKey(idx, af, field));
  }
  return keys;
}

function anyEnv(idx) {
  return allKeys(idx).some((k) => sourceOf(config.value, k) === "env");
}

async function save(idx, isNew) {
  const cfg = config.value;
  const set = {};
  const unset = [];
  for (const key of allKeys(idx)) {
    if (sourceOf(cfg, key) === "env") continue;
    const v = (drafts[idx][key] ?? "").trim();
    if (v === "") {
      if (sourceOf(cfg, key) === "file") unset.push(key);
    } else {
      set[key] = v;
    }
  }
  // Drop AF slots that were removed: unset any leftover file keys beyond afCount.
  for (let af = afCount(idx); af < 16; ++af) {
    for (const field of AF_FIELDS) {
      const key = afKey(idx, af, field);
      if (sourceOf(cfg, key) === "file") unset.push(key);
    }
  }
  const res = await api.put("/api/config", { set, unset });
  if (res.error) {
    messages[idx] = { kind: "err", text: res.error };
  } else {
    const parts = ["Saved."];
    if (res.channels_restarted?.length) parts.push(` Restarted: ${res.channels_restarted.join(", ")}.`);
    if (res.channels_added?.length) parts.push(` Added: ${res.channels_added.join(", ")}.`);
    if (res.restart_required) parts.push(" Container restart required for global changes.");
    messages[idx] = { kind: "ok", text: parts.join("") };
    setTimeout(load, isNew ? 500 : 900);
  }
}

async function remove(idx) {
  if (!confirm(`Remove channel ${idx}?`)) return;
  const unset = [];
  for (const suffix of CH_FIELD_ORDER) {
    const key = chKey(idx, suffix);
    if (sourceOf(config.value, key) === "file") unset.push(key);
  }
  // Unset every possible AF slot once (contiguous discovery stops at gaps,
  // but file may still hold higher indices from earlier edits).
  for (let af = 0; af < 16; ++af) {
    for (const field of AF_FIELDS) {
      const key = afKey(idx, af, field);
      if (sourceOf(config.value, key) === "file") unset.push(key);
    }
  }
  const res = await api.put("/api/config", { set: {}, unset });
  if (res.error) messages[idx] = { kind: "err", text: res.error };
  else setTimeout(load, 500);
}

function addChannel() {
  const indices = new Set([...channelIndices(config.value), ...extraPanels.value]);
  const freeIdx = [...Array(16).keys()].find((i) => !indices.has(i));
  if (freeIdx === undefined) return;
  initDraft(config.value, freeIdx);
  drafts[freeIdx][chKey(freeIdx, "DIRECTION")] = "input";
  drafts[freeIdx][chKey(freeIdx, "SUBDEVICE_INDEX")] = "0";
  drafts[freeIdx][chKey(freeIdx, "AUDIO_ENABLE")] = "true";
  drafts[freeIdx][chKey(freeIdx, "AUDIO_CHANNEL_COUNT")] = "16";
  applyDirectionDefaults(freeIdx, "input");
  extraPanels.value = [...extraPanels.value, freeIdx];
}

function onDirectionChange(idx) {
  applyDirectionDefaults(idx, drafts[idx][chKey(idx, "DIRECTION")] || "input");
}

function onGroupHintChange(idx) {
  refreshSuggestions(idx);
}

function onLabelChange(idx) {
  // When the user edits the channel label, refresh flow labels that still match.
  refreshSuggestions(idx);
}

const panelIndices = computed(() =>
  config.value ? [...channelIndices(config.value), ...extraPanels.value] : []
);

onMounted(load);
</script>

<template>
  <div v-if="error" class="panel">API error: {{ error }}</div>
  <template v-else-if="config">
    <div v-if="!config.file" class="panel" style="margin-bottom:.9rem">
      No configuration file is mounted (<code>MXL_CONFIG_FILE</code> is unset) — settings are read-only.
      Mount a config volume and set <code>MXL_CONFIG_FILE</code> to enable editing.
    </div>
    <div
      v-for="idx in panelIndices"
      :key="idx"
      class="panel"
      style="margin-bottom:.9rem"
    >
      <h3>
        Channel {{ idx }}{{ extraPanels.includes(idx) ? " (new)" : "" }}
        — {{ drafts[idx]?.[chKey(idx, "LABEL")] || valueOf(config, chKey(idx, "LABEL")) || ("ch" + idx) }}
      </h3>
      <div v-if="anyEnv(idx)" class="muted" style="font-size:.75rem;margin-bottom:.4rem">
        Some settings are fixed by environment variables and cannot be changed here.
      </div>
      <div class="grid fields">
        <SettingField
          v-for="suffix in CH_FIELD_ORDER"
          :key="suffix"
          :meta="channelMeta(config, suffix)"
          :display-key="chKey(idx, suffix)"
          :value="valueOf(config, chKey(idx, suffix)) ?? ''"
          :source="sourceOf(config, chKey(idx, suffix))"
          :editable="!!config.file"
          :required="suffix === 'DIRECTION' || suffix === 'SUBDEVICE_INDEX'"
          :subdevice-count="config.card_subdevices || 8"
          v-model="drafts[idx][chKey(idx, suffix)]"
          @update:model-value="() => {
            if (suffix === 'DIRECTION') onDirectionChange(idx);
            if (suffix === 'MXL_GROUP_HINT') onGroupHintChange(idx);
            if (suffix === 'LABEL') onLabelChange(idx);
          }"
        />
      </div>

      <div v-if="(drafts[idx][chKey(idx, 'AUDIO_ENABLE')] || 'true') !== 'false'" class="audio-matrix">
        <h4 style="margin:1rem 0 .4rem;font-size:.95rem">Audio flows (routing matrix)</h4>
        <p class="muted" style="font-size:.75rem;margin:0 0 .6rem">
          Each flow maps a subset of the DeckLink interleaved channels (<code>AUDIO_CHANNEL_COUNT</code>).
          Unmapped channels are silence. Outputs: each DeckLink channel may appear in at most one map (no mixing);
          use the nil UUID to leave a flow unassigned.
        </p>
        <div v-for="af in afCount(idx)" :key="af" class="af-row">
          <div class="af-title">AF{{ af - 1 }}</div>
          <div class="grid fields af-fields">
            <SettingField
              v-for="field in AF_FIELDS"
              :key="field"
              :meta="audioFlowMeta(config, field)"
              :display-key="afKey(idx, af - 1, field)"
              :value="valueOf(config, afKey(idx, af - 1, field)) ?? ''"
              :source="sourceOf(config, afKey(idx, af - 1, field))"
              :editable="!!config.file"
              :required="field !== 'LABEL'"
              v-model="drafts[idx][afKey(idx, af - 1, field)]"
              @update:model-value="() => { if (field === 'CHANNEL_COUNT') onChannelCountChange(idx, af - 1); }"
            />
          </div>
          <button
            class="btn danger small"
            type="button"
            :disabled="!config.file || afCount(idx) <= 1"
            @click="removeAudioFlow(idx, af - 1)"
          >Remove flow</button>
        </div>
        <button
          class="btn secondary small"
          type="button"
          :disabled="!config.file || afCount(idx) >= 16"
          @click="addAudioFlow(idx)"
        >+ Add audio flow</button>
      </div>

      <div class="actions">
        <button
          v-if="!extraPanels.includes(idx)"
          class="btn danger"
          :disabled="!config.file || anyEnv(idx)"
          @click="remove(idx)"
        >Remove</button>
        <button class="btn" :disabled="!config.file" @click="save(idx, extraPanels.includes(idx))">
          {{ extraPanels.includes(idx) ? "Create" : "Save & apply" }}
        </button>
      </div>
      <div v-if="messages[idx]" class="msg" :class="messages[idx].kind">{{ messages[idx].text }}</div>
    </div>
    <button
      class="btn"
      :disabled="!config.file || panelIndices.length >= 16"
      @click="addChannel"
    >+ Add channel</button>
  </template>
</template>

<style scoped>
.audio-matrix {
  border-top: 1px solid var(--border, #333);
  margin-top: .8rem;
  padding-top: .2rem;
}
.af-row {
  margin-bottom: .75rem;
  padding: .5rem .6rem;
  background: rgba(127, 127, 127, 0.08);
  border-radius: 4px;
}
.af-title {
  font-weight: 600;
  font-size: .8rem;
  margin-bottom: .35rem;
  letter-spacing: .02em;
}
.af-fields {
  margin-bottom: .4rem;
}
</style>
