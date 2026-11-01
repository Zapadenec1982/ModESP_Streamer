<script>
  // Налаштування › Звук › Еквалайзер: the
  // equalizer for enthusiasts. The switch; the sounds as a dropdown (SoundPick; chips until the
  // customer's 15.10.2026); the curve with a point per band that is on; the bands as a list, each
  // opened in a sheet; «Скинути «Власний»» for «Власний» alone — a genre never changes: an edit
  // of one makes it «Власний», and the page says so;
  // filters in and out as Equalizer APO / REW text; the whole system's curve on request. Under it,
  // bass and stereo, and the way to the speakers and the amplifier. The limiter is Service's.
  //
  // The EQ is one document on the device, edited from here and from the panel (lib/eq.js). An edit
  // is heard at once; the device writes it a minute after the last one, or at once when this page
  // is left. A change made on the panel meanwhile shows here within a WebSocket tick.
  import { onMount, onDestroy } from 'svelte';
  import { t, K, fmt, language } from '../stores/i18n.js';
  import { decimal } from '../lib/numbers.js';
  import { state } from '../stores/state.js';
  import { createSettingSender } from '../lib/settings.js';
  import { toastError, toastWarn, toastAction, noteUndo } from '../stores/toast.js';
  import { soundName } from '../lib/sounds.js';
  import { port } from '../lib/speakers.js';
  import { createEq } from '../lib/eq.js';
  import { addBand, resetBands, MAX_BANDS } from '../lib/eqband.js';
  import { hasGain } from '../lib/eqmath.js';
  import { formatFilters } from '../lib/eqfile.js';
  import { systemBands } from '../lib/syscurve.js';
  import Switch from '../m3/Switch.svelte';
  import Icon from '../m3/Icon.svelte';
  import IconButton from '../m3/IconButton.svelte';
  import Button from '../m3/Button.svelte';
  import EqPlot from './dsp/EqPlot.svelte';
  import BandSheet from './dsp/BandSheet.svelte';
  import ImportDialog from './dsp/ImportDialog.svelte';
  import SoundPick from './sound/SoundPick.svelte';

  const TYPE_KEYS = {
    peak: K('eq.type.peak'),
    lowshelf: K('eq.type.lowshelf'),
    highshelf: K('eq.type.highshelf'),
    lowpass: K('eq.type.lowpass'),
    highpass: K('eq.type.highpass'),
    notch: K('eq.type.notch'),
  };

  let failed = '';
  const eq = createEq({
    onConflict: () => toastWarn($t['eq.conflict']),
    onError: (e) => {
      if (!$doc) failed = e.message;
      else toastError(e.message);
    },
  });
  const { doc } = eq;

  $: s = $state;
  $: d = $doc;
  $: bands = d ? d.bands : [];
  $: eqOn = s['audio.eq_enabled'] ?? true;
  // the panel edited it: take its version (unless an edit of ours is on its way)
  $: if (d) eq.follow(s['audio.eq_rev']);
  $: mine = d?.preset === 'custom';
  // every band by its place, on or off, as the glass has them
  $: listed = bands.map((b, i) => ({ i, b }));
  $: full = bands.length >= MAX_BANDS;
  $: anyOn = bands.some((b) => b.on);

  const eqEnabled = createSettingSender('audio.eq_enabled', { debounceMs: 0 });

  // ── an edit by hand: on a genre the result is «Власний» (eq_hand_edited); the field says so ──
  function byHand(next) {
    eq.setBands(next);
  }
  function setBand(i, b) {
    const next = bands.slice();
    next[i] = b;
    byHand(next);
  }

  // ── a band in its sheet ──
  let open = -1;
  $: if (open >= 0 && !bands[open]) open = -1;            // a shorter set came from the panel
  let sel = 0;
  $: if (open >= 0) sel = open;
  function add() {
    const r = addBand(bands);
    if (!r) return;
    byHand(r.bands);
    open = r.i;
  }
  // «Увімкнена» in the sheet: on or off, its numbers kept — the switch is its own way back
  const turn = (i, on) => setBand(i, { ...bands[i], on });

  // ── a point moved on the plot can be put back from a snackbar (stores/toast.js): where it was,
  //    and the sound it was then — «Скасувати» chooses a genre the drag made «Власний» again ──
  $: fText = (f) => (f >= 1000 ? `${decimal(f / 1000, 2, $language)} ${$t['unit.khz']}` : `${f.toFixed(0)} ${$t['unit.hz']}`);
  $: gText = (b) => (hasGain(b.type) ? `${b.gain > 0 ? '+' : ''}${decimal(b.gain, 1, $language)} ${$t['unit.db']}` : '—');
  let grabbed = null;
  function grab(i) {
    grabbed = { i, from: { band: { ...bands[i] }, preset: d.preset } };
  }
  function release(i) {
    const g = grabbed;
    grabbed = null;
    if (!g || g.i !== i || !bands[i]) return;
    const say = (b) => (hasGain(b.type) ? `${fText(b.f)} ${gText(b)}` : fText(b.f));
    noteUndo({
      key: `eq.band.${i}`,
      from: g.from,
      to: { band: { ...bands[i] }, preset: d.preset },
      action: $t['undo.action'],
      text: (a, b) => `${fmt($t['eq.band_n'], { n: i + 1 })}: ${say(a.band)} → ${say(b.band)}`,
      revert: (a) => (a.preset !== 'custom' ? eq.choose(a.preset) : bands[i] && setBand(i, a.band)),
    });
  }

  // ── «Скинути «Власний»»: every band off, «Повернути» puts them back ──
  function reset() {
    const was = bands.slice();
    byHand(resetBands(bands));
    toastAction($t['eq.reset_done'], $t['sound.undo'], () => eq.setBands(was));
  }

  // ── filters in and out (lib/eqfile.js) ──
  let importing = false;
  function imported(e) {
    importing = false;
    const was = { preset: d.preset, bands: bands.slice() };
    eq.setBands(e.detail);
    toastAction($t['eq.imported'], $t['sound.undo'],
      () => (was.preset !== 'custom' ? eq.choose(was.preset) : eq.setBands(was.bands)));
  }
  function exportSound() {
    menu = false;
    const text = formatFilters(bands, `P4 Player - ${soundName($t, d.preset)}`);
    const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = `p4-eq-${d.preset}.txt`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  // ── the menu ──
  let menu = false;
  let menuEl;
  function outside(e) {
    if (menu && menuEl && !menuEl.contains(e.target)) menu = false;
  }

  // ── the whole system (lib/syscurve.js): the stages around the EQ, the loudness as it is now
  //    (audio.loud_db: published on every change, so the WebSocket's deltas carry it) ──
  let showSystem = false;
  $: p = port(s);
  $: stages = showSystem ? systemBands({
    bass: s['audio.bass_db'] ?? 0,
    treble: s['audio.treble_db'] ?? 0,
    loudDb: Number(s['audio.loud_db']) || 0,
    hpHz: s['audio.hp_hz'] ?? 40,
    portHz: p.hz,
    liftDb: s['audio.port_lift_db'] ?? 3,
  }) : null;

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
    eq.keep();
    eqEnabled.cleanup();
  });
</script>

<svelte:window on:click={outside} />

{#if !d}
  {#if failed}
    <!-- not the device's error text and a dead end: what happened, and another try -->
    <p class="msg">{$t['app.load_failed']}</p>
    <div class="again"><Button variant="tonal" icon="refresh" on:click={() => { failed = ''; eq.load(); }}>{$t['app.retry']}</Button></div>
  {:else}
    <p class="msg">{$t['app.loading']}</p>
  {/if}
{:else}
<div class="page">
  <div class="bar">
    <Switch checked={eqOn} label={$t['page.eq']} on:change={(e) => eqEnabled.send(e.detail)} />
    <span class="state">{eqOn ? $t['eq.enabled'] : $t['eq.disabled']}</span>
    <div class="menuwrap" bind:this={menuEl}>
      <IconButton icon="more_vert" label={$t['eq.menu']} on:click={() => (menu = !menu)} />
      {#if menu}
        <div class="menu" role="menu">
          <button type="button" role="menuitem" on:click={() => { menu = false; importing = true; }}>
            <Icon name="upload_file" size={20} />{$t['eq.import']}
          </button>
          <button type="button" role="menuitem" disabled={!listed.length} on:click={exportSound}>
            <Icon name="download" size={20} />{$t['eq.export']}
          </button>
          <button type="button" role="menuitemcheckbox" aria-checked={showSystem}
                  on:click={() => { menu = false; showSystem = !showSystem; }}>
            <Icon name="graphic_eq" size={20} />{$t['eq.system']}
            {#if showSystem}<span class="tick"><Icon name="check" size={20} /></span>{/if}
          </button>
        </div>
      {/if}
    </div>
  </div>

  <SoundPick presets={d.presets} preset={d.preset} dim={!eqOn} on:choose={(e) => eq.choose(e.detail)} />

  <div class:dim={!eqOn}>
    <EqPlot {bands} fs={d.fs} {sel} on={eqOn} label={$t['eq.curve']} {stages}
            on:select={(e) => (sel = e.detail)}
            on:edit={(e) => setBand(e.detail.i, e.detail.band)}
            on:grab={(e) => grab(e.detail)} on:release={(e) => release(e.detail)} />
  </div>
  {#if showSystem}<p class="sysnote">{$t['eq.system_note']}</p>{/if}

  <div class="list" class:dim={!eqOn}>
    {#each listed as x (x.i)}
      <button type="button" class="band" class:off={!x.b.on} on:click={() => (open = x.i)}>
        <span class="n">{x.i + 1}</span>
        <span class="what"><span>{$t[TYPE_KEYS[x.b.type]]}</span><span class="f">{fText(x.b.f)}</span></span>
        <span class="g">{x.b.on ? gText(x.b) : $t['eq.off']}</span>
        <Icon name="chevron_right" size={20} />
      </button>
    {:else}
      <p class="empty">{$t['eq.no_bands']}</p>
    {/each}
    {#if !full}<button type="button" class="add" on:click={add}>{$t['eq.add']}</button>{/if}
  </div>

  {#if mine}
    <div class="foot">
      <Button variant="text" disabled={!anyOn} on:click={reset}>{$t['eq.reset_mine']}</Button>
    </div>
  {/if}


  {#if open >= 0 && bands[open]}
    <BandSheet band={bands[open]} n={open + 1}
               on:edit={(e) => setBand(open, e.detail)} on:turn={(e) => turn(open, e.detail)} on:close={() => (open = -1)} />
  {/if}
  {#if importing}
    <ImportDialog on:import={imported} on:close={() => (importing = false)} />
  {/if}
</div>
{/if}

<style>
  .msg { text-align: center; color: var(--on-surface-var); padding: 32px 16px; }
  .page { max-width: 960px; }
  .bar { display: flex; align-items: center; gap: 12px; min-height: 52px; }
  .state { flex: 1; font-size: 15px; color: var(--on-surface-var); }
  .menuwrap { position: relative; }
  .menu {
    position: absolute;
    top: 46px;
    right: 0;
    z-index: 60;
    min-width: 250px;
    padding: 6px 0;
    border-radius: var(--r-xs);
    background: var(--surface-container);
    box-shadow: 0 8px 24px rgb(0 0 0 / 50%);
    display: flex;
    flex-direction: column;
  }
  .menu button {
    display: flex;
    align-items: center;
    gap: 12px;
    min-height: 48px;
    padding: 0 16px;
    border: 0;
    background: none;
    color: var(--on-surface);
    font: inherit;
    font-size: 15px;
    text-align: left;
  }
  .menu button:hover { background: color-mix(in srgb, currentColor 8%, transparent); }
  .menu button:disabled { opacity: 0.38; }
  .menu :global(.ms) { color: var(--on-surface-var); }
  .tick { margin-left: auto; color: var(--primary); display: flex; }
  .tick :global(.ms) { color: var(--primary); }

  .dim { opacity: 0.45; }
  .sysnote { font-size: 13px; line-height: 18px; color: var(--on-surface-var); margin: 6px 4px 0; }

  .list {
    margin-top: 12px;
    background: var(--surface-low);
    border-radius: var(--r-l);
    overflow: hidden;
  }
  .band {
    width: 100%;
    display: flex;
    align-items: center;
    gap: 14px;
    min-height: 60px;
    padding: 6px 10px 6px 16px;
    border: 0;
    border-bottom: 1px solid var(--outline-var);
    background: none;
    color: var(--on-surface);
    font: inherit;
    text-align: left;
  }
  .band :global(.ms) { color: var(--outline); }
  .n {
    width: 28px;
    height: 28px;
    border-radius: 14px;
    flex: none;
    display: flex;
    align-items: center;
    justify-content: center;
    background: var(--secondary-container);
    color: var(--on-secondary-container);
    font-size: 13px;
    font-weight: 600;
  }
  .what { flex: 1; min-width: 0; display: flex; flex-direction: column; font-size: 16px; }
  .what .f { font-size: 13px; color: var(--on-surface-var); }
  .g { font-size: 16px; font-variant-numeric: tabular-nums; }
  /* a band that is off: its number hollow and its words faint, as its point on the plot */
  .band.off .n { background: none; box-shadow: inset 0 0 0 2px var(--outline); color: var(--outline); }
  .band.off .what, .band.off .g { color: var(--outline); }
  .band.off .what .f { color: var(--outline); }
  .empty { padding: 16px 16px 4px; font-size: 14px; line-height: 20px; color: var(--on-surface-var); }
  .add {
    width: 100%;
    min-height: 52px;
    padding: 0 16px;
    border: 0;
    background: none;
    color: var(--primary);
    font: inherit;
    font-size: 15px;
    font-weight: 500;
    text-align: left;
  }
  .add:disabled { color: var(--outline); }
  .foot { min-height: 48px; display: flex; align-items: center; justify-content: center; margin: 6px 0 4px; }

  .again { display: flex; justify-content: center; }
</style>
