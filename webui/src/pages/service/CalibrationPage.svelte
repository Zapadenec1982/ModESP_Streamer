<script>
  // The speakers' port (docs/AUDIO.md, "Speaker calibration: port frequency"): found step by step —
  // by the panel's microphone or by hand — each step a line of text and a button that sets the tone
  // that step needs; the page's title says what it is for, Колонки what was found. No tone
  // controls outside the steps; the settings the port sets are Налаштування › Тонке налаштування
  // (TuningPage). The Speakers wizard's «Знайти вручну» comes here.
  //
  // The amplifier's ceiling has no task here: «Максимум» is the one limit a person sets, and the
  // ceiling stays the datasheet's (audio.amp_clip_db10, −9.8 dBFS), a setting the API still
  // changes. What a person chose here stays in this tab (sessionStorage): a phone that left
  // the page for its spectrum app comes back to the same step. The tone bar is the shell's: a tone
  // shows on every page.
  import { onDestroy } from 'svelte';
  import { t, fmt, msg } from '../../stores/i18n.js';
  import { state } from '../../stores/state.js';
  import { live } from '../../lib/live.js';
  import { createSettingSender, writeNow } from '../../lib/settings.js';
  import { toastError, toastSuccess } from '../../stores/toast.js';
  import { playTone, stopTone, CHAIN } from '../../lib/tone.js';
  import { portWrites, meterOutcome } from '../../lib/speakers.js';
  import Chip from '../../m3/Chip.svelte';
  import Button from '../../m3/Button.svelte';
  import NumField from '../dsp/NumField.svelte';
  import Step from './Step.svelte';

  // ── what was chosen, kept for this tab ──
  const KEY = 'p4-calibration';
  const BLANK = { portHow: 'mic', hz: 40 };
  let w = { ...BLANK };
  try { w = { ...BLANK, ...JSON.parse(sessionStorage.getItem(KEY) || '{}') }; } catch (e) { /* storage blocked */ }
  function keep(v) {
    try { sessionStorage.setItem(KEY, JSON.stringify(v)); } catch (e) { /* storage blocked */ }
  }
  $: keep(w);

  $: s = $state;
  // read-only keys the device writes without a WebSocket delta
  $: ro = (k) => ($live[k] !== undefined ? $live[k] : s[k]);

  async function act(fn) {
    try { await fn(); } catch (e) { toastError(e.message); }
  }

  // ── the port ──
  $: measuring = !!s['audio.port_meas'];
  $: measCh = s['audio.port_meas_ch'] ?? 1;
  $: measHz = Number(ro('audio.port_meas_hz')) || 0;
  $: measSays = msg($t, ro('audio.port_meas_status') || '');   // a message of the catalog
  // the meter and what it found are shown at once, and taken back if the device refuses them
  const portMeas = (on) => act(() => writeNow({ 'audio.port_meas': on }));
  // The meter offers what it found and writes nothing: «Застосувати» writes it, as measured.
  $: measFound = meterOutcome(ro('audio.port_meas_status'), measuring) === 'found' && measHz > 0;
  const portApply = () => act(async () => {
    const keys = portWrites(measHz, true);
    await writeNow(keys);
    toastSuccess(fmt($t['cal.port_is'], { hz: keys['audio.port_hz'] }));
  });
  const measChSend = createSettingSender('audio.port_meas_ch', { debounceMs: 0 });

  // The port off and the volume at most 20 % for as long as the tone plays: keys the device never
  // saves and drops with the tone, so closing the page mid-search changes nothing.
  function portStart() {
    act(() => playTone({ mode: CHAIN, hz: w.hz }, { extra: { 'audio.cal_port_off': true, 'audio.cal_vol_cap': 20 } }));
  }
  function portHz(hz) {
    w = { ...w, hz: Math.max(20, Math.min(100, Math.round(hz))) };
    if (s['audio.source'] === 1) act(() => playTone({ mode: CHAIN, hz: w.hz }));
  }
  // found by hand: written with where it came from, and the speakers are ported
  const portWrite = () => act(async () => {
    const keys = portWrites(w.hz, false);
    await stopTone({ restore: true, extra: keys });
    toastSuccess(fmt($t['cal.port_is'], { hz: keys['audio.port_hz'] }));
  });

  onDestroy(() => measChSend.cleanup());
</script>

<div class="cal">

<div class="chips" role="radiogroup" aria-label={$t['cal.port']}>
  <Chip button selected={w.portHow === 'mic'} on:click={() => (w = { ...w, portHow: 'mic' })}>{$t['cal.by_mic']}</Chip>
  <Chip button selected={w.portHow === 'hand'} on:click={() => (w = { ...w, portHow: 'hand' })}>{$t['cal.by_hand']}</Chip>
</div>

<div class="steps">
  {#if w.portHow === 'mic'}
    <Step n="1" title={$t['cal.s_place']}>
      <svelte:fragment slot="text">{$t['cal.s_place_x']}</svelte:fragment>
      <div class="chips">
        <Chip button selected={measCh === 1} on:click={() => measChSend.send(1)}>{$t['cal.speaker_left']}</Chip>
        <Chip button selected={measCh === 2} on:click={() => measChSend.send(2)}>{$t['cal.speaker_right']}</Chip>
      </div>
    </Step>
    <Step n="2" title={$t['cal.s_measure']} done={measHz > 0 && !measuring}>
      <svelte:fragment slot="text">{$t['cal.s_measure_x']}</svelte:fragment>
      <div class="acts">
        {#if measuring}
          <Button variant="outlined" icon="stop" on:click={() => portMeas(false)}>{$t['cal.measure_stop']}</Button>
        {:else}
          <Button icon="play_arrow" on:click={() => portMeas(true)}>{$t['cal.measure']}</Button>
        {/if}
      </div>
      <!-- the device's own words, frequency included (audio_module.cpp port meter) -->
      {#if measSays}<p class="res" role="status">{measSays}</p>{/if}
      {#if measFound}
        <div class="acts"><Button icon="save" on:click={portApply}>{fmt($t['cal.apply'], { hz: Math.round(measHz) })}</Button></div>
      {/if}
      </Step>
  {:else}
    <Step n="1" title={$t['cal.s_sweep']}>
      <svelte:fragment slot="text">{$t['cal.s_sweep_x']}</svelte:fragment>
      <div class="acts"><Button variant="tonal" icon="play_arrow" on:click={portStart}>{$t['cal.set']}</Button></div>
    </Step>
    <Step n="2" title={$t['cal.s_find']}>
      <svelte:fragment slot="text">{$t['cal.s_find_x']}</svelte:fragment>
      <div class="nudge">
        <NumField caption={$t['cal.freq']} text={`${w.hz} ${$t['unit.hz']}`} raw={w.hz}
                  decLabel={$t['cal.lower']} incLabel={$t['cal.higher']}
                  on:dec={() => portHz(w.hz - 1)} on:inc={() => portHz(w.hz + 1)} on:set={(e) => portHz(e.detail)} />
      </div>
    </Step>
    <Step n="3" title={$t['cal.s_port_write']}>
      <svelte:fragment slot="text">{$t['cal.s_port_write_x']}</svelte:fragment>
      <div class="acts"><Button icon="save" on:click={portWrite}>{fmt($t['cal.port_write'], { hz: Math.max(30, Math.min(80, w.hz)) })}</Button></div>
    </Step>
  {/if}
</div>

</div>

<style>
  /* steps read as text: a phone's column, not a desktop's width */
  .cal { max-width: 760px; margin: 0 auto; }
  .chips { display: flex; gap: 8px; padding: 4px 0 8px; }
  .steps { display: flex; flex-direction: column; gap: 8px; }
  .acts { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; }
  .nudge { max-width: 280px; }
  .res { font-size: 15px; line-height: 22px; color: var(--on-surface); font-variant-numeric: tabular-nums; }
</style>
