<script>
  // One step of a calibration task: its number, a short title, a line or two of what to do, and
  // the controls the step needs — nothing a later step needs.
  export let n = '';
  export let title = '';
  export let done = false;
</script>

<section class="step" class:done>
  <span class="n" aria-hidden="true">{n}</span>
  <div class="body">
    <h3 class="t">{title}</h3>
    {#if $$slots.text}<p class="x"><slot name="text" /></p>{/if}
    <slot />
  </div>
</section>

<style>
  .step {
    display: flex;
    gap: 14px;
    padding: 14px 14px 16px 12px;
    background: var(--surface-low);
    border-radius: var(--r-l);
  }
  .n {
    flex: none;
    width: 28px;
    height: 28px;
    border-radius: 14px;
    background: var(--secondary-container);
    color: var(--on-secondary-container);
    font-size: 14px;
    font-weight: 600;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .done .n { background: var(--primary); color: var(--on-primary); }
  .body { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 10px; }
  .t { font-size: 16px; line-height: 22px; font-weight: 500; color: var(--on-surface); padding-top: 3px; }
  .x { font-size: 15px; line-height: 22px; color: var(--on-surface-var); margin-top: -4px; }
</style>
