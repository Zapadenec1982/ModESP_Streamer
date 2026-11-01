<script>
  // A value with − and + at its ends, as on the panel's DSP band panel (screen_eq.cpp), so a
  // thumb aimed at one cannot land on the other. The parent owns the number and
  // its step; this only shows `text` and reports `dec` / `inc`.
  import { createEventDispatcher } from 'svelte';
  import IconButton from './IconButton.svelte';

  export let caption = '';
  export let text = '';
  export let decLabel;
  export let incLabel;
  export let disabled = false;

  const dispatch = createEventDispatcher();
</script>

<div class="step" class:disabled>
  <span class="body">
    {#if caption}<span class="cap">{caption}</span>{/if}
    <span class="val">{text}</span>
  </span>
  <IconButton icon="remove" label={decLabel} {disabled} on:click={() => dispatch('dec')} />
  <IconButton icon="add" label={incLabel} {disabled} on:click={() => dispatch('inc')} />
</div>

<style>
  .step {
    background: var(--surface-container);
    border-radius: var(--r-m);
    display: flex;
    align-items: center;
    gap: 2px;
    min-height: 56px;
    padding: 4px 4px 4px 16px;
    min-width: 0;
  }
  .body { flex: 1; min-width: 0; display: flex; flex-direction: column; }
  .cap { font-size: 13px; line-height: 18px; color: var(--on-surface-var); }
  .val { font-size: 17px; line-height: 22px; white-space: nowrap; }
  .disabled .body { opacity: 0.5; }
</style>
