<script>
  // The web UI of P4 Player: the panel's sections, in the panel's colours, as a companion for
  // settings and control; its addresses are listed in stores/router.js.
  // The generated pages live on only as Service's long tail.
  import { onMount } from 'svelte';
  import { fade } from 'svelte/transition';
  import { loadUiConfig, uiLoading, uiError, deviceName } from './stores/ui.js';
  import { apiGet, needsLogin } from './lib/api.js';
  import { initWebSocket, state, wsOpened } from './stores/state.js';
  import { toastAction } from './stores/toast.js';
  import { createBuildWatch } from './lib/update.js';
  import './stores/scheme.js';
  import { route } from './stores/router.js';
  import { t, K } from './stores/i18n.js';
  import { NAV } from './lib/nav.js';
  import Shell from './shell/Shell.svelte';
  import Snackbar from './m3/Snackbar.svelte';
  import Button from './m3/Button.svelte';
  import LoginModal from './components/LoginModal.svelte';
  import PairDialog from './shell/PairDialog.svelte';
  import PlayerPage from './pages/PlayerPage.svelte';
  import SourcePage from './pages/SourcePage.svelte';
  import SystemPage from './pages/SystemPage.svelte';
  import LanguagePage from './pages/LanguagePage.svelte';

  // a tab open across a flash of the web: said when the socket opens again (lib/update.js)
  const watchBuild = createBuildWatch(
    () => fetch('/build.txt', { cache: 'no-store' }).then((r) => (r.ok ? r.text() : null)),
    () => toastAction($t['web.updated'], $t['web.reload'], () => location.reload(), 120000),
  );
  wsOpened.subscribe((n) => watchBuild(n > 0));

  onMount(async () => {
    await loadUiConfig();
    try {
      const s = await apiGet('/api/state');
      state.update((st) => ({ ...st, ...s }));
    } catch (e) {
      console.warn('Initial state load failed', e);
    }
    initWebSocket();
  });

  function titleOf(r, $t, name) {
    const [a, b] = r;
    if (a === 'player') return name;
    const nav = NAV.find((n) => n.id === a);
    if (nav && r.length === 1) return $t[nav.key];
    if (a === 'stations') return $t['page.stations'];
    if (a === 'library') return $t['page.library'];
    if (a === 'dlna') return $t['page.dlna'];
    if (a === 'system' && b === 'language') return $t['page.language'];
    if (a === 'system' && b === 'sound' && r[2] === 'eq') return $t['page.eq'];
    if (a === 'system' && b === 'service') return $t[SERVICE_TITLES[r[2] || '']] || $t['page.service'];
    if (a === 'system' && SYSTEM_TITLES[b]) return SYSTEM_TITLES[b].includes('.') ? $t[SYSTEM_TITLES[b]] : SYSTEM_TITLES[b];
    if (a === 'components') return $t['page.components'];
    return name;
  }

  $: title = titleOf($route, $t, $deviceName);
  $: document.title = $route[0] === 'player' ? $deviceName : `${title} · ${$deviceName}`;
  // Pages most visits never open come in their own file, fetched the first time (rollup chunks).
  // The promise is kept: the second visit is instant.
  let stationsPage = null;
  $: if ($route[0] === 'stations' && !stationsPage) stationsPage = import('./pages/StationsPage.svelte');
  // the equalizer: Налаштування › Звук › Еквалайзер
  let dspPage = null;
  $: if (key === 'system/sound/eq' && !dspPage) dspPage = import('./pages/DspPage.svelte');
  // The card's library and the DLNA browser, each its own file.
  let libraryPage = null;
  $: if (key === 'library' && !libraryPage) libraryPage = import('./pages/LibraryPage.svelte');
  let dlnaPage = null;
  $: if (key === 'dlna' && !dlnaPage) dlnaPage = import('./pages/DlnaPage.svelte');
  // System's own pages, each fetched the first time it is opened.
  const SYSTEM_PAGES = {
    wifi: () => import('./pages/system/WifiPage.svelte'),
    bt: () => import('./pages/system/BtPage.svelte'),
    time: () => import('./pages/system/TimePage.svelte'),
    access: () => import('./pages/system/AccessPage.svelte'),
    firmware: () => import('./pages/system/FirmwarePage.svelte'),
    backup: () => import('./pages/system/BackupPage.svelte'),
    sound: () => import('./pages/system/SoundPage.svelte'),
    speakers: () => import('./pages/system/SpeakersPage.svelte'),
    xover: () => import('./pages/system/XoverPage.svelte'),
    display: () => import('./pages/system/DisplayPage.svelte'),
    tuning: () => import('./pages/system/TuningPage.svelte'),
  };
  const SYSTEM_TITLES = {
    wifi: 'Wi-Fi', bt: 'Bluetooth', time: K('page.time'), access: K('page.access'),
    firmware: K('page.firmware'), backup: K('page.backup'), sound: K('sys.timbre_volume'), speakers: K('spk.title'),
    xover: K('xover.title'), display: K('sys.screen'), tuning: K('page.tuning'),
  };
  const systemLoaded = new Map();
  const loadSystem = (id) => {
    if (!systemLoaded.has(id)) systemLoaded.set(id, SYSTEM_PAGES[id]());
    return systemLoaded.get(id);
  };
  // Колонки has pages of its own under it — the wizard, the details — taken as `sub`
  $: sysId = $route[0] === 'system' && SYSTEM_PAGES[$route[1]] &&
    ($route.length === 2 || ($route.length === 3 && $route[1] === 'speakers')) ? $route[1] : null;
  $: sysPage = sysId ? loadSystem(sysId) : null;
  $: sysProps = sysId === 'speakers' ? { sub: $route[2] || '' } : {};
  // Service and the pages under it: diagnostics, calibration, tools, the long tail. Each its
  // own file. Calibration is the speakers' port alone since the amplifier's ceiling left it
  // (18.10.2026); system/service/calibration/port, the address the speakers' pages use, still leads
  // there.
  const SERVICE_PAGES = {
    '': () => import('./pages/ServicePage.svelte'),
    calibration: () => import('./pages/service/CalibrationPage.svelte'),
    screenshot: () => import('./pages/service/ScreenshotPage.svelte'),
    log: () => import('./pages/service/LogPage.svelte'),
    hardware: () => import('./pages/service/HardwarePage.svelte'),
    more: () => import('./pages/service/MorePage.svelte'),
  };
  const SERVICE_TITLES = {
    '': K('page.service'), calibration: K('cal.port'), screenshot: K('page.screenshot'),
    log: K('page.loglevel'), hardware: K('page.hardware'), more: K('page.more'),
  };
  const serviceLoaded = new Map();
  const loadService = (id) => {
    if (!serviceLoaded.has(id)) serviceLoaded.set(id, SERVICE_PAGES[id]());
    return serviceLoaded.get(id);
  };
  $: svcId = $route[0] === 'system' && $route[1] === 'service' && Object.hasOwn(SERVICE_PAGES, $route[2] || '')
    && ($route.length <= 3 || ($route.length === 4 && $route[2] === 'calibration')) ? $route[2] || '' : null;
  $: svcPage = svcId !== null ? loadService(svcId) : null;
  let componentsPage = null;
  $: if (key === 'components' && !componentsPage) componentsPage = import('./pages/ComponentsPage.svelte');

  // the station tabs are one page, and calibration is one page under two addresses: switching
  // them must not re-create it (and reload the list, or lose what was typed)
  $: key = $route[0] === 'stations' ? 'stations' : svcId === 'calibration' ? 'system/service/calibration' : $route.join('/');
</script>

{#if $uiLoading}
  <div class="splash" in:fade={{ duration: 200 }}>
    <div class="spinner" aria-hidden="true"></div>
    <p>{$t['app.loading']}</p>
  </div>
{:else if $uiError}
  <div class="splash">
    <p class="err">{$t['app.error']}</p>
    <p class="msg">{$uiError}</p>
    <Button variant="tonal" icon="refresh" on:click={() => location.reload()}>{$t['app.retry']}</Button>
  </div>
{:else}
  <Shell {title} mini={key !== 'player'} tint={key === 'player' ? $state['ui.cover_rgb'] || 0 : -1}>
    {#key key}
      <div in:fade={{ duration: 120 }}>
        {#if key === 'player'}
          <PlayerPage />
        {:else if key === 'source'}
          <SourcePage />
        {:else if $route[0] === 'stations' && $route.length <= 2 && stationsPage}
          {#await stationsPage}
            <p class="nf">{$t['app.loading']}</p>
          {:then mod}
            <svelte:component this={mod.default} tab={$route[1] || 'fav'} />
          {:catch e}
            <p class="nf">{e.message}</p>
          {/await}
        {:else if key === 'system/sound/eq' && dspPage}
          {#await dspPage}
            <p class="nf">{$t['app.loading']}</p>
          {:then mod}
            <svelte:component this={mod.default} />
          {:catch e}
            <p class="nf">{e.message}</p>
          {/await}
        {:else if key === 'library' && libraryPage}
          {#await libraryPage}
            <p class="nf">{$t['app.loading']}</p>
          {:then mod}
            <svelte:component this={mod.default} />
          {:catch e}
            <p class="nf">{e.message}</p>
          {/await}
        {:else if key === 'dlna' && dlnaPage}
          {#await dlnaPage}
            <p class="nf">{$t['app.loading']}</p>
          {:then mod}
            <svelte:component this={mod.default} />
          {:catch e}
            <p class="nf">{e.message}</p>
          {/await}
        {:else if key === 'system'}
          <SystemPage />
        {:else if sysPage}
          {#await sysPage}
            <p class="nf">{$t['app.loading']}</p>
          {:then mod}
            <svelte:component this={mod.default} {...sysProps} />
          {:catch e}
            <p class="nf">{e.message}</p>
          {/await}
        {:else if key === 'system/language'}
          <LanguagePage />
        {:else if svcPage}
          {#await svcPage}
            <p class="nf">{$t['app.loading']}</p>
          {:then mod}
            <svelte:component this={mod.default} />
          {:catch e}
            <p class="nf">{e.message}</p>
          {/await}
        {:else if key === 'components' && componentsPage}
          {#await componentsPage}
            <p class="nf">{$t['app.loading']}</p>
          {:then mod}
            <svelte:component this={mod.default} />
          {:catch e}
            <p class="nf">{e.message}</p>
          {/await}
        {:else}
          <p class="nf">{$t['page.not_found']}</p>
        {/if}
      </div>
    {/key}
  </Shell>
{/if}

{#if $needsLogin}
  <LoginModal />
{/if}

<Snackbar />
<PairDialog />

<style>
  .splash {
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 16px;
    padding: 24px;
    text-align: center;
    color: var(--on-surface-var);
  }
  .err { color: var(--on-surface); font-size: 22px; line-height: 28px; }
  .msg { font-size: 14px; }
  .spinner {
    width: 40px;
    height: 40px;
    border-radius: 50%;
    border: 4px solid var(--surface-highest);
    border-top-color: var(--primary);
    animation: spin 0.9s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }
  .nf { text-align: center; color: var(--on-surface-var); padding: 48px 16px; }
</style>
