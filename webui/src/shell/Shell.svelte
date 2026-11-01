<script>
  // The frame every page sits in: a top bar, the three sections — a bottom bar on
  // a phone, a rail from 600 px — the mini-player over them, and while a test tone plays, its bar
  // over the page. Losing the panel shows a banner
  // after 5 s, as the previous interface did, and the panel's fourth WebSocket client is told so.
  import { onDestroy } from 'svelte';
  import { section, isSectionPage, back, route } from '../stores/router.js';
  import { wsConnected, wsBusy, state } from '../stores/state.js';
  import { t } from '../stores/i18n.js';
  import { toastSuccess } from '../stores/toast.js';
  import NavItems from './NavItems.svelte';
  import MiniPlayer from './MiniPlayer.svelte';
  import { now } from '../stores/now.js';
  import Icon from '../m3/Icon.svelte';
  import IconButton from '../m3/IconButton.svelte';

  export let title = '';
  /** Hide the mini-player where the page itself is the player. */
  export let mini = true;
  // ... and where nothing plays and nothing could go on from it: switched off, no phone, a
  // computer that sends nothing, a stopped station (its button is the mute) — «Телефон не
  // підключено» over the navigation was a line with nothing to do. A pause it can resume keeps it.
  $: np = $now;
  $: showMini = mini && (np.playing || !!(np.toggle && np.live));
  /** The Player's background: ui.cover_rgb, the colour
   *  the glass draws from the cover, RGB888; 0 on the Player with no cover, −1 on every other
   *  page. The Player has no top bar: the page starts with where the sound comes from, and the
   *  height is the cover's. */
  export let tint = -1;

  let lost = false;
  let lostTimer = null;
  const unsub = wsConnected.subscribe((up) => {
    if (!up) {
      if (!lostTimer) lostTimer = setTimeout(() => (lost = true), 5000);
    } else {
      clearTimeout(lostTimer);
      lostTimer = null;
      // A busy panel is asked again every 5 s and each try opens for a moment: not a restore.
      if (lost && !$wsBusy) toastSuccess($t['conn.restored']);
      lost = false;
    }
  });
  // A hidden tab holds no socket (lib/websocket.js) and only tries again once shown, so the 5 s
  // start over then: a tab opened in the background is not "lost" and then "restored".
  function onVisibility() {
    if (document.visibilityState !== 'visible' || $wsConnected) return;
    lost = false;
    clearTimeout(lostTimer);
    lostTimer = setTimeout(() => (lost = true), 5000);
  }
  document.addEventListener('visibilitychange', onVisibility);
  onDestroy(() => {
    unsub();
    clearTimeout(lostTimer);
    document.removeEventListener('visibilitychange', onVisibility);
  });

  $: tinted = tint >= 0;
  $: tintStyle = tint > 0 ? `--tint:#${tint.toString(16).padStart(6, '0')}` : null;

  $: wifiUp = !!$state['wifi.connected'];
  $: ssid = $state['wifi.ssid'] || '';

  // The test tone's bar is fetched the first time a tone plays, as its own file: most visits never
  // see one, and with it would come the calibration's arithmetic.
  // audio.source 1 is the tone — SRC_TONE in lib/tone.js, not imported here for that reason.
  const SRC_TONE = 1;
  let ToneBar = null;
  let toneBarLoading = false;
  $: if ($state['audio.source'] === SRC_TONE && !ToneBar && !toneBarLoading) loadToneBar();
  async function loadToneBar() {
    toneBarLoading = true;
    try {
      ToneBar = (await import('./ToneBar.svelte')).default;
    } catch (e) {
      console.warn('Tone bar failed to load', e);   // the next tone tries again
    }
    toneBarLoading = false;
  }
</script>

<div class="shell" class:with-mini={showMini}>
  <nav class="rail" aria-label={$t['nav.sections']}>
    <span class="brand"><Icon name="speaker" size={32} /></span>
    <NavItems active={$section} />
  </nav>

  <div class="main" class:tinted style={tintStyle}>
    <header class="topbar">
      {#if !$isSectionPage}
        <IconButton icon="arrow_back" label={$t['common.back']} on:click={() => back($route)} />
      {/if}
      <h1 class="title" class:pad={$isSectionPage}>{title}</h1>
      <span class="wifi" class:down={!wifiUp} title={ssid}>
        <Icon name={wifiUp ? 'wifi' : 'wifi_off'} size={20} label={ssid || 'Wi-Fi'} />
      </span>
    </header>

    {#if lost || $wsBusy}
      <div class="banner" role="status">
        <Icon name="warning" size={20} />
        <span>{$wsBusy ? $t['conn.busy'] : $t['conn.lost']}</span>
      </div>
    {/if}

    <main class="content">
      {#if ToneBar}<svelte:component this={ToneBar} />{/if}
      <slot />
    </main>
  </div>

  {#if showMini}
    <div class="mini-dock"><MiniPlayer /></div>
  {/if}

  <nav class="navbar" aria-label={$t['nav.sections']}>
    <NavItems active={$section} />
  </nav>
</div>

<style>
  .shell { min-height: 100vh; }

  .rail { display: none; }

  .main {
    min-width: 0;
    padding-bottom: calc(var(--navbar-h) + 16px);
  }
  .with-mini .main { padding-bottom: calc(var(--navbar-h) + var(--mini-h) + 24px); }

  /* the cover's colour from the top, fading into the surface: a new cover fades in (--tint is
     registered in base.css, so it can be animated) */
  .tinted {
    background: radial-gradient(130% 560px at 50% 0, var(--tint), transparent) no-repeat;
    transition: --tint 0.6s ease;
  }
  .tinted .topbar { display: none; }

  .topbar {
    position: sticky;
    top: 0;
    z-index: 50;
    height: var(--topbar-h);
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 0 8px 0 4px;
    background: var(--surface);
  }
  .title {
    flex: 1;
    min-width: 0;
    font-size: 22px;
    line-height: 28px;
    font-weight: 400;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .title.pad { padding-left: 12px; }
  @media (max-width: 420px) {
    .title { font-size: 20px; }                /* «Aktualizacja oprogramowania» whole at 390 px */
  }
  .wifi {
    color: var(--on-surface-var);
    display: flex;
    padding: 0 12px;
  }
  .wifi.down { color: var(--outline-var); }

  .banner {
    margin: 0 var(--gutter) 8px;
    padding: 10px 14px;
    border-radius: var(--r-s);
    background: var(--secondary-container);
    color: var(--on-secondary-container);
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 14px;
    line-height: 20px;
  }

  .content {
    padding: 0 var(--gutter);
    /* a phone on its side: clear of the notch */
    padding-left: max(var(--gutter), env(safe-area-inset-left, 0px));
    padding-right: max(var(--gutter), env(safe-area-inset-right, 0px));
    max-width: var(--content-max);
    margin: 0 auto;
  }

  .mini-dock {
    position: fixed;
    left: 8px;
    right: 8px;
    bottom: calc(var(--navbar-h) + 8px);
    z-index: 60;
  }

  .navbar {
    position: fixed;
    left: 0;
    right: 0;
    bottom: 0;
    z-index: 60;
    height: var(--navbar-h);
    background: var(--surface-container);
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));   /* lib/nav.js NAV */
    padding-top: 12px;
    padding-bottom: var(--safe-b);
  }

  @media (min-width: 600px) {
    .rail {
      position: fixed;
      top: 0;
      bottom: 0;
      left: 0;
      width: var(--rail-w);
      z-index: 60;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 12px;
      padding: 16px 0;
      background: var(--surface);
    }
    .brand { color: var(--primary); margin: 4px 0 20px; display: flex; }
    .navbar { display: none; }
    .main {
      margin-left: var(--rail-w);
      padding-bottom: 32px;
    }
    .with-mini .main { padding-bottom: calc(var(--mini-h) + 40px); }
    .topbar { padding-left: 12px; }
    .content { padding: 0 24px; }
    .mini-dock {
      left: calc(var(--rail-w) + 16px);
      right: 16px;
      bottom: 16px;
      max-width: 560px;
      margin: 0 auto;
    }
  }
</style>
