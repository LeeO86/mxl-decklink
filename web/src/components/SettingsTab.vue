<script setup>
import { onMounted, reactive, ref } from "vue";
import { api, sourceOf, valueOf } from "../api.js";
import SettingField from "./SettingField.vue";

const config = ref(null);
const error = ref("");
const drafts = reactive({});
const msg = ref({ kind: "", text: "" });

async function load() {
  try {
    config.value = await api.get("/api/config");
    error.value = "";
    for (const meta of config.value.schema.filter((m) => m.kind === "global")) {
      drafts[meta.key] = valueOf(config.value, meta.key) ?? "";
    }
  } catch (e) {
    error.value = String(e);
  }
}

async function save() {
  const cfg = config.value;
  const globals = cfg.schema.filter((m) => m.kind === "global");
  const set = {};
  const unset = [];
  for (const meta of globals) {
    if (sourceOf(cfg, meta.key) === "env") continue;
    const v = (drafts[meta.key] ?? "").trim();
    if (v === "") {
      if (sourceOf(cfg, meta.key) === "file") unset.push(meta.key);
    } else {
      set[meta.key] = v;
    }
  }
  const res = await api.put("/api/config", { set, unset });
  if (res.error) {
    msg.value = { kind: "err", text: res.error };
  } else {
    msg.value = {
      kind: "ok",
      text: "Saved." + (res.restart_required ? " Container restart required to apply global changes." : ""),
    };
    await load();
  }
}

async function copyEnv() {
  try {
    await navigator.clipboard.writeText(config.value.env_block);
  } catch {
    /* clipboard may be unavailable over plain HTTP */
  }
}

onMounted(load);
</script>

<template>
  <div v-if="error" class="panel">API error: {{ error }}</div>
  <template v-else-if="config">
    <div class="panel">
      <h3>Configuration file</h3>
      <dl class="kv" style="font-size:.85rem">
        <dt>MXL_CONFIG_FILE</dt>
        <dd>{{ config.file || "not set — settings are read-only" }}</dd>
      </dl>
    </div>
    <div class="panel" style="margin-top:.9rem">
      <h3>Global settings</h3>
      <div class="grid" style="grid-template-columns:repeat(auto-fill,minmax(230px,1fr))">
        <SettingField
          v-for="meta in config.schema.filter(m => m.kind === 'global')"
          :key="meta.key"
          :meta="meta"
          :display-key="meta.key"
          :value="valueOf(config, meta.key) ?? ''"
          :source="sourceOf(config, meta.key)"
          :editable="!!config.file"
          v-model="drafts[meta.key]"
        />
      </div>
      <div class="actions">
        <button class="btn" :disabled="!config.file" @click="save">Save</button>
      </div>
      <div v-if="msg.text" class="msg" :class="msg.kind">{{ msg.text }}</div>
    </div>
    <div class="panel" style="margin-top:.9rem">
      <h3>
        Effective configuration as environment variables
        <button class="btn secondary copybtn" @click="copyEnv">Copy</button>
      </h3>
      <div class="muted" style="font-size:.75rem;margin-bottom:.4rem">
        Copy this block to persist the current setup as environment variables (they then override the file, §4.5).
      </div>
      <textarea readonly :value="config.env_block"></textarea>
    </div>
  </template>
</template>
