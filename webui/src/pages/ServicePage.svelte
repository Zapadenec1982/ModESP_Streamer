<script>
  // Service: the panel's service page (screen_svc.cpp) — three figures
  // and the groups under them, each number in the colour the panel's own thresholds give it
  // (lib/service.js, held to service_readout.h by a host test) — then calibration and the tools.
  // The figures are read every 2 s while the page is open: they reach no WebSocket delta.
  import { t, K, fmt, language } from '../stores/i18n.js';
  import { state, wsOpened } from '../stores/state.js';
  import { live } from '../lib/live.js';
  import { href } from '../stores/router.js';
  import { pages } from '../stores/ui.js';
  import { apiPost } from '../lib/api.js';
  import { createSettingSender } from '../lib/settings.js';
  import { toastError } from '../stores/toast.js';
  import { longTail } from '../lib/longtail.js';
  import * as svc from '../lib/service.js';
  import Group from '../m3/Group.svelte';
  import Row from '../m3/Row.svelte';
  import Switch from '../m3/Switch.svelte';
  import StatTile from '../m3/StatTile.svelte';
  import Dialog from '../m3/Dialog.svelte';
  import Button from '../m3/Button.svelte';
  import Reading from './service/Reading.svelte';

  const BOOT = {
    power_on: K('svc.boot_power_on'),
    software: K('svc.boot_software'),
    panic: K('svc.boot_panic'),
    int_wdt: K('svc.boot_int_wdt'),
    task_wdt: K('svc.boot_task_wdt'),
    wdt: K('svc.boot_wdt'),
    brownout: K('svc.boot_brownout'),
    deep_sleep: K('svc.boot_deep_sleep'),
    unknown: K('svc.boot_unknown'),
  };
  const STATUS = ['ok', 'warn', 'bad'];

  $: s = $state;
  // the diagnostics as last read; the WebSocket's copy until the first read lands
  $: d = { ...s, ...$live };
  $: n = (k) => svc.num(d[k]);
  $: f = (k) => Number(d[k]) || 0;
  $: sep = (1.5).toLocaleString($language).charAt(1);
  $: one = (x) => x.toLocaleString($language, { minimumFractionDigits: 1, maximumFractionDigits: 1 }).replace('-', '−');
  $: units = { b: $t['unit.b'], kb: $t['unit.kb'], mb: $t['unit.mb'] };
  $: bytes = (k) => svc.bytes(n(k), units, sep);

  // ── the three that decide a soak ──
  $: cpu = n('audio.cpu_pct');
  $: loop = n('system.main_loop_max_ms');
  $: ram = n('system.heap_internal');

  // ── audio ──
  $: under = n('audio.underruns');
  $: lim = svc.limTenths(d['audio.lim_gr']);
  $: bassGr = f('audio.bass_gr');
  $: clamps = n('audio.out_clamps');
  $: faults = n('audio.dsp_faults');

  // ── the run ──
  $: boot = d['system.boot_reason'] || '';
  $: stalled = d['system.stalled'] || '';
  $: safe = !!d['system.safe_mode'];

  // ── the satellite: "no link" is a fault only once a satellite has said hello (bt.sat) ──
  $: link = !!d['bt.link'];
  $: seen = !!d['bt.sat'];
  $: phone = d['bt.state'] === 'connected' || d['bt.state'] === 'streaming';

  // ── calibration in one line: the speakers' port ──
  $: portLine = s['audio.port_hz'] ? fmt($t['cal.port_is'], { hz: s['audio.port_hz'] }) : $t['cal.port_none'];
  $: tail = longTail($pages);
  $: tailCount = tail.reduce((a, p) => a + p.cards.reduce((b, c) => b + c.widgets.length, 0), 0);

  // ── Bluetooth: the output clock follows the stream ──
  const btClock = createSettingSender('audio.bt_clock_follow', { debounceMs: 0 });

  // ── factory reset ──
  let confirmReset = false;
  let resetting = false;
  // the panel is back once the socket has opened again since it was asked
  let openedAt = 0;
  $: if (resetting && $wsOpened !== openedAt) resetting = false;
  async function factoryReset() {
    confirmReset = false;
    resetting = true;
    openedAt = $wsOpened;
    try {
      await apiPost('/api/factory-reset', {});
    } catch (e) {
      resetting = false;
      toastError(e.message);
    }
  }
</script>

<div class="tiles">
  <StatTile caption={$t['svc.cpu']} value={cpu} unit="%" status={STATUS[svc.cpu(cpu)]} />
  <StatTile caption={$t['svc.loop']} value={loop} unit={$t['unit.ms']} status={STATUS[svc.loopMs(loop)]} />
  <StatTile caption={$t['svc.ram']} value={svc.kib(ram)} unit={$t['unit.kb']} status={STATUS[svc.internalFree(ram)]} />
</div>

<div class="cols">
  <div class="col">
    <Group caps caption={$t['svc.audio']}>
      <Reading label={$t['svc.underruns']} value={under} grade={svc.counter(under)} />
      <Reading label={$t['svc.limiter']} value={`${svc.gainReduction(lim, sep)} ${$t['unit.db']}`} grade={svc.limiter(lim)} />
      <Reading label={$t['svc.bass_lim']} value={`${bassGr > 0.05 ? '−' : ''}${one(bassGr)} ${$t['unit.db']}`} grade={svc.bassGr(bassGr)} />
      <Reading label="DSP" value={`${one(f('audio.dsp_pct'))} %`} grade={svc.dspPct(f('audio.dsp_pct'))} />
      <Reading label={$t['svc.dsp_peak']} value={`${one(f('audio.dsp_max_pct'))} %`} grade={svc.dspMaxPct(f('audio.dsp_max_pct'))} />
      <Reading label={$t['svc.eq_headroom']} value={`${one(f('audio.eq_headroom_db'))} ${$t['unit.db']}`} />
      <Reading label={$t['svc.clamps']} value={clamps} grade={svc.counter(clamps)} />
      <Reading label={$t['svc.faults']} value={faults} grade={svc.counter(faults)} />
    </Group>

    <Group caps caption={$t['svc.run']}>
      <Reading label={$t['svc.uptime']} value={svc.uptime(n('system.uptime'), $t['unit.day'])} />
      <Reading label={$t['svc.boot']} value={BOOT[boot] ? $t[BOOT[boot]] : boot || '—'} grade={svc.bootGrade(boot)} />
      <Reading label={$t['svc.stack']} value={bytes('system.main_stack_min')} grade={svc.stackFree(n('system.main_stack_min'))} />
      <Reading label={$t['svc.stalled']} value={stalled || $t['svc.none']} grade={svc.flag(!!stalled)} />
      <Reading label={$t['svc.safe']} value={safe ? $t['svc.safe_on'] : $t['svc.safe_off']} grade={svc.flag(safe)} />
    </Group>
  </div>

  <div class="col">
    <Group caps caption={$t['svc.memory']}>
      <Reading label={$t['svc.internal']} value={bytes('system.heap_internal')} grade={svc.internalFree(ram)} />
      <Reading label="DMA" value={bytes('system.heap_dma')} grade={svc.dmaFree(n('system.heap_dma'))} />
      <Reading label={$t['svc.largest']} value={bytes('system.heap_largest')} />
      <Reading label="PSRAM" value={bytes('system.heap_psram')} />
      <Reading label={$t['svc.free']} value={bytes('system.heap_free')} />
      <Reading label={$t['svc.internal_min']} value={bytes('system.heap_internal_min')} grade={svc.internalFree(n('system.heap_internal_min'))} />
    </Group>

    <Group caps caption={$t['svc.sat']}>
      <Reading label={$t['svc.link']} value={link ? $t['svc.link_up'] : seen ? $t['svc.link_lost'] : $t['svc.none']}
               grade={!link && seen ? svc.WARN : svc.OK} />
      <Reading label={$t['svc.peer']} value={phone ? $t['svc.peer_on'] : $t['svc.none']} />
      <Reading label="RTT" value={link ? `${n('bt.rtt_ms')} ${$t['unit.ms']}` : '—'} grade={link ? svc.rttMs(n('bt.rtt_ms')) : svc.OK} />
      <Reading label={$t['svc.uart_ovf']} value={n('bt.uart_ovf')} grade={svc.softCounter(n('bt.uart_ovf'))} />
      <Reading label={$t['svc.drops']} value={n('bt.rx_drops')} grade={svc.softCounter(n('bt.rx_drops'))} />
      <Reading label={$t['svc.gaps']} value={n('bt.seq_gaps')} grade={svc.softCounter(n('bt.seq_gaps'))} />
    </Group>
  </div>
</div>

<div class="cols">
  <div class="col">
    <Group caps caption={$t['cal.title']}>
      <Row icon="speaker" title={$t['cal.port']} subtitle={portLine} href={href('system/service/calibration/port')} />
    </Group>
  </div>
  <div class="col">
    <Group caps caption={$t['svc.tools']}>
      <Row icon="screenshot_monitor" title={$t['page.screenshot']} href={href('system/service/screenshot')} />
      <Row icon="article" title={$t['page.loglevel']} href={href('system/service/log')} />
      <Row icon="memory" title={$t['page.hardware']} href={href('system/service/hardware')} />
      <Row icon="sync_alt" title={$t['svc.bt_clock']}>
        <Switch checked={s['audio.bt_clock_follow'] ?? true} label={$t['svc.bt_clock']} on:change={(e) => btClock.send(e.detail)} />
      </Row>
      {#if tailCount}
        <Row icon="view_list" title={$t['page.more']} href={href('system/service/more')} />
      {/if}
      <Row button icon="warning" title={$t['svc.reset']} subtitle={resetting ? $t['sys.restarting'] : ''}
           disabled={resetting} on:click={() => (confirmReset = true)} />
    </Group>
  </div>
</div>

{#if confirmReset}
  <Dialog title={$t['svc.reset_q']} on:close={() => (confirmReset = false)}>
    <p class="b">{$t['svc.reset_lost']}</p>
    <p class="b">{$t['svc.reset_kept']}</p>
    <p class="b">{$t['svc.reset_net']}</p>
    <p class="b"><a href={href('system/backup')} on:click={() => (confirmReset = false)}>{$t['svc.reset_backup']}</a></p>
    <svelte:fragment slot="actions">
      <Button variant="text" on:click={() => (confirmReset = false)}>{$t['sys.cancel']}</Button>
      <Button danger on:click={factoryReset}>{$t['svc.reset_do']}</Button>
    </svelte:fragment>
  </Dialog>
{/if}

<style>
  .tiles {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    gap: 8px;
    padding-top: 8px;
  }
  @media (max-width: 420px) {
    .tiles :global(.num) { font-size: 26px; line-height: 32px; }
    .tiles :global(.cap) { letter-spacing: 1px; }
  }
  .cols { display: grid; grid-template-columns: minmax(0, 1fr); gap: 0 16px; }
  @media (min-width: 900px) {
    .cols { grid-template-columns: repeat(2, minmax(0, 1fr)); align-items: start; }
  }
  .col { display: flex; flex-direction: column; min-width: 0; }
  .b { font-size: 15px; line-height: 22px; color: var(--on-surface-var); }
  .b + .b { margin-top: 10px; }
</style>
