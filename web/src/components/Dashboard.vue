<script setup>
import { onMounted, onUnmounted, ref } from "vue";
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
            <th v-for="h in ['#','Label','Dir','Sub','State','Mode','Signal','Frames','Drops','Grains','Reconn','Flow']" :key="h">{{ h }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="!status.channels.length">
            <td colspan="12" class="muted">no channels configured</td>
          </tr>
          <tr v-for="c in status.channels" :key="c.index">
            <td>{{ c.index }}</td>
            <td>{{ c.label }}</td>
            <td>{{ c.direction }}</td>
            <td>{{ c.subdevice_index }}</td>
            <td><Pill :text="c.state" :kind="c.state" /></td>
            <td>{{ c.active_video_mode || c.video_mode_setting }}</td>
            <td><Pill :text="c.signal_locked ? 'locked' : 'no signal'" :kind="c.signal_locked ? 'on' : 'off'" /></td>
            <td>{{ c.frames_total }}</td>
            <td>{{ c.frames_dropped }}</td>
            <td>{{ c.grains_committed }}</td>
            <td>{{ c.reconnects }}</td>
            <td><code>{{ c.active_video_flow_id.slice(0, 8) }}…</code></td>
          </tr>
        </tbody>
      </table>
    </div>
  </template>
</template>
