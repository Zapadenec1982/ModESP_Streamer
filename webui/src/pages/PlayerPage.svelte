<script context="module">
  // The station list as last read for the presets (GET /api/stations), kept between visits: the
  // Player is the page opened most, and radio.list_rev says when the list has moved.
  let kept = null;
</script>

<script>
  // Player: a remote, not a second player.
  // Where the sound comes from; the cover, big; what plays, its codec; the
  // transport with a big pause; a thin volume that shows its number while it moves; one «Звук»
  // button for the sheet with the sounds, Бас, Високі and the loudness, and the moon beside it,
  // «Вирівнювання гучності» (audio.leveler, as on the glass), and where the source has a list —
  // the card's library, a DLNA server's folders — the glass's list button after them. Nothing
  // beside the card: what plays and from where is on it already, and the sources are the rail's.
  // The faces, texts and commands are the panel's (stores/now.js after screen_now.cpp), and so is
  // the background: the shell tints the page with ui.cover_rgb.
  //
  // A station is the glass's station face, not a track: no transport row and no buffer line — it
  // does not pause; it starts and stops from the mini-player and the knob. The station's name
  // between ‹ and ›, what it plays under it, and right there the six presets, the reason to own a
  // radio, with the glass's rules: a tap tunes, a hold stores what plays, a tap on an empty one
  // stores too. The picture takes the height the rest leaves, within bounds: the card fits one
  // screen with the volume.
  import { onDestroy } from 'svelte';
  import { now, subLine } from '../stores/now.js';
  import { state } from '../stores/state.js';
  import { t, fmt, language } from '../stores/i18n.js';
  import { href } from '../stores/router.js';
  import { command } from '../lib/cmd.js';
  import { badge } from '../lib/numbers.js';
  import { apiPost, apiBlob } from '../lib/api.js';
  import { createSettingSender, writeNow } from '../lib/settings.js';
  import { press } from '../lib/gesture.js';
  import { stationBadge } from '../lib/stationbadge.js';
  import { favicons } from '../lib/radiobrowser.js';
  import * as S from '../lib/stations.js';
  import { toastSuccess, toastError } from '../stores/toast.js';
  import Icon from '../m3/Icon.svelte';
  import IconButton from '../m3/IconButton.svelte';
  import Slider from '../m3/Slider.svelte';
  import TrackProgress from '../m3/TrackProgress.svelte';
  import BottomSheet from '../m3/BottomSheet.svelte';

  $: np = $now;
  $: title = np.title ?? $t[np.titleKey];
  $: sub = subLine(np, $t, $language);
  $: s = $state;
  $: station = np.kind === 'radio';

  // ── position between the device's position reports: count on while playing. The anchor moves
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

  // ── the card's queue modes: shown at once, taken back if the device refuses them ──
  function cycleRepeat() {
    const next = ((np.repeat || 0) + 1) % 3;
    writeNow({ 'player.repeat': next }).catch((e) => toastError(e.message));
  }
  function toggleShuffle() {
    writeNow({ 'player.shuffle': !np.shuffle }).catch((e) => toastError(e.message));
  }

  function seek(e) {
    if (!np.seekKey) return;
    anchor = { pos: e.detail, at: Date.now() };
    apiPost('/api/settings', { [np.seekKey]: Math.round(e.detail) }).catch((err) => toastError(err.message));
  }

  // ── the station face ──
  $: current = s['radio.station'];
  $: count = s['radio.count'] || 0;
  // ‹ and › while the list has another station to step to (screen_now.cpp refresh_station)
  $: steps = count > (current >= 0 && current < count ? 1 : 0);
  // something to store: a station of the list, or one from the catalog (station −1, with its
  // name), and the stream it plays to name it by
  $: storable = station && !!s['radio.url'] && (current >= 0 || !!s['radio.name']);

  // The presets are in the list, which only GET /api/stations gives: read when the face shows,
  // and again when radio.list_rev moves — by a store here, on the glass or on the Stations page.
  let list = kept;
  let unread = false;             // the last read failed
  let asked = null;               // the radio.list_rev the last read was for
  $: rev = s['radio.list_rev'];
  $: if (station && rev !== asked) readList(rev);
  async function readList(r) {
    asked = r;
    if (list && list.rev === r) return;
    try {
      const got = await S.loadStations();
      if (asked === r) list = kept = got;
      unread = false;
    } catch (e) {
      unread = true;              // the buttons wait: the next generation reads again
    }
  }

  // The logos: Radio Browser's favicons, asked from here by the stations' uuids (lib/radiobrowser.js
  // keeps them for the tab's session) — the presets' and the playing one's in one request, once
  // the list is read. No uuid, no answer or a picture that does not load: the station's disc
  // (lib/stationbadge.js, the glass's stand-in).
  let logos = {};
  let broken = {};
  $: wanted = station && (list || unread)
    ? [...new Set([...(list ? list.presets.map((i) => (i >= 0 ? list.stations[i]?.uuid : '')) : []), s['radio.uuid']]
      .filter(Boolean))].sort().join(',')
    : '';
  $: if (wanted) askLogos(wanted);
  async function askLogos(key) {
    try {
      logos = { ...logos, ...(await favicons(key.split(','))) };
    } catch (e) {
      // Radio Browser did not answer: the discs stay
    }
  }
  $: logoOf = (uuid) => (uuid && logos[uuid] && !broken[logos[uuid]] ? logos[uuid] : '');
  const breakLogo = (url) => (broken = { ...broken, [url]: true });

  $: presets = Array.from({ length: S.PRESETS }, (_, slot) => {
    const i = list ? list.presets[slot] : -1;
    const st = i >= 0 ? list.stations[i] : null;
    return { slot, i, st, on: !!st && i === current, disc: st ? stationBadge(st.name) : null, logo: st ? logoOf(st.uuid) : '' };
  });
  $: standIn = stationBadge(title);
  $: tileLogo = logoOf(s['radio.uuid']);
  // a catalog station that is not in the list (radio.station −1) can be kept without a preset
  $: keepable = station && current === -1 && !!s['radio.url'];

  // The glass's listen(): a station not tuned is a tune, the one tuned a play if it is not on.
  async function listen(i) {
    if (i !== current) {
      try {
        await writeNow({ 'radio.station': i });     // shown at once, taken back if refused
      } catch (e) {
        toastError(e.message);
      }
    } else if (!np.playing) {
      command('radio.cmd', 1);
    }
  }

  function presetTap(p) {
    if (!list) return;
    if (p.i >= 0) listen(p.i);
    else store(p.slot);
  }

  // What plays, into preset `slot`, through the catalog's route: radio.web_* names the station as
  // the panel says it plays, radio.web_act 2–7 the button (catalog.cpp: a station the list has is
  // found by its stream, one it has not is added). From the catalog (station −1) the glass also
  // makes the stream that plays the list's station (keep_playing_station), and so does
  // radio.fav_add in the same request; in either order the two end the same.
  let storing = false;
  $: inPreset = (name, n) => fmt($t['cat.in_preset'], { name, n });
  async function store(slot) {
    if (!list || !storable || storing) return;
    const keys = {
      'radio.web_name': S.fitName(s['radio.name']),
      'radio.web_url': s['radio.url'],
      'radio.web_uuid': s['radio.uuid'] || '',
      'radio.web_act': slot + 2,
    };
    if (current < 0) keys['radio.fav_add'] = true;
    storing = true;
    try {
      await apiPost('/api/settings', keys);
      // a station of the list is on its button at once; radio.list_rev then brings the list as
      // the panel has it
      if (current >= 0) list = { ...list, presets: S.setPreset(list, current, slot).presets };
      toastSuccess(inPreset(keys['radio.web_name'], slot + 1));
    } catch (e) {
      toastError(e.message);
    }
    storing = false;
  }

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

  $: hasTransport = !!np.toggle;
  // the glass's list button (screen_now.cpp list_cb): the station's list is «Усі станції» here
  $: listPage = np.kind === 'card' ? 'library' : np.kind === 'dlna' ? 'dlna' : '';
  $: listLabel = np.kind === 'card' ? 'page.library' : 'page.dlna';
  $: playIcon = np.playing ? 'pause' : 'play_arrow';
  $: playLabel = np.playing ? $t['transport.pause'] : $t['transport.play'];
  $: repeatLabel = np.repeat === 1 ? $t['transport.repeat_one'] : np.repeat === 2 ? $t['transport.repeat_all'] : $t['transport.repeat_off'];
</script>

<div class="page">
  <section class="pult" class:station aria-label={$t['nav.player']}>
    {#if np.fromKey}
      <a class="from" href={href('source')}>
        <span class="eyebrow">{$t[np.fromKey]}</span>
        {#if np.device}<span class="device">{np.device}</span>{/if}
      </a>
    {:else}
      <span class="from"></span>
    {/if}

    <div class="art" class:tile={station}>
      {#if coverUrl}
        <img src={coverUrl} alt={$t['now.cover']} on:error={dropCover} />
      {:else if station}
        <span class="disc" style:background={standIn.color}>
          {#if tileLogo}
            <img src={tileLogo} alt="" referrerpolicy="no-referrer" on:error={() => breakLogo(tileLogo)} />
          {:else}
            <span class="label" class:long={standIn.label.length > 3}>{standIn.label}</span>
          {/if}
        </span>
      {:else}
        <Icon name={np.icon} size={72} />
      {/if}
    </div>

    {#if station}
      <div class="tune">
        <button class="arrow" type="button" aria-label={$t['transport.prev']} disabled={!steps}
                on:click={() => command(np.prev.key, np.prev.value)}><Icon name="chevron_left" size={32} /></button>
        <span class="name">
          <span class="t">{title}</span>
          {#if keepable}<IconButton icon="favorite" size={20} label={$t['manage.keep']} on:click={keepStation} />{/if}
        </span>
        <button class="arrow" type="button" aria-label={$t['transport.next']} disabled={!steps}
                on:click={() => command(np.next.key, np.next.value)}><Icon name="chevron_right" size={32} /></button>
      </div>
      <span class="s now">
        {#if sub}<span class="who">{sub}</span>{/if}
        {#if np.format}<span class="badge">{badge(np.format, $t, $language)}</span>{/if}
      </span>

      <div class="presets" role="group" aria-label={$t['st.presets']}>
        {#each presets as p (p.slot)}
          {@const label = p.st ? p.st.name || '—' : storable ? $t['st.preset_store'] : $t['st.preset_empty']}
          <button class="preset" class:on={p.on} type="button" disabled={!list} aria-label={`${p.slot + 1}: ${label}`}
                  aria-current={p.on ? 'true' : null}
                  use:press={{ tap: () => presetTap(p), hold: () => store(p.slot) }}>
            <span class="pdisc" class:empty={!p.st} style:background={p.st ? p.disc.color : null}>
              {#if p.logo}
                <img src={p.logo} alt="" referrerpolicy="no-referrer" on:error={() => breakLogo(p.logo)} />
              {:else}{p.st ? p.disc.first : p.slot + 1}{/if}
            </span>
            <span class="pname" class:store={!p.st && storable} class:dim={!p.st && !storable}>{label}</span>
          </button>
        {/each}
      </div>
      <a class="all" href={href('stations')}>{fmt($t['st.all'], { n: count })}<Icon name="chevron_right" size={20} /></a>
    {:else}
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
          <TrackProgress pos={livePos} dur={np.dur} seekable={!!np.seekKey && np.live} label={$t['progress.position']} on:seek={seek} />
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
      {/if}
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
      {#if listPage}<IconButton icon="queue_music" variant="outlined" label={$t[listLabel]} href={href(listPage)} />{/if}
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
  .pult {
    display: flex;
    flex-direction: column;
    min-width: 0;
    width: 100%;
    max-width: 420px;
    margin: 0 auto;
    padding: 12px 16px 0;
  }
  /* the station's card is wider: three presets abreast on a phone */
  .pult.station { padding: 8px 0 0; }

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
  /* a station's: what the height leaves (the rest of the card, the bottom bar and the gap above
     it take 510 px; 20 more are kept spare), from 96 on a short phone to 260 on a tall one */
  .art.tile {
    --tile: clamp(96px, calc(100vh - 530px), 260px);
    margin-top: 8px;
    width: var(--tile);
    max-width: none;
    border-radius: 16px;
    box-shadow: 0 12px 32px rgb(0 0 0 / 0.45);
  }
  @supports (height: 100svh) {
    .art.tile { --tile: clamp(96px, calc(100svh - 530px), 260px); }
  }
  /* the station's disc: its logo, or its stand-in (lib/stationbadge.js), as on the glass's tile */
  .disc {
    width: 72%;
    aspect-ratio: 1;
    border-radius: 50%;
    overflow: hidden;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #fff;
  }
  .art .disc img { object-fit: contain; }
  /* the biggest that fits the disc: three characters, or five */
  .label { font-size: calc(var(--tile) * 0.24); font-weight: 600; line-height: 1; white-space: nowrap; }
  .label.long { font-size: calc(var(--tile) * 0.17); }

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

  /* ‹ the station › and what it plays, centred under it */
  .tune {
    margin-top: 12px;
    display: grid;
    grid-template-columns: 48px minmax(0, 1fr) 48px;
    align-items: center;
  }
  .arrow {
    width: 48px;
    height: 48px;
    border: 0;
    border-radius: 50%;
    padding: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    color: var(--on-surface);
    position: relative;
  }
  .arrow::after, .preset::after {
    content: '';
    position: absolute;
    inset: 0;
    border-radius: inherit;
    background: currentColor;
    opacity: 0;
    transition: opacity 120ms ease;
    pointer-events: none;
  }
  .arrow:hover:not(:disabled)::after, .preset:hover:not(:disabled)::after { opacity: 0.08; }
  .arrow:active:not(:disabled)::after, .preset:active:not(:disabled)::after { opacity: 0.12; }
  .arrow:disabled { opacity: 0.38; cursor: default; }
  .name { display: flex; align-items: center; justify-content: center; min-width: 0; }
  .name .t { font-size: 22px; line-height: 28px; }
  .name :global(.ib) { margin-right: -8px; }
  .now { justify-content: center; font-size: 15px; line-height: 20px; min-height: 20px; padding: 0 8px; }

  /* the six presets, 3 × 2: the glass's buttons, each a finger's height */
  .presets {
    margin-top: 12px;
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    gap: 8px;
  }
  .preset {
    height: 56px;
    min-width: 0;
    border: 0;
    border-radius: var(--r-s);
    padding: 0 8px 0 6px;
    display: flex;
    align-items: center;
    gap: 6px;
    text-align: left;
    background: var(--surface-high);
    background: color-mix(in srgb, var(--on-surface) 10%, transparent);
    color: var(--on-surface);
    position: relative;
    /* a long press stores: no text selection, no callout, no zoom on a double tap */
    -webkit-user-select: none;
    user-select: none;
    -webkit-touch-callout: none;
    touch-action: manipulation;
  }
  .preset.on { box-shadow: inset 0 0 0 2px var(--primary); }
  .preset:disabled { cursor: default; }
  .pdisc {
    width: 32px;
    height: 32px;
    border-radius: 50%;
    flex: none;
    overflow: hidden;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 17px;
    font-weight: 500;
    color: #fff;
  }
  .pdisc.empty { background: var(--surface-highest); color: var(--on-surface-var); font-size: 14px; }
  .pdisc img { width: 100%; height: 100%; object-fit: contain; display: block; }
  .pname {
    min-width: 0;
    font-size: 13px;
    line-height: 16px;
    font-weight: 500;
    overflow: hidden;
    display: -webkit-box;
    -webkit-box-orient: vertical;
    -webkit-line-clamp: 2;
    line-clamp: 2;
    overflow-wrap: break-word;
  }
  .pname.store { color: var(--primary); }
  .pname.dim { color: var(--on-surface-var); }
  .all {
    align-self: center;
    height: 44px;
    display: inline-flex;
    align-items: center;
    gap: 2px;
    padding: 0 8px 0 12px;
    font-size: 14px;
    font-weight: 500;
    color: var(--primary);
    text-decoration: none;
  }

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

  /* the volume: the mute button, then the one slider with the number at its right */
  .vol { margin-top: 8px; display: flex; align-items: center; gap: 8px; }
  .station .vol { margin-top: 4px; }
  .line { flex: 1; min-width: 0; }
  .off :global(.fill), .off :global(.knob) { background: var(--outline); }

  .quick { display: flex; justify-content: center; align-items: center; gap: 8px; margin-top: 12px; }
  .station .quick { margin-top: 4px; }
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
