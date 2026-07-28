<script setup>
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from "vue";
import {
  NIL_UUID,
  afKey,
  api,
  chKey,
  channelIndices,
  channelMeta,
  fmtRate,
  identityMap,
  modeNameForFlow,
  valueOf,
} from "../api.js";
import Pill from "./Pill.vue";

const domains = ref(null);
const config = ref(null);
const flows = ref(null);
const selectedDomain = ref(null);
const error = ref("");
const createMsg = ref({ kind: "", text: "" });
const createForm = ref({ path: "", label: "", description: "", history_ms: "" });

const assignDlg = ref(null);
const assignFlow = ref(null);
const assignChannel = ref("");
const assignAudio = ref("");
const assignHint = ref({ kind: "", text: "" });

const videoModeOpts = computed(() => channelMeta(config.value, "VIDEO_MODE")?.options ?? []);
const assignMode = computed(() =>
  assignFlow.value ? modeNameForFlow(assignFlow.value, videoModeOpts.value) : null
);

async function load() {
  try {
    const [doms, cfg] = await Promise.all([api.get("/api/domains"), api.get("/api/config")]);
    domains.value = doms;
    config.value = cfg;
    error.value = "";
    if (!selectedDomain.value) {
      selectedDomain.value = (doms.domains.find((d) => d.current) || doms.domains[0])?.path ?? null;
    }
    if (!createForm.value.path && doms.scan_path) {
      createForm.value.path = `${doms.scan_path}/`;
    }
  } catch (e) {
    error.value = String(e);
  }
}

async function loadFlows() {
  if (!selectedDomain.value) {
    flows.value = null;
    return;
  }
  flows.value = await api.get(`/api/flows?domain=${encodeURIComponent(selectedDomain.value)}`);
}

watch(selectedDomain, loadFlows);

async function createDomain() {
  const body = {
    path: createForm.value.path.trim(),
    label: createForm.value.label.trim(),
    description: createForm.value.description.trim(),
  };
  if (createForm.value.history_ms) body.history_ms = Number(createForm.value.history_ms);
  const res = await api.post("/api/domains", body);
  if (res.error) {
    createMsg.value = { kind: "err", text: res.error };
  } else {
    createMsg.value = {
      kind: "ok",
      text: `Domain created at ${res.path} (id ${res.id})${res.tmpfs ? "" : " — WARNING: not tmpfs-backed!"}`,
    };
    selectedDomain.value = res.path;
    setTimeout(load, 800);
  }
}

async function openAssign(flow) {
  assignFlow.value = flow;
  const cfg = config.value;
  const outputs = channelIndices(cfg).filter((i) => valueOf(cfg, chKey(i, "DIRECTION")) === "output");
  const freeIdx = [...Array(16).keys()].find((i) => !channelIndices(cfg).includes(i));
  assignChannel.value = outputs.length
    ? String(outputs[0])
    : freeIdx !== undefined
      ? `new:${freeIdx}`
      : "";
  assignAudio.value = "";
  const mode = modeNameForFlow(flow, videoModeOpts.value);
  assignHint.value = mode
    ? { kind: "ok", text: `Video mode will be set to ${mode}.` }
    : { kind: "err", text: "WARNING: no DeckLink video mode matches this flow's geometry — assignment will keep the channel's current mode and may fail." };
  await nextTick();
  if (assignDlg.value && !assignDlg.value.open) assignDlg.value.showModal();
}

function closeAssign() {
  if (assignDlg.value?.open) assignDlg.value.close();
  else onAssignDialogClose();
}

function onAssignDialogClose() {
  assignFlow.value = null;
}

async function confirmAssign() {
  const flow = assignFlow.value;
  const cfg = config.value;
  let idx = assignChannel.value;
  const set = {};
  if (idx.startsWith("new:")) {
    idx = idx.slice(4);
    set[chKey(idx, "DIRECTION")] = "output";
    set[chKey(idx, "SUBDEVICE_INDEX")] = "0";
  }
  set[chKey(idx, "MXL_VIDEO_FLOW_ID")] = flow.id;
  if (assignAudio.value) {
    const audioFlow = audioOptions.value.find((f) => f.id === assignAudio.value);
    const chCount = Number(audioFlow?.channel_count) || 2;
    set[afKey(idx, 0, "FLOW_ID")] = assignAudio.value;
    set[afKey(idx, 0, "CHANNEL_COUNT")] = String(chCount);
    set[afKey(idx, 0, "MAP")] = identityMap(chCount);
    if (!valueOf(cfg, afKey(idx, 0, "LABEL"))) {
      set[afKey(idx, 0, "LABEL")] = audioFlow?.label || `ch${idx}-audio1`;
    }
    set[chKey(idx, "AUDIO_ENABLE")] = "true";
  } else if (!valueOf(cfg, afKey(idx, 0, "FLOW_ID")) || valueOf(cfg, afKey(idx, 0, "FLOW_ID")) === NIL_UUID) {
    set[chKey(idx, "AUDIO_ENABLE")] = "false";
  }
  if (assignMode.value) set[chKey(idx, "VIDEO_MODE")] = assignMode.value;
  const res = await api.put("/api/config", { set, unset: [] });
  closeAssign();
  if (res.error) alert(res.error);
  else {
    await load();
    await loadFlows();
  }
}

const outputOptions = computed(() => {
  if (!config.value) return [];
  const cfg = config.value;
  const opts = channelIndices(cfg)
    .filter((i) => valueOf(cfg, chKey(i, "DIRECTION")) === "output")
    .map((i) => ({ value: String(i), label: `channel ${i} (${valueOf(cfg, chKey(i, "LABEL")) || "ch" + i})` }));
  const freeIdx = [...Array(16).keys()].find((i) => !channelIndices(cfg).includes(i));
  if (freeIdx !== undefined) opts.push({ value: `new:${freeIdx}`, label: `new output channel ${freeIdx}` });
  return opts;
});

const audioOptions = computed(() =>
  (flows.value?.flows ?? []).filter((f) => f.media_type.startsWith("audio"))
);

let timer = null;
onMounted(async () => {
  await load();
  await loadFlows();
  timer = setInterval(async () => {
    await load();
    await loadFlows();
  }, 5000);
});
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <div v-if="error" class="panel">API error: {{ error }}</div>
  <template v-else-if="domains && config">
    <div class="panel" style="margin-bottom:.9rem">
      <h3>Domains under {{ domains.scan_path }}</h3>
      <p class="note">
        Domain deletion is not supported — remove unused domains from the host filesystem if needed.
      </p>
      <table>
        <thead>
          <tr>
            <th v-for="h in ['Label','Path','Flows','tmpfs','History','Description']" :key="h">{{ h }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="!domains.domains.length">
            <td colspan="6" class="muted">no domains found</td>
          </tr>
          <tr
            v-for="d in domains.domains"
            :key="d.path"
            class="selectable"
            :class="{ selected: d.path === selectedDomain }"
            @click="selectedDomain = d.path"
          >
            <td>{{ d.current ? "▶ " : "" }}{{ d.label || d.path.split("/").pop() }}</td>
            <td><code>{{ d.path }}</code></td>
            <td>{{ d.flow_count }}</td>
            <td><Pill :text="d.tmpfs ? 'yes' : 'no'" :kind="d.tmpfs ? 'on' : 'off'" /></td>
            <td>{{ d.history_duration_ns ? `${d.history_duration_ns / 1e6} ms` : "default" }}</td>
            <td class="muted">{{ d.description || "" }}</td>
          </tr>
        </tbody>
      </table>
    </div>

    <div class="panel">
      <h3>{{ selectedDomain ? `Flows in ${selectedDomain}` : "Flows" }}</h3>
      <div v-if="!selectedDomain" class="muted">no domain selected</div>
      <table v-else-if="flows">
        <thead>
          <tr>
            <th v-for="h in ['Flow ID','Type','Label','Geometry','Rate','State','Head','']" :key="h">{{ h }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="!flows.flows.length">
            <td colspan="8" class="muted">no flows in this domain</td>
          </tr>
          <tr v-for="f in flows.flows" :key="f.id">
            <td><code :title="f.id">{{ f.id.slice(0, 13) }}…</code></td>
            <td>{{ f.media_type }}</td>
            <td>{{ f.label || "" }}<span v-if="!f.label" class="muted">{{ f.group_hint }}</span></td>
            <td>
              <template v-if="f.width">{{ f.width }}×{{ f.height }}{{ f.interlaced ? "i" : "p" }}</template>
              <template v-else-if="f.channel_count">{{ f.channel_count }} ch</template>
            </td>
            <td>{{ f.media_type.startsWith("audio") ? "48 kHz" : fmtRate(f.rate_numerator, f.rate_denominator) }}</td>
            <td><Pill :text="f.active ? 'live' : 'idle'" :kind="f.active ? 'on' : 'off'" /></td>
            <td>{{ f.head_index ?? "" }}</td>
            <td>
              <button
                v-if="f.format.endsWith('video') && config.file"
                class="btn secondary small"
                @click="openAssign(f)"
              >→ output</button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <div class="panel" style="margin-top:.9rem">
      <h3>Create new domain</h3>
      <div class="row">
        <div>
          <label>Path</label>
          <input v-model="createForm.path" :placeholder="`${domains.scan_path}/my-domain`" />
        </div>
        <div>
          <label>Label</label>
          <input v-model="createForm.label" placeholder="label" />
        </div>
        <div>
          <label>Description</label>
          <input v-model="createForm.description" placeholder="description (optional)" />
        </div>
        <div>
          <label>History duration</label>
          <input v-model="createForm.history_ms" type="number" placeholder="history ms (optional, default 200)" />
        </div>
      </div>
      <div class="actions">
        <button class="btn" @click="createDomain">Create domain</button>
      </div>
      <div v-if="createMsg.text" class="msg" :class="createMsg.kind">{{ createMsg.text }}</div>
    </div>

    <Teleport to="body">
      <dialog ref="assignDlg" class="assign-dialog" @close="onAssignDialogClose">
        <form style="padding:1rem" @submit.prevent="confirmAssign">
          <h3 style="margin-top:0">Assign flow to output</h3>
          <div v-if="assignFlow" class="muted" style="font-size:.8rem">
            Video flow {{ assignFlow.id }} —
            {{ assignFlow.width }}×{{ assignFlow.height }}{{ assignFlow.interlaced ? "i" : "p" }}
            {{ fmtRate(assignFlow.rate_numerator, assignFlow.rate_denominator) }}
          </div>
          <label>Output channel</label>
          <select v-model="assignChannel">
            <option v-for="o in outputOptions" :key="o.value" :value="o.value">{{ o.label }}</option>
          </select>
          <label>Audio flow (optional)</label>
          <select v-model="assignAudio">
            <option value="">(none / keep current)</option>
            <option v-for="f in audioOptions" :key="f.id" :value="f.id">
              {{ f.label || f.id.slice(0, 13) }} ({{ f.channel_count }} ch)
            </option>
          </select>
          <div class="msg" :class="assignHint.kind">{{ assignHint.text }}</div>
          <div class="actions">
            <button type="button" class="btn secondary" @click="closeAssign">Cancel</button>
            <button type="submit" class="btn" :disabled="!assignChannel">Assign</button>
          </div>
        </form>
      </dialog>
    </Teleport>
  </template>
</template>

<style scoped>
.assign-dialog {
  margin: auto;
  padding: 0;
}
</style>
