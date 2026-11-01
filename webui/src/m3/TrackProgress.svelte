<script>
  // A track's position, as on the panel (screen_now.cpp): a 4 px line, primary up to where the
  // track is, m:ss under both ends. Where seeking exists, a knob at the fill's end says so, a
  // finger drags it along — a press or a finger that goes up or down does
  // not (lib/gesture.js) — over a target 40 px tall that the layout does not see, and a native
  // range input under it is for the keyboard and screen readers; elsewhere a read-out. `seek`
  // fires with seconds on release.
  import { createEventDispatcher } from 'svelte';
  import { slide } from '../lib/gesture.js';

  export let pos = 0;
  export let dur = 0;
  export let seekable = false;
  export let label = '';

  const dispatch = createEventDispatcher();
  let dragging = null;

  const mmss = (s) => {
    s = Math.max(0, Math.floor(s || 0));
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const ss = String(s % 60).padStart(2, '0');
    return h ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${m}:${ss}`;
  };

  $: shown = dragging ?? pos;
  const seek = (v) => { dispatch('seek', v); dragging = null; };
  $: frac = dur > 0 ? Math.min(1, shown / dur) : 0;
</script>

<div class="prog" class:seekable>
  <div class="hit" use:slide={{ disabled: !(seekable && dur > 0), at: (f) => (dragging = Math.round(f * dur)),
                                end: () => dragging !== null && seek(dragging) }}>
  <div class="track"><div class="fill" style="width:{frac * 100}%"></div></div>
  {#if seekable && dur > 0}
    <span class="knob" style="left:clamp(8px, {frac * 100}%, calc(100% - 8px))"></span>
    <input
      type="range"
      min="0"
      max={dur}
      step="1"
      value={shown}
      aria-label={label}
      aria-valuetext={`${mmss(shown)} / ${mmss(dur)}`}
      on:input={(e) => (dragging = Number(e.target.value))}
      on:change={(e) => seek(Number(e.target.value))}
    />
  {/if}
  </div>
  <div class="times">
    <span>{mmss(shown)}</span>
    <span class="dur">{dur > 0 ? mmss(dur) : ''}</span>
  </div>
</div>

<style>
  .prog { position: relative; }
  /* the 40 px a finger takes, 24 of them in the layout: the rest reaches over the gap above and
     the times below, which let it through */
  .hit { position: relative; padding: 18px 0; margin: -8px 0; }
  .seekable .hit { touch-action: pan-y; cursor: pointer; }
  .track {
    height: 4px;
    border-radius: 2px;
    background: var(--surface-highest);
    overflow: hidden;
  }
  .fill { height: 100%; background: var(--primary); border-radius: 2px; }
  .knob {
    position: absolute;
    top: 50%;
    width: 16px;
    height: 16px;
    border-radius: 50%;
    background: var(--primary);
    transform: translate(-50%, -50%);
    pointer-events: none;
  }
  input {
    position: absolute;
    left: 0;
    right: 0;
    top: 8px;
    width: 100%;
    height: 24px;
    margin: 0;
    opacity: 0;
    pointer-events: none;
  }
  .prog:has(input:focus-visible) .track { outline: 2px solid var(--primary); outline-offset: 4px; }
  .times {
    position: relative;
    pointer-events: none;
    display: flex;
    justify-content: space-between;
    margin-top: -4px;
    font-size: 13px;
    line-height: 18px;
    font-weight: 500;
    color: var(--on-surface-var);
    font-variant-numeric: tabular-nums;
  }
  .dur { color: var(--outline); }
</style>
