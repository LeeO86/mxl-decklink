<script setup>
import { onMounted, reactive, ref } from "vue";
import { api, chKey, channelIndices, channelMeta, sourceOf, valueOf } from "../api.js";
import SettingField from "./SettingField.vue";

const CH_FIELD_ORDER = [
  "DIRECTION", "SUBDEVICE_INDEX", "VIDEO_MODE", "PIXEL_FORMAT", "MXL_VIDEO_FLOW_ID", "MXL_AUDIO_FLOW_ID",
  "AUDIO_ENABLE", "AUDIO_CHANNEL_COUNT", "AUDIO_SAMPLE_TYPE", "VIDEO_ANC_ENABLE", "MXL_ANC_FLOW_ID", "LABEL",
  "MXL_VIDEO_FLOW_LABEL", "MXL_AUDIO_FLOW_LABEL", "MXL_GROUP_HINT", "MXL_DEVICE_ID", "MXL_SOURCE_ID",
  "ALLOW_FORMAT_CONVERSION", "GRAIN_COUNT", "AUDIO_BUFFER_MS", "COMMIT_BATCH_HINT", "OUTPUT_PREROLL_GRAINS", "READER_TIMEOUT_MS",
];

const config = ref(null);
const error = ref("");
const drafts = reactive({}); // idx -> { key -> string }
const messages = reactive({}); // idx -> { kind, text }
const extraPanels = ref([]); // newly added channel indices not yet in config

function initDraft(cfg, idx) {
  const d = {};
  for (const suffix of CH_FIELD_ORDER) {
    const key = chKey(idx, suffix);
    d[key] = valueOf(cfg, key) ?? "";
  }
  drafts[idx] = d;
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

function anyEnv(idx) {
  return CH_FIELD_ORDER.some((sfx) => sourceOf(config.value, chKey(idx, sfx)) === "env");
}

async function save(idx, isNew) {
  const cfg = config.value;
  const set = {};
  const unset = [];
  for (const suffix of CH_FIELD_ORDER) {
    const key = chKey(idx, suffix);
    if (sourceOf(cfg, key) === "env") continue;
    const v = (drafts[idx][key] ?? "").trim();
    if (v === "") {
      if (sourceOf(cfg, key) === "file") unset.push(key);
    } else {
      set[key] = v;
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
  const unset = CH_FIELD_ORDER.map((sfx) => chKey(idx, sfx)).filter((k) => sourceOf(config.value, k) === "file");
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
  extraPanels.value = [...extraPanels.value, freeIdx];
}

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
      v-for="idx in [...channelIndices(config), ...extraPanels]"
      :key="idx"
      class="panel"
      style="margin-bottom:.9rem"
    >
      <h3>
        Channel {{ idx }}{{ extraPanels.includes(idx) ? " (new)" : "" }}
        — {{ valueOf(config, chKey(idx, "LABEL")) || ("ch" + idx) }}
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
        />
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
      :disabled="!config.file || [...channelIndices(config), ...extraPanels].length >= 16"
      @click="addChannel"
    >+ Add channel</button>
  </template>
</template>
