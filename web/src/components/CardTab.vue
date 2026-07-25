<script setup>
import { onMounted, onUnmounted, ref } from "vue";
import { api } from "../api.js";
import Pill from "./Pill.vue";

const card = ref(null);
const error = ref("");

async function refresh() {
  try {
    card.value = await api.get("/api/card");
    error.value = "";
  } catch (e) {
    error.value = String(e);
  }
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
  <template v-else-if="card">
    <div class="panel" style="margin-bottom:.9rem">
      <h3>Card</h3>
      <dl class="kv">
        <dt>Name</dt><dd>{{ card.name }}</dd>
        <dt>Persistent ID</dt><dd>{{ card.persistent_id }}</dd>
      </dl>
    </div>
    <div class="grid cards">
      <div v-for="sd in card.subdevices" :key="sd.index" class="panel">
        <h3>Sub-device {{ sd.index }} — {{ sd.display_name }}</h3>
        <dl class="kv">
          <dt>Model</dt><dd>{{ sd.model }}</dd>
          <dt>Capture / Playback</dt>
          <dd>{{ sd.supports_capture ? "✓" : "—" }} / {{ sd.supports_playback ? "✓" : "—" }}</dd>
          <dt>Format detection</dt>
          <dd><Pill :text="sd.supports_format_detection ? 'yes' : 'no'" :kind="sd.supports_format_detection ? 'on' : 'off'" /></dd>
          <dt>Detected input</dt><dd>{{ sd.status.detected_input_mode || "—" }}</dd>
          <dt>Current input</dt><dd>{{ sd.status.current_input_mode || "—" }}</dd>
          <dt>Current output</dt><dd>{{ sd.status.current_output_mode || "—" }}</dd>
          <dt>Input pixel format</dt><dd>{{ sd.status.current_input_pixel_format || "—" }}</dd>
          <dt>Signal lock</dt>
          <dd><Pill :text="sd.status.input_signal_locked ? 'locked' : 'unlocked'" :kind="sd.status.input_signal_locked ? 'on' : 'off'" /></dd>
          <dt>Reference lock</dt>
          <dd><Pill :text="sd.status.reference_locked ? 'locked' : 'unlocked'" :kind="sd.status.reference_locked ? 'on' : 'off'" /></dd>
          <dt>Busy</dt>
          <dd>capture: {{ sd.status.capture_busy ? "yes" : "no" }}, playback: {{ sd.status.playback_busy ? "yes" : "no" }}</dd>
          <dt>PCIe</dt>
          <dd>{{ sd.status.pcie_link_width ? `x${sd.status.pcie_link_width} gen${sd.status.pcie_link_speed ?? "?"}` : "—" }}</dd>
          <dt>Temperature</dt>
          <dd>{{ sd.status.temperature_c ? `${sd.status.temperature_c} °C` : "—" }}</dd>
        </dl>
      </div>
    </div>
  </template>
</template>
