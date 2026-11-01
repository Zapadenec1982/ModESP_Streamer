<script>
  // What the test tone is doing, held under the top bar while a page scrolls: the frequency, the
  // level, the channel, how long until the device turns a calibration tone off by itself — and
  // the button that stops it now. The shell holds it: a tone left playing on
  // the way out of calibration, or started by another client, shows on every page, and a tone
  // through the DSP never turns itself off.
  import { onDestroy } from 'svelte';
  import { t, fmt, language } from '../stores/i18n.js';
  import { state } from '../stores/state.js';
  import { toastError } from '../stores/toast.js';
  import { toneOf, calClock, stopTone, CHAIN, LEFT, RIGHT, CAL_MAX_S } from '../lib/tone.js';
  import Icon from '../m3/Icon.svelte';
  import Button from '../m3/Button.svelte';

  let now = Date.now();
  const timer = setInterval(() => (now = Date.now()), 1000);
  onDestroy(() => clearInterval(timer));

  $: tone = toneOf($state);
  $: cal = tone.on && tone.mode !== CHAIN;
  $: left = $calClock.since ? Math.max(0, CAL_MAX_S - Math.floor((now - $calClock.since) / 1000)) : 0;
  $: db = (tone.db10 / 10).toLocaleString($language, { minimumFractionDigits: 1, maximumFractionDigits: 1 }).replace('-', '−');
  $: ch = tone.ch === LEFT ? $t['cal.ch_left'] : tone.ch === RIGHT ? $t['cal.ch_right'] : $t['cal.ch_both'];
  $: what = cal
    ? [`${tone.hz} ${$t['unit.hz']}`, `${db} ${$t['unit.dbfs']}`, ch].join(' · ')
    : [`${tone.hz} ${$t['unit.hz']}`, $t['cal.tone_chain']].join(' · ');
  $: until = !cal ? '' : $calClock.exact ? fmt($t['cal.tone_left'], { s: left }) : fmt($t['cal.tone_max'], { s: CAL_MAX_S });

  async function stop() {
    try { await stopTone(); } catch (e) { toastError(e.message); }
  }
</script>

{#if tone.on}
  <div class="bar" role="status">
    <span class="ic"><Icon name="graphic_eq" /></span>
    <span class="txt">
      <span class="h">{[$t['cal.tone_on'], until].filter(Boolean).join(' · ')}</span>
      <span class="s">{what}</span>
    </span>
    <Button variant="tonal" icon="stop" on:click={stop}>{$t['cal.tone_stop']}</Button>
  </div>
{/if}

<style>
  .bar {
    position: sticky;
    top: var(--topbar-h);
    z-index: 40;
    display: flex;
    align-items: center;
    gap: 12px;
    margin: 0 0 8px;
    padding: 8px 8px 8px 14px;
    border-radius: var(--r-l);
    background: var(--primary-container);
    color: var(--on-primary-container);
    box-shadow: 0 6px 16px rgb(0 0 0 / 30%);
  }
  .ic { display: flex; flex: none; }
  .txt { flex: 1; min-width: 0; display: flex; flex-direction: column; }
  .h { font-size: 15px; line-height: 20px; font-weight: 500; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .s {
    font-size: 14px;
    line-height: 20px;
    opacity: 0.85;
    font-variant-numeric: tabular-nums;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
