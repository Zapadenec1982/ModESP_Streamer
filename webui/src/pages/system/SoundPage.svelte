<script>
  // Налаштування › Звук › Тембр і гучність
  // (named for what it holds, the customer, 18.10.2026 — the glass's page, without the volume's
  // two, is «Тембр»): the sounds, the timbre, and the equalizer for enthusiasts. The Player's
  // sheet is a shortcut to the top of this page — the same component — and the volume's two
  // below. The usual volume is a slider in Налаштування › Тонке налаштування.
  import SoundControls from '../sound/SoundControls.svelte';
</script>

<div class="page">
  <SoundControls page />
</div>

<style>
  .page {
    max-width: 640px;
    padding: 0 0 24px;
  }
</style>
