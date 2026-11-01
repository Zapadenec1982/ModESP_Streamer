<script>
  // The sounds as a dropdown, not chips. The Player's sheet, Налаштування › Звук and the
  // equalizer have it, as the glass does (sound_sheet.cpp ui_sound_field). `presets` are the ones
  // offered while `preset` plays (GET /api/eq "presets": the eight, and a kept one while it
  // plays); a choice is the host's to make (`choose`).
  import { createEventDispatcher } from 'svelte';
  import { t } from '../../stores/i18n.js';
  import { soundName } from '../../lib/sounds.js';

  export let presets = [];
  export let preset = '';
  /** The EQ is off: still a choice, shown faded. */
  export let dim = false;

  const dispatch = createEventDispatcher();
</script>

<div class="field" class:dim>
  <span class="name">{$t['sound.styles']}</span>
  <select class="pick" aria-label={$t['sound.styles']} value={preset}
          on:change={(e) => dispatch('choose', e.target.value)}>
    {#each presets as id (id)}<option value={id}>{soundName($t, id)}</option>{/each}
  </select>
</div>

<style>
  .field { display: flex; align-items: center; gap: 16px; min-height: 56px; }
  .name { flex: 1; min-width: 0; font-size: 16px; color: var(--on-surface); }
  .pick {
    height: 40px;
    max-width: 60%;
    border: 1px solid var(--outline-var);
    border-radius: 20px;
    padding: 0 12px;
    background: var(--secondary-container);
    color: var(--on-secondary-container);
    font: inherit;
    font-size: 15px;
    font-weight: 500;
  }
  .pick option { background: var(--surface-container); color: var(--on-surface); }
  .dim { opacity: 0.45; }
</style>
