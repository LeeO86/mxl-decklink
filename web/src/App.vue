<script setup>
import { onMounted, onUnmounted, ref } from "vue";
import { api } from "./api.js";
import Dashboard from "./components/Dashboard.vue";
import ChannelsTab from "./components/ChannelsTab.vue";
import CardTab from "./components/CardTab.vue";
import MxlTab from "./components/MxlTab.vue";
import SettingsTab from "./components/SettingsTab.vue";

const tabs = [
  { id: "dashboard", label: "Dashboard", component: Dashboard },
  { id: "channels", label: "Channels", component: ChannelsTab },
  { id: "card", label: "Card", component: CardTab },
  { id: "mxl", label: "MXL", component: MxlTab },
  { id: "settings", label: "Settings", component: SettingsTab },
];

const currentTab = ref("dashboard");
const status = ref(null);

function switchTab(id) {
  currentTab.value = id;
  location.hash = id;
}

function onHashChange() {
  const tab = location.hash.slice(1);
  if (tabs.some((t) => t.id === tab)) currentTab.value = tab;
}

async function refreshHeader() {
  try {
    status.value = await api.get("/api/status");
  } catch {
    /* header stays stale until the next poll */
  }
}

let timer = null;
onMounted(() => {
  onHashChange();
  window.addEventListener("hashchange", onHashChange);
  refreshHeader();
  timer = setInterval(refreshHeader, 3000);
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHashChange);
  clearInterval(timer);
});
</script>

<template>
  <header>
    <h1>mxl-decklink</h1>
    <span class="card-name" v-if="status">
      {{ status.card.name }} ({{ status.card.persistent_id }}) — {{ status.backend }} backend
    </span>
    <span style="flex:1"></span>
    <span class="muted" style="font-size:.75rem" v-if="status">
      v{{ status.version }} · MXL {{ status.mxl_version }}
      <template v-if="status.decklink_api_version"> · DeckLink API {{ status.decklink_api_version }}</template>
    </span>
  </header>
  <div class="restart-banner" v-if="status?.restart_required">
    Global configuration changed — restart the container to apply.
  </div>
  <div class="restart-banner" v-if="status?.card_open_fallback">
    No DeckLink card opened — running the mock card so the UI stays reachable. Set a card selector and restart before enabling live channels.
  </div>
  <nav>
    <button
      v-for="t in tabs"
      :key="t.id"
      :class="{ active: currentTab === t.id }"
      @click="switchTab(t.id)"
    >{{ t.label }}</button>
  </nav>
  <main>
    <component :is="tabs.find(t => t.id === currentTab).component" />
  </main>
</template>
