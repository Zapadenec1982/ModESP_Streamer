<script>
  // Source, stage W1: the panel's cards (screen_src.cpp) — only those whose bit is in
  // audio.sources, the active one on primary_container with a check, under each what it would
  // give now. A tap is "listen": the source is set and the Player opens, as on the glass.
  // One grid, the most listened to first (customer, 18.10.2026): Bluetooth, AirPlay, the radio,
  // then the card, the media servers and USB — the glass's order. USB took AUX's place
  // (the customer, 24.10.2026): the line input left the hardware on 15.10.2026. DLNA has no card: an app starts
  // it and the panel follows by itself; while a pushed track plays, the servers' card shows it.
  // The lists — the stations, the catalog, the library — are the Player's, for the source that
  // plays.
  import { now } from '../stores/now.js';
  import { state } from '../stores/state.js';
  import { t, K, plural, language } from '../stores/i18n.js';
  import { go } from '../stores/router.js';
  import { writeNow } from '../lib/settings.js';
  import { toastError } from '../stores/toast.js';
  import { badge } from '../lib/numbers.js';
  import Icon from '../m3/Icon.svelte';

  // The panel's order (screen_src.cpp CARDS). The media servers ride the radio's stream, so they
  // show with bit 5.
  const CARDS = [
    { id: 'bt', source: 3, bit: 3, icon: 'bluetooth', key: K('src.bt') },
    { id: 'airplay', source: 6, bit: 6, icon: 'airplay', key: K('src.airplay') },
    { id: 'radio', source: 5, bit: 5, icon: 'radio', key: K('src.radio') },
    { id: 'card', source: 4, bit: 4, icon: 'sd_card', key: K('src.card') },
    { id: 'servers', source: 5, bit: 5, icon: 'hub', key: K('src.servers') },
    { id: 'usb', source: 7, bit: 7, icon: 'usb', key: K('src.usb') },
  ];
  // a pushed track (now.kind 'dlna') is the servers' card's
  const isActive = (c, np) => np.kind === c.id || (c.id === 'servers' && np.kind === 'dlna');
  // marks the plural forms used (stores/i18n.js plural())
  K('srcsub.tracks.one'); K('srcsub.tracks.few'); K('srcsub.tracks.many'); K('srcsub.tracks.other');
  K('srcsub.stations.one'); K('srcsub.stations.few'); K('srcsub.stations.many'); K('srcsub.stations.other');

  $: s = $state;
  $: sources = s['audio.sources'] ?? 0;
  $: has = (c) => (sources & (1 << c.bit)) !== 0;
  $: cards = CARDS.filter(has);

  // What the card would give now — screen_src.cpp:118-153.
  function describe(c, s, np, $t, lang) {
    const playing = isActive(c, np) ? np.title : null;
    switch (c.id) {
      case 'bt':
        return playing || (s['bt.peer'] ? $t['srcsub.phone_on'] : $t['srcsub.phone_off']);
      case 'servers':
        return playing || $t['srcsub.servers'];
      case 'card': {
        const n = s['player.count'] || 0;
        return playing || (n > 0 ? plural($t, lang, 'srcsub.tracks', n) : $t['srcsub.card_none']);
      }
      case 'radio': {
        if (s['radio.name']) return s['radio.name'];
        const n = s['radio.count'] || 0;
        return n > 0 ? plural($t, lang, 'srcsub.stations', n) : $t['srcsub.radio_none'];
      }
      case 'airplay': {
        const on = ['connected', 'playing', 'paused'].includes(s['airplay.state']);
        return playing || (on ? $t['srcsub.phone_on'] : $t['srcsub.airplay_wait']);
      }
      case 'usb': {
        // what the computer sends once it sends; otherwise whether there is one
        const on = ['ready', 'playing'].includes(s['usb_audio.state']);
        return (isActive(c, np) && badge(np.subtitle, $t, lang)) || (on ? $t['srcsub.usb_on'] : $t['srcsub.usb_wait']);
      }
      default:
        return '';
    }
  }

  async function choose(c) {
    if (c.id === 'servers') {
      // Nothing to switch to until a track is chosen on a server: the browser is where that
      // happens, as on the glass (screen_src.cpp). Once one is pushed, the card leads to it.
      go(isActive(c, $now) ? 'player' : 'dlna');
      return;
    }
    if ($now.kind !== c.id) {
      // in body order: the source first, then the radio's "play" (screen_src.cpp:90-113); shown
      // at once, and taken back if the device refuses it
      const body = { 'audio.source': c.source };
      if (c.id === 'radio') body['radio.cmd'] = 1;
      try {
        await writeNow(body);
      } catch (e) {
        toastError(e.message);
        return;
      }
    }
    go('player');
  }
</script>

<div class="grid">
  {#each cards as c (c.id)}
    {@const active = isActive(c, $now)}
    <button class="src" class:active type="button" aria-pressed={active ? 'true' : 'false'} on:click={() => choose(c)}>
      <span class="ic"><Icon name={c.icon} size={28} fill={active} /></span>
      {#if active}<span class="check"><Icon name="check_circle" size={26} fill /></span>{/if}
      <span class="name">{$t[c.key]}</span>
      <span class="sub">{describe(c, s, $now, $t, $language)}</span>
    </button>
  {/each}
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 12px;
    padding-top: 8px;
  }
  @media (min-width: 900px) {
    .grid { grid-template-columns: repeat(3, minmax(0, 1fr)); }
  }
  .src {
    position: relative;
    min-height: 128px;
    border: 0;
    border-radius: var(--r-l);
    padding: 14px 16px;
    text-align: left;
    display: flex;
    flex-direction: column;
    gap: 2px;
    background: var(--surface-container);
    color: var(--on-surface);
  }
  .src::after {
    content: '';
    position: absolute;
    inset: 0;
    border-radius: inherit;
    background: currentColor;
    opacity: 0;
    transition: opacity 120ms ease;
  }
  .src:hover::after { opacity: 0.06; }
  .src:active::after { opacity: 0.1; }
  .active { background: var(--primary-container); color: var(--on-primary-container); }
  .ic { display: flex; margin-bottom: 10px; }
  .check { position: absolute; right: 14px; top: 14px; display: flex; }
  .name { font-size: 20px; line-height: 26px; }
  .sub {
    font-size: 14px;
    line-height: 20px;
    font-weight: 500;
    opacity: 0.9;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }
</style>
