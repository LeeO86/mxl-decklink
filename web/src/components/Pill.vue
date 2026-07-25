<script setup>
defineProps({
  text: { type: String, required: true },
  kind: { type: String, required: true }, // healthy/degraded/failed/init/on/off
});
</script>

<template>
  <span class="pill" :class="kind">{{ text }}</span>
</template>
