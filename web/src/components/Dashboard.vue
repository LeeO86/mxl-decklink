<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";
import { api } from "../api.js";
import Pill from "./Pill.vue";

const status = ref(null);
const error = ref("");

async function refresh() {
  try {
    status.value = await api.get("/api/status");
    error.value = "";
  } catch (e) {
    error.value = String(e);
  }
}

function fmtUptime(s) {
  return `${Math.floor(s / 3600)}h ${Math.floor(s / 60) % 60}m ${s % 60}s`;
}

function audioSummary(c) {
  const a = c.audio;
  if (!a || !a.enabled) return { text: "off", kind: "off" };
  const flows = a.flows?.length ?? 0;
  const assigned = (a.flows ?? []).filter((f) => !f.unassigned).length;
  const live = (a.flows ?? []).filter((f) => f.mxl_active).length;
  const text = `${a.deck_channel_count}ch · ${flows} flow${flows === 1 ? "" : "s"}`;
  if (assigned === 0) return { text: `${text} · unassigned`, kind: "off" };
  if (live > 0) return { text: `${text} · ${live} live`, kind: "on" };
  return { text: `${text} · idle`, kind: "off" };
}

function mapLabel(map) {
  if (!map?.length) return "—";
  return map.join(",");
}

function flowMxlPill(f) {
  if (f.unassigned) return { text: "unassigned", kind: "off" };
  if (!f.mxl_present) return { text: "missing", kind: "failed" };
  return f.mxl_active ? { text: "live", kind: "on" } : { text: "idle", kind: "off" };
}

function signalLabel(c) {
  if (c.direction === "output") {
    return c.signal_locked
      ? { text: "ref locked", kind: "on" }
      : { text: "no ref", kind: "off" };
  }
  return c.signal_locked
    ? { text: "locked", kind: "on" }
    : { text: "no signal", kind: "off" };
}

const audioRows = computed(() => {
  if (!status.value?.channels) return [];
  const rows = [];
  for (const c of status.value.channels) {
    if (!c.audio?.enabled) {
      rows.push({ channel: c, flow: null });
      continue;
    }
    if (!c.audio.flows?.length) {
      rows.push({ channel: c, flow: null });
      continue;
    }
    for (const f of c.audio.flows) {
      rows.push({ channel: c, flow: f });
    }
  }
  return rows;
});

let timer = null;
onMounted(() => {
  refresh();
  timer = setInterval(refresh, 2000);
});
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <div v-if="error" class="panel">API error: {{ error }}</div>
  <template v-else-if="status">
    <div class="grid cards">
      <div class="panel">
        <h3>Process</h3>
        <dl class="kv">
          <dt>Version</dt><dd>{{ status.version }}</dd>
          <dt>MXL</dt><dd>{{ status.mxl_version }}</dd>
          <dt>DeckLink API</dt><dd>{{ status.decklink_api_version || "n/a" }}</dd>
          <dt>Uptime</dt><dd>{{ fmtUptime(status.uptime_s) }}</dd>
        </dl>
      </div>
      <div class="panel">
        <h3>Card</h3>
        <dl class="kv">
          <dt>Name</dt><dd>{{ status.card.name }}</dd>
          <dt>Persistent ID</dt><dd>{{ status.card.persistent_id }}</dd>
          <dt>Sub-devices</dt><dd>{{ status.card.subdevices }}</dd>
          <dt>Backend</dt><dd>{{ status.backend }}</dd>
        </dl>
      </div>
      <div class="panel">
        <h3>MXL domain</h3>
        <dl class="kv">
          <dt>Path</dt><dd>{{ status.domain.path }}</dd>
          <dt>tmpfs</dt><dd><Pill :text="status.domain.tmpfs ? 'yes' : 'no'" :kind="status.domain.tmpfs ? 'on' : 'off'" /></dd>
        </dl>
      </div>
    </div>

    <div class="panel" style="margin-top:.9rem">
      <h3>Channels</h3>
      <table>
        <thead>
          <tr>
            <th v-for="h in ['#','Label','Dir','Sub','State','Mode','Signal','Audio','Frames','Drops','Grains','Reconn','Video flow']" :key="h">{{ h }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="!status.channels.length">
            <td colspan="13" class="muted">no channels configured</td>
          </tr>
          <tr v-for="c in status.channels" :key="c.index">
            <td>{{ c.index }}</td>
            <td>{{ c.label }}</td>
            <td>{{ c.direction }}</td>
            <td>{{ c.subdevice_index }}</td>
            <td><Pill :text="c.state" :kind="c.state" /></td>
            <td>{{ c.active_video_mode || c.video_mode_setting }}</td>
            <td>
              <Pill :text="signalLabel(c).text" :kind="signalLabel(c).kind" />
            </td>
            <td>
              <Pill :text="audioSummary(c).text" :kind="audioSummary(c).kind" />
            </td>
            <td>{{ c.frames_total }}</td>
            <td>{{ c.frames_dropped }}</td>
            <td>{{ c.grains_committed }}</td>
            <td>{{ c.reconnects }}</td>
            <td><code>{{ c.active_video_flow_id.slice(0, 8) }}…</code></td>
          </tr>
        </tbody>
      </table>
    </div>

    <div class="panel" style="margin-top:.9rem">
      <h3>Audio overview</h3>
      <p class="note">
        Routing matrix from config, joined with live MXL flow state (domain flock / head index)
        and DeckLink output buffer depth (<code>GetBufferedAudioSampleFrameCount</code>).
        The SDK does not expose per-channel audio level meters.
      </p>
      <table>
        <thead>
          <tr>
            <th v-for="h in ['Ch','Dir','AF','Label','Deck map','MXL ch','Flow','MXL','DL buffer']" :key="h">{{ h }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="!audioRows.length">
            <td colspan="9" class="muted">no channels configured</td>
          </tr>
          <template v-for="(row, i) in audioRows" :key="i">
            <tr v-if="!row.channel.audio?.enabled">
              <td>{{ row.channel.index }}</td>
              <td>{{ row.channel.direction }}</td>
              <td colspan="7" class="muted">audio disabled</td>
            </tr>
            <tr v-else-if="!row.flow">
              <td>{{ row.channel.index }}</td>
              <td>{{ row.channel.direction }}</td>
              <td colspan="7" class="muted">no audio flows configured</td>
            </tr>
            <tr v-else>
              <td>{{ row.channel.index }} {{ row.channel.label }}</td>
              <td>{{ row.channel.direction }}</td>
              <td>AF{{ row.flow.index }}</td>
              <td>{{ row.flow.label || "—" }}</td>
              <td><code>{{ mapLabel(row.flow.map) }}</code></td>
              <td>{{ row.flow.channel_count }}</td>
              <td>
                <code v-if="!row.flow.unassigned" :title="row.flow.flow_id">{{ row.flow.flow_id.slice(0, 8) }}…</code>
                <span v-else class="muted">nil</span>
              </td>
              <td>
                <Pill :text="flowMxlPill(row.flow).text" :kind="flowMxlPill(row.flow).kind" />
                <span v-if="row.flow.mxl_head_index != null" class="muted" style="margin-left:.35rem;font-size:.75rem">
                  head {{ row.flow.mxl_head_index }}
                </span>
              </td>
              <td>
                <template v-if="row.channel.direction === 'output' && row.channel.audio.decklink_buffered_audio_frames != null">
                  {{ row.channel.audio.decklink_buffered_audio_frames }}
                  <span class="muted">frames</span>
                </template>
                <span v-else class="muted">—</span>
              </td>
            </tr>
          </template>
        </tbody>
      </table>
    </div>
  </template>
</template>
