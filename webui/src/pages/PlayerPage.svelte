<script>
  // Player: a remote, not a second player
  // (customer, 11.10.2026). Where the sound comes from; the cover, big; what plays, its codec; the
  // transport with a big pause; a thin volume that shows its number while it moves; one «Звук»
  // button for the sheet with the sounds, Бас, Високі and the loudness (S3), and the moon beside it,
  // «Вирівнювання гучності» (audio.leveler, as on the glass). Below, what can be done
  // about the source that plays. The faces, texts and commands are the panel's (stores/now.js after
  // screen_now.cpp), and so is the background: the shell tints the page with ui.cover_rgb.
  import { onDestroy } from 'svelte';
  import { now, subLine } from '../stores/now.js';
  import { state, setStateKey } from '../stores/state.js';
  import { t, fmt, msg, plural, language, K } from '../stores/i18n.js';
  import { href } from '../stores/router.js';
  import { command } from '../lib/cmd.js';
  import { badge } from '../lib/numbers.js';
  import { apiPost, apiBlob } from '../lib/api.js';
  import { createSettingSender } from '../lib/settings.js';
  import { toastSuccess, toastError } from '../stores/toast.js';
  import Icon from '../m3/Icon.svelte';
  import IconButton from '../m3/IconButton.svelte';
  import Slider from '../m3/Slider.svelte';
  import Button from '../m3/Button.svelte';
  import Group from '../m3/Group.svelte';
  import Row from '../m3/Row.svelte';
  import WaveLine from '../m3/WaveLine.svelte';
  import TrackProgress from '../m3/TrackProgress.svelte';
  import BottomSheet from '../m3/BottomSheet.svelte';

  $: np = $now;
  $: title = np.title ?? $t[np.titleKey];
  $: sub = subLine(np, $t, $language);
  $: s = $state;

  // ── position between the panel's deltas (every 1.5 s): count on while playing. The anchor moves
  //    only when the device reports a different position or the play state turns, never because
  //    some other key changed — that would pull the bar back.
  let anchor = { pos: 0, at: Date.now() };
  let seen = '';
  let tick = Date.now();
  const timer = setInterval(() => (tick = Date.now()), 1000);
  onDestroy(() => clearInterval(timer));
  $: {
    const key = `${np.kind}|${np.pos}|${np.playing}`;
    if (key !== seen) {
      seen = key;
      anchor = { pos: np.pos, at: Date.now() };
    }
  }
  $: livePos = np.playing && np.live
    ? Math.min(np.dur > 0 ? np.dur : Infinity, anchor.pos + Math.max(0, tick - anchor.at) / 1000)
    : anchor.pos;

  // ── volume, mute: the one slider, the number at its right as on the glass's (m3w::slider) ──
  const volume = createSettingSender('audio.volume', { debounceMs: 150, live: true });
  const mute = createSettingSender('audio.mute', { debounceMs: 0 });
  const leveler = createSettingSender('audio.leveler', { debounceMs: 0 });
  onDestroy(() => [volume, mute, leveler].forEach((x) => x.cleanup()));

  $: muted = !!s['audio.mute'];
  $: leveling = !!s['audio.leveler'];
  $: vol = s['audio.volume'] ?? 0;
  // a finger drags it along; a press, or one that goes up or down, does not (lib/gesture.js)
  function setVolume(v) {
    if (v !== vol) volume.send(v);
  }

  // ── the «Звук» sheet: its content in its own file, fetched the first time it opens ──
  let soundOpen = false;
  let soundControls = null;
  function openSound() {
    soundControls ||= import('./sound/SoundControls.svelte');
    soundOpen = true;
  }

  // ── the card's queue modes ──
  function cycleRepeat() {
    const next = ((np.repeat || 0) + 1) % 3;
    setStateKey('player.repeat', next);
    apiPost('/api/settings', { 'player.repeat': next }).catch((e) => toastError(e.message));
  }
  function toggleShuffle() {
    setStateKey('player.shuffle', !np.shuffle);
    apiPost('/api/settings', { 'player.shuffle': !np.shuffle }).catch((e) => toastError(e.message));
  }

  function seek(e) {
    if (!np.seekKey) return;
    anchor = { pos: e.detail, at: Date.now() };
    apiPost('/api/settings', { [np.seekKey]: Math.round(e.detail) }).catch((err) => toastError(err.message));
  }

  // A catalog station that is not in the list yet (radio.station −1) can be kept from here.
  async function keepStation() {
    try {
      await apiPost('/api/settings', { 'radio.fav_add': true });
      toastSuccess($t['manage.kept']);
    } catch (e) {
      toastError(e.message);
    }
  }

  // ── the cover (A1): ui.cover names the picture the glass shows, 0 for none. A new one is fetched
  //    (behind the login, so not as a plain <img src>) and replaces the glyph; none, or a failed
  //    fetch, leaves the glyph. Never the login dialog: nobody asked for this picture. The whole
  //    tile, 280 a side: the cover is the page now, not a thumbnail.
  let coverUrl = '';
  let coverFor = 0;
  $: fetchCover(s['ui.cover'] || 0);
  async function fetchCover(id) {
    if (id === coverFor) return;
    coverFor = id;
    if (!id) return dropCover();
    try {
      const blob = await apiBlob('/api/cover?px=280', { ask: false });
      if (coverFor !== id) return;
      if (!blob.type.startsWith('image/')) throw new Error(blob.type);
      dropCover();
      coverUrl = URL.createObjectURL(blob);
    } catch (e) {
      if (coverFor === id) dropCover();
    }
  }
  function dropCover() {
    if (coverUrl) URL.revokeObjectURL(coverUrl);
    coverUrl = '';
  }
  onDestroy(dropCover);

  K('srcsub.tracks.one'); K('srcsub.tracks.few'); K('srcsub.tracks.many'); K('srcsub.tracks.other');

  $: hasTransport = !!np.toggle;
  $: playIcon = np.playing ? (np.stopIcon ? 'stop' : 'pause') : 'play_arrow';
  $: playLabel = !np.playing ? $t['transport.play'] : np.stopIcon ? $t['transport.stop'] : $t['transport.pause'];
  $: repeatLabel = np.repeat === 1 ? $t['transport.repeat_one'] : np.repeat === 2 ? $t['transport.repeat_all'] : $t['transport.repeat_off'];
</script>

<div class="page">
  <section class="pult" aria-label={$t['nav.player']}>
    {#if np.fromKey}
      <a class="from" href={href('source')}>
        <span class="eyebrow">{$t[np.fromKey]}</span>
        {#if np.device}<span class="device">{np.device}</span>{/if}
      </a>
    {:else}
      <span class="from"></span>
    {/if}

    <div class="art">
      {#if coverUrl}
        <img src={coverUrl} alt={$t['now.cover']} on:error={dropCover} />
      {:else}
        <Icon name={np.icon} size={72} />
      {/if}
    </div>

    <div class="meta">
      <span class="t">{title}</span>
      {#if sub || np.format}
        <span class="s">
          {#if sub}<span class="who">{sub}</span>{/if}
          {#if np.format}<span class="badge">{badge(np.format, $t, $language)}</span>{/if}
        </span>
      {/if}
    </div>

    {#if np.progress}
      <div class="prog">
        {#if np.progress === 'buffer'}
          <WaveLine fraction={(np.buffer || 0) / 100} moving={np.playing} />
        {:else}
          <TrackProgress pos={livePos} dur={np.dur} seekable={!!np.seekKey && np.live} label={$t['progress.position']} on:seek={seek} />
        {/if}
      </div>
    {/if}

    {#if hasTransport}
      <div class="transport" class:dead={!np.live}>
        {#if np.repeat !== null}
          <IconButton icon="shuffle" label={$t['transport.shuffle']} selected={np.shuffle} on:click={toggleShuffle} />
        {/if}
        <button class="tbtn side" type="button" aria-label={$t['transport.prev']} disabled={!np.live} on:click={() => command(np.prev.key, np.prev.value)}>
          <Icon name="skip_previous" size={40} fill />
        </button>
        <button class="tbtn main" type="button" aria-label={playLabel} disabled={!np.live} on:click={() => command(np.toggle.key, np.toggle.value)}>
          <Icon name={playIcon} size={44} fill />
        </button>
        <button class="tbtn side" type="button" aria-label={$t['transport.next']} disabled={!np.live} on:click={() => command(np.next.key, np.next.value)}>
          <Icon name="skip_next" size={40} fill />
        </button>
        {#if np.repeat !== null}
          <IconButton icon={np.repeat === 1 ? 'repeat_one' : 'repeat'} label={repeatLabel} selected={np.repeat > 0} on:click={cycleRepeat} />
        {/if}
      </div>
    {:else}
      <div class="pick"><Button variant="tonal" icon="speaker_group" href={href('source')}>{$t['nav.source']}</Button></div>
    {/if}

    <div class="vol">
      <IconButton icon={muted ? 'volume_off' : 'volume_down'} size={20} label={muted ? $t['volume.unmute'] : $t['volume.mute']} selected={muted} on:click={() => mute.send(!muted)} />
      <div class="line" class:off={muted}>
        <Slider label={$t['volume.label']} min={0} max={100} step={1} value={Math.min(100, Math.max(0, vol))}
                on:input={(e) => setVolume(e.detail)} />
      </div>
    </div>

    <div class="quick">
      <button class="pill" type="button" on:click={openSound}><Icon name="tune" size={20} />{$t['sys.sound']}</button>
      <IconButton icon="bedtime" label={$t['sound.leveler']} selected={leveling} on:click={() => leveler.send(!leveling)} />
    </div>
  </section>

  {#if soundOpen}
    <BottomSheet title={$t['sys.sound']} on:close={() => (soundOpen = false)}>
      {#await soundControls}
        <p class="wait">{$t['app.loading']}</p>
      {:then mod}
        <svelte:component this={mod.default} onLeave={() => (soundOpen = false)} />
      {:catch e}
        <p class="wait">{e.message}</p>
      {/await}
    </BottomSheet>
  {/if}

  <section class="manage" aria-label={$t[np.nameKey]}>
    {#if np.kind === 'radio'}
      <Group caption={$t['src.radio']}>
        {#if s['radio.station'] === -1 && s['radio.url']}
          <Row button icon="favorite" title={$t['manage.keep']} on:click={keepStation} />
        {/if}
        <Row icon="radio" title={$t['page.stations']} href={href('stations')} />
        <Row icon="search" title={$t['manage.catalog']} href={href('stations/top')} />
      </Group>
    {:else if np.kind === 'card'}
      <Group caption={$t['src.card']}>
        <Row icon="queue_music" title={$t['manage.queue']}
             subtitle={s['player.count'] ? fmt($t['manage.queue_sub'], { i: (s['player.index'] || 0) + 1, n: s['player.count'] }) : '—'} />
        {#if s['player.dir']}<Row icon="folder" title={s['player.dir']} />{/if}
        <Row icon="view_list" title={$t['page.library']}
             subtitle={s['player.count'] ? plural($t, $language, 'srcsub.tracks', s['player.count']) : $t['srcsub.card_none']}
             href={href('library')} />
      </Group>
    {:else if np.kind === 'dlna'}
      <Group caption={$t['src.dlna']}>
        <Row icon="cast" title={fmt($t['manage.visible_as'], { name: s['dlna.name'] || 'P4 Player' })} />
        <Row icon="hub" title={$t['page.dlna']} href={href('dlna')} />
      </Group>
    {:else if np.kind === 'bt'}
      <Group caption={$t['src.bt']}>
        <Row icon="smartphone" title={s['bt.peer'] || $t['srcsub.phone_off']} subtitle={s['bt.peer'] ? badge(np.format, $t, $language) : ''} />
      </Group>
    {:else if np.kind === 'airplay'}
      <Group caption={$t['src.airplay']}>
        <Row icon="airplay" title={fmt($t['manage.visible_as'], { name: s['airplay.name'] || 'P4 Player' })}
             subtitle={msg($t, s['airplay.active_mode_label'] || '')} href={href('system')} />
      </Group>
    {/if}
  </section>
</div>

<style>
  .page {
    display: grid;
    /* minmax(0, …): a long nowrap line in a row must ellipsize, not widen the column */
    grid-template-columns: minmax(0, 1fr);
    gap: 24px;
    align-items: start;
  }
  @media (min-width: 600px) {
    /* a rail, and no bar at the bottom */
    .art { max-width: max(200px, calc(100vh - 420px)); max-width: max(200px, calc(100svh - 420px)); }
  }
  @media (min-width: 900px) {
    .page { grid-template-columns: minmax(320px, 420px) minmax(0, 1fr); gap: 8px 32px; }
    .manage { min-width: 0; padding-top: 66px; }
  }
  .pult {
    display: flex;
    flex-direction: column;
    min-width: 0;
    width: 100%;
    max-width: 420px;
    margin: 0 auto;
    padding: 12px 16px 0;
  }

  /* where it plays from: «ГРАЄ З BLUETOOTH» and the phone, over the cover */
  .from {
    align-self: center;
    min-height: 38px;
    max-width: 100%;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 2px;
    color: var(--on-surface);
    text-decoration: none;
    text-align: center;
  }
  .eyebrow {
    font-size: 12px;
    line-height: 16px;
    font-weight: 500;
    letter-spacing: 1.2px;
    text-transform: uppercase;
    color: var(--on-surface-var);
  }
  .device { font-size: 15px; line-height: 20px; font-weight: 500; }
  .eyebrow, .device, .t, .who { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 100%; }

  /* as wide as the column, and on a short phone no taller than leaves «Звук» on the screen */
  .art {
    margin-top: 16px;
    align-self: center;
    width: 100%;
    max-width: max(200px, calc(100vh - 500px));
    max-width: max(200px, calc(100svh - 500px));
    aspect-ratio: 1;
    border-radius: 18px;
    background: var(--surface-high);
    color: var(--outline);
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: hidden;
    box-shadow: 0 24px 48px rgb(0 0 0 / 0.5);
  }
  .art img { width: 100%; height: 100%; object-fit: cover; display: block; }

  .meta { margin-top: 22px; display: flex; flex-direction: column; gap: 4px; min-width: 0; }
  .t { font-size: 24px; line-height: 30px; font-weight: 500; }
  .s {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
    font-size: 17px;
    line-height: 24px;
    color: var(--on-surface-var);
  }
  .badge {
    flex: none;
    font-size: 11px;
    line-height: 16px;
    font-weight: 500;
    letter-spacing: 0.4px;
    padding: 1px 6px;
    border-radius: 4px;
    border: 1px solid var(--outline-var);
    border-color: color-mix(in srgb, var(--outline) 50%, var(--outline-var));
    white-space: nowrap;
  }
  .prog { margin-top: 16px; }

  .transport {
    margin-top: 8px;
    height: 84px;
    display: flex;
    align-items: center;
    justify-content: space-evenly;
  }
  .tbtn {
    border: 0;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    position: relative;
    flex: none;
    background: none;
    color: var(--on-surface);
  }
  .tbtn::after {
    content: '';
    position: absolute;
    inset: 0;
    border-radius: inherit;
    background: currentColor;
    opacity: 0;
    transition: opacity 120ms ease;
  }
  .tbtn:hover:not(:disabled)::after { opacity: 0.08; }
  .tbtn:active:not(:disabled)::after { opacity: 0.12; }
  .side { width: 56px; height: 56px; }
  .main {
    width: 80px;
    height: 80px;
    background: var(--primary);
    color: var(--on-primary);
    box-shadow: 0 8px 20px rgb(0 0 0 / 0.35);
  }
  .tbtn:disabled { opacity: 0.38; cursor: default; }
  .pick { display: flex; justify-content: center; margin: 16px 0 8px; }

  /* the volume: the mute button, then the one slider with the number at its right */
  .vol { margin-top: 8px; display: flex; align-items: center; gap: 8px; }
  .line { flex: 1; min-width: 0; }
  .off :global(.fill), .off :global(.knob) { background: var(--outline); }

  .quick { display: flex; justify-content: center; align-items: center; gap: 8px; margin-top: 12px; }
  .pill {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    height: 40px;
    padding: 0 18px 0 14px;
    border: 0;
    border-radius: 20px;
    background: var(--surface-high);
    background: color-mix(in srgb, var(--on-surface) 10%, transparent);
    color: var(--on-surface);
    font-size: 15px;
    font-weight: 500;
    position: relative;
  }
  .pill::after {
    content: '';
    position: absolute;
    inset: 0;
    border-radius: inherit;
    background: currentColor;
    opacity: 0;
    transition: opacity 120ms ease;
  }
  .pill:hover::after { opacity: 0.08; }
  .pill:active::after { opacity: 0.12; }

  .wait { color: var(--on-surface-var); padding: 8px 0 16px; }
</style>
