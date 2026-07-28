<script setup>
// Visual DeckLink × audio-flow crosspoint editor. Writes CHx_AFn_MAP as an
// ordered list (flow channel i ← DeckLink column). Output channels enforce
// one source per DeckLink column (no mixing).
import { computed } from "vue";

const props = defineProps({
  open: { type: Boolean, default: false },
  channelIndex: { type: Number, required: true },
  direction: { type: String, default: "input" },
  deckChannelCount: { type: Number, default: 16 },
  /** [{ index, label, channelCount, map: number[] }] */
  flows: { type: Array, default: () => [] },
  editable: { type: Boolean, default: true },
});

const emit = defineEmits(["close", "apply"]);

const isOutput = computed(() => props.direction === "output");

const deckCols = computed(() =>
  [...Array(Math.max(0, props.deckChannelCount)).keys()]
);

/** Flattened rows: one per (flow, flowChannel). */
const rows = computed(() => {
  const out = [];
  for (const f of props.flows) {
    const count = Math.max(1, Number(f.channelCount) || 1);
    const map = Array.isArray(f.map) ? f.map : [];
    for (let fc = 0; fc < count; ++fc) {
      out.push({
        flowIndex: f.index,
        flowLabel: f.label || `AF${f.index}`,
        flowChannel: fc,
        selected: map[fc] ?? null,
      });
    }
  }
  return out;
});

/** DeckLink column → which flow rows claim it (for conflict highlight). */
const columnOwners = computed(() => {
  const owners = new Map();
  for (const row of rows.value) {
    if (row.selected == null || row.selected < 0) continue;
    const list = owners.get(row.selected) || [];
    list.push(row);
    owners.set(row.selected, list);
  }
  return owners;
});

function cellClass(row, dl) {
  const selected = row.selected === dl;
  const owners = columnOwners.value.get(dl) || [];
  const conflict = isOutput.value && owners.length > 1;
  return {
    selected,
    conflict: selected && conflict,
    muted: !selected && conflict,
  };
}

function pick(row, dl) {
  if (!props.editable) return;
  // Working copy of maps keyed by flow index.
  const byFlow = new Map();
  for (const f of props.flows) {
    const count = Math.max(1, Number(f.channelCount) || 1);
    const map = Array.isArray(f.map) ? [...f.map] : [];
    while (map.length < count) map.push(0);
    byFlow.set(f.index, map.slice(0, count));
  }
  const map = byFlow.get(row.flowIndex);
  if (!map) return;

  if (map[row.flowChannel] === dl) {
    // Toggle off → leave a placeholder 0 (engineer can re-pick); keep length.
    // Prefer next free column when clearing would leave a duplicate on output.
    map[row.flowChannel] = -1;
  } else {
    map[row.flowChannel] = dl;
    if (isOutput.value) {
      // Clear this DeckLink channel from every other flow slot.
      for (const [fi, m] of byFlow) {
        for (let i = 0; i < m.length; ++i) {
          if (fi === row.flowIndex && i === row.flowChannel) continue;
          if (m[i] === dl) m[i] = -1;
        }
      }
    }
  }

  // Replace -1 placeholders with the lowest unused DeckLink index so MAP stays valid.
  const used = new Set();
  for (const m of byFlow.values()) {
    for (const v of m) {
      if (v >= 0) used.add(v);
    }
  }
  const result = [];
  for (const f of props.flows) {
    const m = byFlow.get(f.index);
    for (let i = 0; i < m.length; ++i) {
      if (m[i] < 0) {
        let next = 0;
        while (used.has(next) && next < props.deckChannelCount) next++;
        if (next >= props.deckChannelCount) next = Math.min(i, props.deckChannelCount - 1);
        m[i] = next;
        used.add(next);
      }
    }
    result.push({ flowIndex: f.index, map: m });
  }
  emit("apply", result);
}

function close() {
  emit("close");
}
</script>

<template>
  <dialog :open="open" class="matrix-dialog" style="max-width:min(960px,96vw);width:96vw" @close="close">
    <form style="padding:1rem" @submit.prevent>
      <h3 style="margin-top:0">Audio routing matrix — CH{{ channelIndex }}</h3>
      <p class="note">
        Rows are MXL flow channels; columns are DeckLink interleaved channels.
        Click a cell to route. Unmapped DeckLink columns stay silence.
        <template v-if="isOutput"> Outputs: each DeckLink column may feed at most one flow channel (no mixing).</template>
        <template v-else> Inputs: the same DeckLink column may fan out to multiple flows.</template>
      </p>
      <div class="matrix-scroll">
        <table class="matrix-table">
          <thead>
            <tr>
              <th>Flow</th>
              <th v-for="dl in deckCols" :key="dl">DL{{ dl }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-if="!rows.length">
              <td :colspan="deckCols.length + 1" class="muted">no audio flows</td>
            </tr>
            <tr v-for="(row, ri) in rows" :key="ri">
              <th scope="row">
                <span class="flow-tag">AF{{ row.flowIndex }}</span>
                <span class="muted">.{{ row.flowChannel }}</span>
                <div class="muted" style="font-weight:400;font-size:.7rem">{{ row.flowLabel }}</div>
              </th>
              <td
                v-for="dl in deckCols"
                :key="dl"
                class="matrix-cell"
                :class="cellClass(row, dl)"
                :title="`AF${row.flowIndex} ch${row.flowChannel} ← DeckLink ${dl}`"
                @click="pick(row, dl)"
              >
                <span v-if="row.selected === dl">●</span>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <div class="actions">
        <button type="button" class="btn secondary" @click="close">Close</button>
      </div>
    </form>
  </dialog>
</template>

<style scoped>
.matrix-dialog {
  max-width: min(960px, 96vw);
  width: 96vw;
}
.matrix-scroll {
  overflow: auto;
  max-height: 60vh;
  border: 1px solid var(--border, #2a3644);
  border-radius: 6px;
}
.matrix-table {
  border-collapse: separate;
  border-spacing: 0;
  font-size: .78rem;
  min-width: 100%;
}
.matrix-table th,
.matrix-table td {
  border-bottom: 1px solid var(--border, #2a3644);
  border-right: 1px solid var(--border, #2a3644);
  text-align: center;
  padding: .35rem .4rem;
  white-space: nowrap;
}
.matrix-table thead th {
  position: sticky;
  top: 0;
  background: var(--panel, #161d26);
  z-index: 1;
}
.matrix-table tbody th {
  position: sticky;
  left: 0;
  background: var(--panel, #161d26);
  text-align: left;
  min-width: 7rem;
  z-index: 1;
}
.matrix-cell {
  width: 2.2rem;
  height: 2rem;
  cursor: pointer;
  background: var(--panel2, #1d2733);
  user-select: none;
}
.matrix-cell:hover {
  outline: 1px solid var(--accent, #4da3ff);
  outline-offset: -1px;
}
.matrix-cell.selected {
  background: rgba(77, 163, 255, .25);
  color: var(--accent, #4da3ff);
  font-size: 1rem;
}
.matrix-cell.conflict {
  background: rgba(224, 90, 78, .3);
  color: var(--bad, #e05a4e);
}
.matrix-cell.muted {
  opacity: .45;
}
.flow-tag {
  font-weight: 600;
}
</style>
