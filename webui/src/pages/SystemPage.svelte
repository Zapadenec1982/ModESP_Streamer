<script>
  // Налаштування: the panel's
  // groups (screen_set.cpp) — Sound, Device, Links, Services — and what the glass does not do: the
  // clock, the login, firmware, a backup of the user's files. Each page under it is its own file,
  // fetched the first time it is opened. No Home Assistant row: MQTT stays off (customer,
  // 11.10.2026). Екран is a page of its own, as on the glass: inline, its sliders made this the
  // longest page of the web (the customer, 16.10.2026, from a review).
  import { onDestroy } from 'svelte';
  import { t, fmt, msg, language, LANG_NAMES } from '../stores/i18n.js';
  import { state, wsOpened } from '../stores/state.js';
  import { href } from '../stores/router.js';
  import { apiPost } from '../lib/api.js';
  import { createSettingSender } from '../lib/settings.js';
  import { speakerState } from '../lib/speakers.js';
  import { schemeKey } from '../lib/schemes.js';
  import { toastError } from '../stores/toast.js';
  import Group from '../m3/Group.svelte';
  import Row from '../m3/Row.svelte';
  import Switch from '../m3/Switch.svelte';
  import Dialog from '../m3/Dialog.svelte';
  import Button from '../m3/Button.svelte';

  $: s = $state;

  const now = (key) => createSettingSender(key, { debounceMs: 0 });
  const airplay = now('airplay.enabled');
  const airplayMode = now('airplay.mode');
  const dlna = now('dlna.enabled');
  const covers = now('radio.covers');
  onDestroy(() => [airplay, airplayMode, dlna, covers].forEach((x) => x.cleanup()));

  // the screen at a glance, as the glass's row says it: its brightness and the scheme
  $: screenSub = `${s['ui.backlight'] ?? 80} % · ${$t[schemeKey(s['ui.theme'] ?? 0)]}`;
  $: wifiSub = s['wifi.connected']
    ? [s['wifi.ssid'], s['wifi.ip']].filter(Boolean).join(' · ')
    : s['wifi.mode'] === 'ap' ? $t['sys.wifi_ap'] : $t['sys.wifi_off'];
  $: btSub = !s['bt.link'] ? $t['sys.bt_none'] : s['bt.peer'] || $t['sys.bt_ready'];
  // what has been done about the speakers, in the words of their page
  $: spk = speakerState(s);
  $: spkSub = spk.line ? fmt($t[spk.line], spk.args) : $t[spk.title];

  // ── restart ──
  let confirmRestart = false;
  let restarting = false;
  // the panel is back once the socket has opened again since it was asked
  let openedAt = 0;
  $: if (restarting && $wsOpened !== openedAt) restarting = false;
  async function restart() {
    confirmRestart = false;
    restarting = true;
    openedAt = $wsOpened;
    try {
      await apiPost('/api/restart', {});
    } catch (e) {
      restarting = false;
      toastError(e.message);
    }
  }
</script>

<div class="cols">
  <div class="col">
    <Group caption={$t['sys.sound']}>
      <Row icon="tune" title={$t['sys.timbre_volume']} href={href('system/sound')} />
      <Row icon="speaker" title={$t['spk.title']} subtitle={spkSub} href={href('system/speakers')} />
      <Row icon="call_split" title={$t['xover.title']} subtitle={$t['xover.stereo']} href={href('system/xover')} />
      <Row icon="graphic_eq" title={$t['page.tuning']} href={href('system/tuning')} />
    </Group>

    <Group caption={$t['sys.device']}>
      <Row icon="brightness_6" title={$t['sys.screen']} subtitle={screenSub} href={href('system/display')} />
      <Row icon="language" title={$t['page.language']} subtitle={LANG_NAMES[$language]} href={href('system/language')} />
      <Row icon="schedule" title={$t['page.time']} subtitle={s['system.time'] || ''} href={href('system/time')} />
      <Row icon="lock" title={$t['page.access']} href={href('system/access')} />
      <Row icon="system_update" title={$t['page.firmware']} href={href('system/firmware')} />
      <Row icon="settings_backup_restore" title={$t['page.backup']} href={href('system/backup')} />
      <Row icon="developer_board" title={$t['page.service']} href={href('system/service')} />
      <Row button icon="restart_alt" title={$t['sys.restart']} subtitle={restarting ? $t['sys.restarting'] : ''}
           disabled={restarting} on:click={() => (confirmRestart = true)} />
    </Group>
  </div>

  <div class="col">
    <Group caption={$t['sys.links']}>
      <Row icon="wifi" title="Wi-Fi" subtitle={wifiSub} href={href('system/wifi')} />
      <Row icon="bluetooth" title="Bluetooth" subtitle={btSub} href={href('system/bt')} />
    </Group>

    <Group caption={$t['sys.services']}>
      <Row icon="airplay" title="AirPlay" subtitle={s['airplay.name'] || ''}>
        <Switch checked={!!s['airplay.enabled']} label="AirPlay" on:change={(e) => airplay.send(e.detail)} />
      </Row>
      {#if s['airplay.enabled']}
        <!-- the mode is the select's; under it only what is happening to it, while something is,
             as the glass says it (screen_set.cpp) — not «Режим застосовано» for ever -->
        <Row icon="swap_horiz" title={$t['sys.airplay_mode']}
             subtitle={msg($t, s['airplay.mode_error'] || (s['airplay.switching'] ? s['airplay.mode_status'] : '') || '')}>
          <select class="pick" aria-label={$t['sys.airplay_mode']} value={s['airplay.mode'] ?? 0}
                  on:change={(e) => airplayMode.send(Number(e.target.value))}>
            <option value={0}>AirPlay 2</option>
            <option value={1}>{$t['sys.airplay_1']}</option>
          </select>
        </Row>
      {/if}
      <Row icon="cast" title="DLNA" subtitle={s['dlna.name'] || ''}>
        <Switch checked={!!s['dlna.enabled']} label="DLNA" on:change={(e) => dlna.send(e.detail)} />
      </Row>
      <Row icon="image" title={$t['sys.covers']}>
        <Switch checked={!!s['radio.covers']} label={$t['sys.covers']} on:change={(e) => covers.send(e.detail)} />
      </Row>
    </Group>
  </div>
</div>

{#if confirmRestart}
  <Dialog title={$t['sys.restart_q']} on:close={() => (confirmRestart = false)}>
    <p class="b">{$t['sys.restart_body']}</p>
    <svelte:fragment slot="actions">
      <Button variant="text" on:click={() => (confirmRestart = false)}>{$t['sys.cancel']}</Button>
      <Button on:click={restart}>{$t['sys.restart']}</Button>
    </svelte:fragment>
  </Dialog>
{/if}

<style>
  .cols { display: grid; grid-template-columns: minmax(0, 1fr); gap: 0 16px; }
  @media (min-width: 900px) {
    .cols { grid-template-columns: repeat(2, minmax(0, 1fr)); align-items: start; }
  }
  .col { display: flex; flex-direction: column; min-width: 0; }
  .pick {
    height: 40px;
    max-width: 50%;
    border: 1px solid var(--outline-var);
    border-radius: 20px;
    padding: 0 12px;
    background: var(--secondary-container);
    color: var(--on-secondary-container);
    font: inherit;
    font-size: 15px;
    font-weight: 500;
  }
  .pick option { background: var(--surface-container); color: var(--on-surface); }
  .b { font-size: 16px; line-height: 24px; color: var(--on-surface-var); }
</style>
