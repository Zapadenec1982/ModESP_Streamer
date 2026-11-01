<script>
  // A slider whose zero is in the middle — Бас, Високі and the balance:
  // the one slider (Slider.svelte) — a 6 px rail, a round 20 px knob,
  // the name at the left and the number at the right — with a tick at the centre and the fill
  // running from the centre to the value; the number without a unit ("+2", "0", "−3"). Touch and the mouse are
  // lib/gesture.js's — a press changes nothing, a finger that goes up or down scrolls the page —
  // and a native range input under the track is for the keyboard and screen readers. `input`
  // fires while it moves, `change` when it lets go; with `undo`, a snackbar offers to put it back.
  import { createEventDispatcher } from 'svelte';
  import { slide, valueAt } from '../lib/gesture.js';
  import { noteUndo } from '../stores/toast.js';
  import { t } from '../stores/i18n.js';

  export let value = 0;
  export let min = -6;
  export let max = 6;
  export let step = 1;
  export let label = '';
  export let disabled = false;
  /** Words at the two ends in place of the name and the number — the balance's Л and П. */
  export let ends = null;
  /** The value in words, for a screen reader (the balance's «Лівіше · 3»). */
  export let format = null;
  /** Puts a value back: «Бас: 0 → +3 · Скасувати» after a move. */
  export let undo = null;

  const dispatch = createEventDispatcher();
  let from = null;     // where the move started

  $: v = Math.min(max, Math.max(min, Number(value) || 0));
  $: at = max > min ? (v - min) / (max - min) : 0.5;
  $: mid = max > min ? (0 - min) / (max - min) : 0.5;
  const say = (x) => (format ? format(x) : x > 0 ? `+${x}` : x < 0 ? `−${-x}` : '0');
  $: text = say(v);

  function set(x) {
    if (from === null) from = v;
    if (x === v) return;
    value = x;
    dispatch('input', x);
  }
  function commit() {
    dispatch('change', Number(value));
    const was = from;
    from = null;
    if (!undo || was === null || was === Number(value)) return;
    noteUndo({ key: label, from: was, to: Number(value), action: $t['undo.action'],
               text: (a, b) => `${label}: ${say(a)} → ${say(b)}`, revert: undo });
  }
</script>

<div class="row" class:disabled class:ends={!!ends}>
  <span class="name">{ends ? ends[0] : label}</span>
  <!-- the knob's centre runs 10 px in from either end: it never hangs off the rail -->
  <div class="track" style="--at:{at};--mid:{mid}"
       use:slide={{ at: (f) => set(valueAt(f, min, max, step)), end: commit, disabled, inset: 10 }}>
    <div class="rail"></div>
    <div class="zero"></div>
    <div class="fill"></div>
    <div class="knob"></div>
    <input type="range" {min} {max} {step} value={v} {disabled} aria-label={label} aria-valuetext={text}
           on:input={(e) => set(Number(e.target.value))} on:change={commit} />
  </div>
  <span class="val">{ends ? ends[1] : text}</span>
</div>

<style>
  /* the name and the number keep the track off the screen's edges: a finger that scrolls has
     somewhere to go that sets nothing */
  .row { display: flex; align-items: center; gap: 16px; min-height: 48px; }
  .name { width: 70px; font-size: 16px; flex: none; }
  .track { position: relative; flex: 1; height: 40px; min-width: 0; touch-action: pan-y; cursor: pointer; }
  .rail, .fill {
    position: absolute;
    top: 17px;
    height: 6px;
    border-radius: 3px;
  }
  .rail { left: 0; right: 0; background: var(--surface-highest); }
  .zero {
    position: absolute;
    left: calc(10px + var(--mid) * (100% - 20px) - 1px);
    top: 12px;
    width: 2px;
    height: 16px;
    border-radius: 1px;
    background: var(--outline);
  }
  .fill {
    background: var(--primary);
    left: calc(10px + min(var(--at), var(--mid)) * (100% - 20px));
    width: calc(max(var(--at) - var(--mid), var(--mid) - var(--at)) * (100% - 20px));
  }
  .knob {
    position: absolute;
    top: 10px;
    left: calc(var(--at) * (100% - 20px));
    width: 20px;
    height: 20px;
    border-radius: 10px;
    background: var(--primary);
  }
  input {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    margin: 0;
    opacity: 0;
    pointer-events: none;
  }
  .val {
    width: 26px;
    text-align: right;
    font-size: 15px;
    color: var(--on-surface-var);
    font-variant-numeric: tabular-nums;
    flex: none;
  }
  .ends .name, .ends .val {
    width: 16px;
    text-align: center;
    font-size: 13px;
    font-weight: 500;
    color: var(--outline);
  }
  .disabled { opacity: 0.38; }
  .disabled .track { cursor: default; }
  .track:has(input:focus-visible) .knob { outline: 2px solid var(--primary); outline-offset: 2px; }
</style>
