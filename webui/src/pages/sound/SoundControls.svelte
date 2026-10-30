<script>
  // The daily sound: the presets, Бас and
  // Високі, the loudness and the way to the equalizer. One component in two places — the Player's
  // sheet and Налаштування › Звук (`page`), of which the sheet is a shortcut to the top. The
  // presets are the EQ document's (lib/eq.js), so a choice here is the panel's at once, and the
  // other way round; they are a dropdown (SoundPick), as on the equalizer's page. No captions,
  // resets or notes beyond what a control needs (the customer, 15–16.10.2026).
  import { onMount, onDestroy } from 'svelte';
  import { state } from '../../stores/state.js';
  import { t, fmt } from '../../stores/i18n.js';
  import { href } from '../../stores/router.js';
  import { createEq } from '../../lib/eq.js';
  import { soundName } from '../../lib/sounds.js';
  import { createSettingSender } from '../../lib/settings.js';
  import { toastError, toastWarn } from '../../stores/toast.js';
  import Switch from '../../m3/Switch.svelte';
  import CenterSlider from '../../m3/CenterSlider.svelte';
  import Button from '../../m3/Button.svelte';
  import Icon from '../../m3/Icon.svelte';
  import Slider from '../../m3/Slider.svelte';
  import Dialog from '../../m3/Dialog.svelte';
  import SoundPick from './SoundPick.svelte';

  /** Налаштування › Звук: the volume's section too. */
  export let page = false;
  /** A link here leaves the sheet: the sheet closes. */
  export let onLeave = () => {};

  const eq = createEq({
    onConflict: () => toastWarn($t['eq.conflict']),
    // the presets could not be read: say so with another try, not «Завантаження…» for ever
    onError: (e) => (!$doc ? (loadFailed = true) : toastError(e.message)),
  });
  let loadFailed = false;
  function retry() {
    loadFailed = false;
    eq.load();
  }
  const { doc } = eq;
  $: s = $state;
  $: d = $doc;
  // the panel or another page moved the document: take its version
  $: if (d) eq.follow(s['audio.eq_rev']);

  const bass = createSettingSender('audio.bass_db', { debounceMs: 120, live: true });
  const treble = createSettingSender('audio.treble_db', { debounceMs: 120, live: true });
  const loudness = createSettingSender('audio.loudness', { debounceMs: 0 });
  const eqEnabled = createSettingSender('audio.eq_enabled', { debounceMs: 0 });
  // the volume's two (S4), on the page only
  const volMaxS = createSettingSender('audio.vol_max', { debounceMs: 150, live: true });
  const volStartS = createSettingSender('audio.vol_start', { debounceMs: 0 });
  const START_STEPS = [0, 10, 20, 30, 40, 50];     // 0: as it was
  let startOpen = false;
  // «Вирівнювання гучності» by the clock, on the page only; the switch itself is the Player's moon
  const levSchedS = createSettingSender('audio.lev_sched', { debounceMs: 0 });
  const levFromS = createSettingSender('audio.lev_from', { debounceMs: 0 });
  const levToS = createSettingSender('audio.lev_to', { debounceMs: 0 });

  $: eqOn = s['audio.eq_enabled'] ?? true;
  $: bassDb = s['audio.bass_db'] ?? 0;
  $: trebleDb = s['audio.treble_db'] ?? 0;
  $: loud = s['audio.loudness'] ?? 70;
  $: volMax = s['audio.vol_max'] ?? 100;
  $: volStart = s['audio.vol_start'] ?? 0;
  $: startText = volStart ? fmt($t['sound.vol_start_n'], { n: volStart }) : $t['sound.vol_start_any'];
  $: levSched = !!s['audio.lev_sched'];
  $: levFrom = s['audio.lev_from'] ?? 22 * 60;
  $: levTo = s['audio.lev_to'] ?? 7 * 60;

  // minutes of the day ↔ the time field's "HH:MM"; the manifest's step is 5 minutes
  const hhmm = (m) => `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;
  function sendTime(sender, value) {
    const [h, m] = String(value).split(':').map(Number);
    if (!Number.isFinite(h) || !Number.isFinite(m)) return;
    sender.send(Math.min(1435, Math.round((h * 60 + m) / 5) * 5));
  }
  $: eqValue = !eqOn ? $t['sys.eq_off'] : d ? soundName($t, d.preset) : '';

  // The loudness off writes 0; the strength it had is kept in this browser and comes back with it
  // on (as the Player's chip did before S3).
  const LOUD_KEY = 'p4-loudness';
  function toggleLoudness(on) {
    if (!on) {
      try { localStorage.setItem(LOUD_KEY, String(loud)); } catch (e) { /* storage blocked */ }
      loudness.send(0);
      return;
    }
    let back = 70;
    try { back = Number(localStorage.getItem(LOUD_KEY)) || 70; } catch (e) { /* storage blocked */ }
    loudness.send(back);
  }

  function hidden() {
    if (document.visibilityState === 'hidden') eq.keep();
  }
  onMount(() => {
    eq.load();
    document.addEventListener('visibilitychange', hidden);
    window.addEventListener('pagehide', eq.keep);
  });
  onDestroy(() => {
    document.removeEventListener('visibilitychange', hidden);
    window.removeEventListener('pagehide', eq.keep);
    eq.keep();                      // a sound chosen here is written as the sheet closes
    [bass, treble, loudness, eqEnabled, volMaxS, volStartS, levSchedS, levFromS, levToS].forEach((x) => x.cleanup());
  });
</script>

{#if !eqOn}
  <div class="card off" role="status">
    <Icon name="info" size={20} />
    <div class="txt">
      <p>{$t['sound.eq_off']}</p>
      <Button variant="tonal" on:click={() => eqEnabled.send(true)}>{$t['sound.eq_on']}</Button>
    </div>
  </div>
{/if}

{#if d}
  <SoundPick presets={d.presets} preset={d.preset} dim={!eqOn} on:choose={(e) => eq.choose(e.detail)} />
{:else if loadFailed}
  <p class="wait">{$t['app.load_failed']} · <button class="link" type="button" on:click={retry}>{$t['app.retry']}</button></p>
{:else}
  <p class="wait">{$t['app.loading']}</p>
{/if}

<CenterSlider label={$t['sound.bass']} value={bassDb} on:input={(e) => bass.send(e.detail)} undo={(v) => bass.send(v)} />
<CenterSlider label={$t['sound.treble']} value={trebleDb} on:input={(e) => treble.send(e.detail)} undo={(v) => treble.send(v)} />

<div class="rule"></div>
<div class="sw">
  <span class="sw-txt">{$t['eq.loudness']}</span>
  <Switch checked={loud > 0} label={$t['eq.loudness']} on:change={(e) => toggleLoudness(e.detail)} />
</div>
<div class="rule"></div>

{#if page}
  <!-- «Максимум» compresses the scale: 100 % from any client is this (S4) -->
  <h3 class="cap">{$t['sound.volume']}</h3>
  <Slider name={$t['sound.vol_max']} label={$t['sound.vol_max']} min={40} max={100} step={5} value={volMax}
          format={(v) => `${v} %`} on:input={(e) => volMaxS.send(e.detail)} undo={(v) => volMaxS.send(v)} />
  <button class="eq" type="button" on:click={() => (startOpen = true)}>
    <span class="eq-t">{$t['sound.vol_start']}</span>
    <span class="eq-v">{startText}</span>
    <Icon name="chevron_right" size={20} />
  </button>
  <div class="rule"></div>

  <h3 class="cap">{$t['sound.leveler']}</h3>
  <div class="sw">
    <span class="sw-txt">{$t['sound.lev_sched']}</span>
    <Switch checked={levSched} label={$t['sound.lev_sched']} on:change={(e) => levSchedS.send(e.detail)} />
  </div>
  {#if levSched}
    <label class="eq">
      <span class="eq-t">{$t['sound.lev_from']}</span>
      <input class="time" type="time" step="300" value={hhmm(levFrom)} on:change={(e) => sendTime(levFromS, e.currentTarget.value)} />
    </label>
    <label class="eq">
      <span class="eq-t">{$t['sound.lev_to']}</span>
      <input class="time" type="time" step="300" value={hhmm(levTo)} on:change={(e) => sendTime(levToS, e.currentTarget.value)} />
    </label>
  {/if}
  <div class="rule"></div>
{/if}

{#if startOpen}
  <Dialog title={$t['sound.vol_start']} on:close={() => (startOpen = false)}>
    <div class="opts" role="radiogroup" aria-label={$t['sound.vol_start']}>
      {#each START_STEPS as n (n)}
        <button type="button" role="radio" class="opt" class:on={volStart === n} aria-checked={volStart === n}
                on:click={() => { volStartS.send(n); startOpen = false; }}>
          {n ? fmt($t['sound.vol_start_n'], { n }) : $t['sound.vol_start_any']}
        </button>
      {/each}
    </div>
  </Dialog>
{/if}
<a class="eq" href={href('system/sound/eq')} on:click={onLeave}>
  <span class="eq-t">{$t['page.eq']}</span>
  <span class="eq-v">{eqValue}</span>
  <Icon name="chevron_right" size={20} />
</a>

<style>
  /* as a settings group's caption (m3/Group): sentence case, the panel's style */
  .cap {
    font-size: 14px;
    line-height: 20px;
    font-weight: 500;
    color: var(--on-surface-var);
    margin: 16px 0 8px;
  }
  .wait { color: var(--on-surface-var); line-height: 56px; margin: 0; }
  .rule { height: 1px; background: var(--outline-var); opacity: 0.6; margin: 8px 0; }
  .sw { display: flex; align-items: center; gap: 16px; min-height: 64px; }
  .sw-txt { flex: 1; font-size: 16px; }
  .opts { display: flex; flex-direction: column; }
  .opt {
    min-height: 48px;
    border: 0;
    border-radius: var(--r-s);
    background: none;
    color: var(--on-surface);
    font: inherit;
    font-size: 16px;
    text-align: left;
    padding: 0 12px;
  }
  .opt.on { background: var(--secondary-container); color: var(--on-secondary-container); }
  .eq {
    display: flex;
    align-items: center;
    gap: 12px;
    min-height: 56px;
    width: 100%;
    color: var(--on-surface);
    text-decoration: none;
    border: 0;
    background: none;
    padding: 0;
    font: inherit;
    text-align: left;
  }
  .eq-t { flex: 1; font-size: 16px; }
  .eq-v { font-size: 15px; color: var(--on-surface-var); }
  .time {
    min-height: 40px;
    padding: 0 12px;
    border: 1px solid var(--outline-var);
    border-radius: var(--r-s);
    background: none;
    color: var(--on-surface);
    font: inherit;
    font-size: 16px;
    color-scheme: light dark;
  }
  .eq :global(.ms) { color: var(--on-surface-var); }
  .card {
    display: flex;
    gap: 12px;
    padding: 12px 16px;
    border-radius: var(--r-s);
    background: var(--surface-high);
    margin-bottom: 8px;
  }
  .card .txt { display: flex; flex-direction: column; gap: 8px; align-items: flex-start; font-size: 15px; line-height: 20px; }
  .link { border: 0; background: none; padding: 0; font: inherit; color: var(--primary); cursor: pointer; }
</style>
