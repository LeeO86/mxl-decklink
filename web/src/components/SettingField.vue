<script setup>
// One configuration field, rendered from schema metadata with provenance
// badges (§7.5.2): env-supplied settings are read-only.
import { computed } from "vue";

const props = defineProps({
  meta: { type: Object, required: true }, // schema entry
  displayKey: { type: String, required: true }, // full key, e.g. CH0_DIRECTION
  value: { type: String, default: "" },
  source: { type: String, default: "default" }, // default/file/env
  editable: { type: Boolean, default: true }, // false when no config file
  required: { type: Boolean, default: false }, // hide the "(unset)" choice
  subdeviceCount: { type: Number, default: 0 }, // for SUBDEVICE_INDEX
});
const model = defineModel({ type: String, default: "" });

const disabled = computed(() => props.source === "env" || !props.editable);
const isSelect = computed(
  () => props.meta.type === "enum" || props.meta.type === "bool" || props.meta.key === "SUBDEVICE_INDEX"
);
const options = computed(() => {
  if (props.meta.key === "SUBDEVICE_INDEX") {
    return [...Array(props.subdeviceCount || 8).keys()].map((i) => ({ value: String(i), label: `sub-device ${i}` }));
  }
  const opts = props.meta.type === "bool" ? ["true", "false"] : props.meta.options;
  return opts.map((o) => ({ value: o, label: o }));
});
const unsetLabel = computed(() => (props.meta.default ? `(default: ${props.meta.default})` : "(unset)"));
</script>

<template>
  <div>
    <label :title="meta.help">
      {{ meta.key }}
      <span v-if="source === 'env'" class="envbadge" title="Set via environment variable — unset it to edit here">ENV</span>
      <span v-else-if="source === 'file'" class="envbadge filebadge" title="From the configuration file">FILE</span>
      <span v-if="meta.requires_restart && meta.kind === 'global'" class="envbadge" title="Applies on next container start">RESTART</span>
    </label>
    <select v-if="isSelect" v-model="model" :disabled="disabled">
      <option v-if="!required" value="">{{ unsetLabel }}</option>
      <option v-for="o in options" :key="o.value" :value="o.value">{{ o.label }}</option>
    </select>
    <input v-else v-model="model" :placeholder="meta.default || ''" :disabled="disabled" />
  </div>
</template>
