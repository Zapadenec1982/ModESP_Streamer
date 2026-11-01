<script>
  // The one slider of the web and the panel (m3w::slider, ui_m3_widgets.cpp): a 6 px rail,
  // primary from its left end to a round 20 px knob, the number to the right of the track, an
  // icon or a name at the left when given. Бас, Високі and the balance are the same with the zero
  // in the middle (CenterSlider). Touch and the mouse are lib/gesture.js's: a press changes
  // nothing, a finger that goes up or down scrolls the page, one that goes along drags. The
  // native <input type="range"> under the track is for the keyboard and screen readers. `input`
  // fires while it moves, `change` when it lets go; with `undo`, a snackbar offers to put it back
  // (stores/toast.js).
  import { createEventDispatcher } from 'svelte';
  import { slide, valueAt } from '../lib/gesture.js';
  import { noteUndo } from '../stores/toast.js';
  import { t } from '../stores/i18n.js';
  import Icon from './Icon.svelte';

  export let value = 0;
  export let min = 0;
  export let max = 100;
  export let step = 1;
  export let label;
  /** An icon at the left of the track. */
  export let icon = '';
  /** A name at the left of the track, where its row has no title of its own. */
  export let name = '';
  /** Formats the number at the right of the track; return '' to hide it. */
  export let format = (v) => String(v);
  export let disabled = false;
  /** Puts a value back: «{label}: 70 % → 50 % · Скасувати» after a move. */
  export let undo = null;

  const dispatch = createEventDispatcher();
  let from = null;     // where the move started

  $: frac = max > min ? Math.min(1, Math.max(0, (Number(value) - min) / (max - min))) : 0;
  $: shown = format(value);

  function set(v) {
    if (from === null) from = Number(value);
    if (v === Number(value)) return;
    value = v;
    dispatch('input', value);
  }
  function commit() {
    dispatch('change', Number(value));
    const was = from;
    from = null;
    if (!undo || was === null || was === Number(value)) return;
    const say = (v) => format(v) || String(v);
    noteUndo({ key: label, from: was, to: Number(value), action: $t['undo.action'],
               text: (a, b) => `${label}: ${say(a)} → ${say(b)}`, revert: undo });
  }
</script>

<div class="row" class:disabled>
  {#if icon}<span class="lead"><Icon name={icon} size={24} /></span>
  {:else if name}<span class="name">{name}</span>{/if}
  <!-- the knob's centre runs 10 px in from either end: it never hangs off the rail -->
  <div class="track" style="--f:{frac}"
       use:slide={{ at: (f) => set(valueAt(f, min, max, step)), end: commit, disabled, inset: 10 }}>
    <div class="rail"></div>
    <div class="fill"></div>
    <div class="knob"></div>
    <input
      type="range"
      {min}
      {max}
      {step}
      {value}
      {disabled}
      aria-label={label}
      aria-valuetext={shown || null}
      on:input={(e) => set(Number(e.target.value))}
      on:change={commit}
    />
  </div>
  {#if shown !== ''}<span class="val">{shown}</span>{/if}
</div>

<style>
  /* the name, the icon and the number keep the track off the screen's edges: a finger that
     scrolls has somewhere to go that sets nothing */
  .row { display: flex; align-items: center; gap: 16px; min-height: 48px; min-width: 0; }
  .lead { display: flex; color: var(--on-surface-var); flex: none; }
  .name {
    min-width: 70px;
    max-width: 40%;
    font-size: 16px;
    flex: none;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .track { position: relative; flex: 1; height: 40px; min-width: 0; touch-action: pan-y; cursor: pointer; }
  .rail, .fill { position: absolute; left: 0; top: 17px; height: 6px; border-radius: 3px; }
  .rail { right: 0; background: var(--surface-highest); }
  .fill { width: calc(10px + var(--f) * (100% - 20px)); background: var(--primary); }
  .knob {
    position: absolute;
    top: 10px;
    left: calc(var(--f) * (100% - 20px));
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--primary);
  }
  .val {
    min-width: 44px;
    text-align: right;
    font-size: 15px;
    color: var(--on-surface-var);
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
    flex: none;
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
  .track:has(input:focus-visible) .knob { outline: 2px solid var(--primary); outline-offset: 2px; }
  .disabled { opacity: 0.38; }
  .disabled .track { cursor: default; }
</style>
