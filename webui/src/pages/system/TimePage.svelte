<script>
  // Time: the panel's clock — from the internet (NTP) or set
  // by hand — and its time zone, over GET / POST /api/time.
  import { onMount } from 'svelte';
  import { t } from '../../stores/i18n.js';
  import { state } from '../../stores/state.js';
  import { apiGet, apiPost } from '../../lib/api.js';
  import { TIMEZONES } from '../../lib/timezones.js';
  import { toastSuccess, toastError } from '../../stores/toast.js';
  import Group from '../../m3/Group.svelte';
  import Row from '../../m3/Row.svelte';
  import Switch from '../../m3/Switch.svelte';
  import Button from '../../m3/Button.svelte';

  let d = null;
  let failed = '';
  let ntp = true;
  let tz = '';
  let manual = '';

  const pad = (n) => String(n).padStart(2, '0');
  const local = (ms) => {
    const x = new Date(ms);
    return `${x.getFullYear()}-${pad(x.getMonth() + 1)}-${pad(x.getDate())}T${pad(x.getHours())}:${pad(x.getMinutes())}`;
  };

  async function load() {
    try {
      d = await apiGet('/api/time');
      ntp = !!d.ntp_enabled;
      tz = d.timezone || '';
      manual = local(d.synced ? d.unix * 1000 : Date.now());
      failed = '';
    } catch (e) {
      failed = e.message;
    }
  }
  onMount(load);

  $: known = TIMEZONES.some((z) => z.tz === tz);
  $: s = $state;

  async function save(body, undo = () => {}) {
    try {
      await apiPost('/api/time', body);
      toastSuccess($t['time.saved']);
      setTimeout(load, 1500);
    } catch (e) {
      undo();
      toastError(e.message);
    }
  }

  // Shown at once; one the device refuses is taken back, unless it was changed again meanwhile
  // (as lib/settings.js writeNow does) — the switch and the zone show what the panel keeps.
  const setNtp = (on) => {
    const was = ntp;
    ntp = on;
    save({ mode: on ? 'ntp' : 'manual', timezone: tz }, () => { if (ntp === on) ntp = was; });
  };
  const setTz = (v) => {
    const was = tz;
    tz = v;
    save({ mode: ntp ? 'ntp' : 'manual', timezone: v }, () => { if (tz === v) tz = was; });
  };
  const setManual = (ms) => save({ mode: 'manual', timezone: tz, time: Math.floor(ms / 1000) });
</script>

{#if !d}
  <p class="msg">{failed || $t['app.loading']}</p>
{:else}
  <div class="clock">
    <span class="hm">{s['system.time'] || d.time}</span>
    <span class="date">{s['system.date'] || d.date}</span>
    {#if !d.synced}<span class="warn">{$t['time.not_set']}</span>{/if}
  </div>

  <Group>
    <Row icon="sync_alt" title={$t['time.ntp']}>
      <Switch checked={ntp} label={$t['time.ntp']} on:change={(e) => setNtp(e.detail)} />
    </Row>
    <Row icon="language" title={$t['time.zone']}>
      <select class="pick" aria-label={$t['time.zone']} value={tz} on:change={(e) => setTz(e.target.value)}>
        {#if !known && tz}<option value={tz}>{tz}</option>{/if}
        {#each TIMEZONES as z}<option value={z.tz}>{z.label}</option>{/each}
      </select>
    </Row>
  </Group>

  {#if !ntp}
    <Group caption={$t['time.manual']}>
      <div class="manual">
        <input type="datetime-local" bind:value={manual} aria-label={$t['time.manual']} />
        <Button variant="tonal" on:click={() => setManual(new Date(manual).getTime())}>{$t['time.set']}</Button>
      </div>
      <div class="end">
        <Button variant="text" icon="smartphone" on:click={() => setManual(Date.now())}>{$t['time.from_device']}</Button>
      </div>
    </Group>
  {/if}
{/if}

<style>
  .msg { text-align: center; color: var(--on-surface-var); padding: 32px 16px; }
  .clock { display: flex; flex-direction: column; align-items: center; padding: 16px 0 8px; gap: 2px; }
  .hm { font-size: 45px; line-height: 52px; color: var(--on-surface); font-variant-numeric: tabular-nums; }
  .date { font-size: 16px; color: var(--on-surface-var); }
  .warn { font-size: 14px; color: var(--error); margin-top: 4px; }
  .pick {
    height: 40px;
    max-width: 60%;
    border: 1px solid var(--outline-var);
    border-radius: 20px;
    padding: 0 12px;
    background: var(--secondary-container);
    color: var(--on-secondary-container);
    font: inherit;
    font-size: 15px;
  }
  .pick option { background: var(--surface-container); color: var(--on-surface); }
  .manual { display: flex; gap: 12px; align-items: center; padding: 12px 16px; flex-wrap: wrap; }
  .manual input {
    flex: 1;
    min-width: 200px;
    height: 44px;
    border: 1px solid var(--outline-var);
    border-radius: 8px;
    background: var(--surface);
    color: var(--on-surface);
    font: inherit;
    padding: 0 12px;
    color-scheme: dark;
  }
  .end { display: flex; justify-content: flex-end; padding: 0 8px 8px; }
</style>
