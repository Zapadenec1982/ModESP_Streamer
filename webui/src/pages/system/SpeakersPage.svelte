<script>
  // Налаштування › Колонки: what has been done
  // about the speakers, said in a line, and the way to do it again; the balance, which moved here
  // from Service; and the details — each number with where it came from — and the equalizer, named
  // as what it is (a «Корекція колонок» would lead to the same EQ the presets change, so a
  // correction tuned there would go with the next preset). `sub`: 'setup', the wizard,
  // and 'details', each a page under this one.
  // The rules are the device's and lib/speakers.js says them: nothing here writes without a tap.
  import { onDestroy } from 'svelte';
  import { state } from '../../stores/state.js';
  import { t, fmt, K } from '../../stores/i18n.js';
  import { href } from '../../stores/router.js';
  import { createSettingSender } from '../../lib/settings.js';
  import { speakerState, port, subUnder, SPK_PORTED, SPK_CLOSED, SRC_MEASURED } from '../../lib/speakers.js';
  import Icon from '../../m3/Icon.svelte';
  import Button from '../../m3/Button.svelte';
  import Group from '../../m3/Group.svelte';
  import Row from '../../m3/Row.svelte';
  import CenterSlider from '../../m3/CenterSlider.svelte';
  import Wizard from '../speakers/Wizard.svelte';

  /** '' | 'setup' | 'details' — the route's third segment */
  export let sub = '';

  // the keys lib/speakers.js names for the state card (it has no imports, so it cannot say K)
  K('spk.ported'); K('spk.closed'); K('spk.unset');
  K('spk.port_measured'); K('spk.port_manual'); K('spk.port_none');

  $: s = $state;
  $: st = speakerState(s);
  $: p = port(s);
  $: type = s['audio.spk_type'] ?? 0;
  $: closed = type === SPK_CLOSED;

  // the balance, in whole dB here (the key keeps tenths: audio.balance_db10)
  const balance = createSettingSender('audio.balance_db10', { debounceMs: 120, live: true });
  onDestroy(() => balance.cleanup());
  $: bal = Math.round((s['audio.balance_db10'] ?? 0) / 5) / 2;
  $: balText = (v) => (v === 0 ? $t['spk.center'] : fmt(v < 0 ? $t['spk.left'] : $t['spk.right'], { n: Math.abs(v) }));

  $: src = s['audio.port_src'] === SRC_MEASURED ? $t['spk.src_measured'] : $t['spk.src_manual'];
  $: detailsValue = closed ? $t['spk.closed'] : p.hz ? fmt($t['spk.port_short'], { hz: p.hz, src }) : '';
  $: typeText = type === SPK_PORTED ? $t['spk.t_ported'] : closed ? $t['spk.t_closed'] : $t['usual.unset'];
  $: hp = s['audio.hp_hz'] ?? 40;
  $: subText = p.hz ? fmt($t['spk.d_sub_port'], { hz: subUnder(p.hz) }) : hp ? `${hp} ${$t['unit.hz']}` : $t['cal.off'];
  $: liftText = p.hz ? `+${s['audio.port_lift_db'] ?? 3} ${$t['unit.db']}` : $t['spk.d_off'];
  $: portText = closed ? $t['spk.d_off'] : p.hz ? fmt($t['spk.port_short'], { hz: p.hz, src }) : $t['cal.port_none'];
</script>

{#if sub === 'setup'}
  <Wizard />
{:else if sub === 'details'}
  <div class="page">
    <Group>
      <Row icon="speaker" title={$t['spk.d_type']} subtitle={typeText} />
      <Row icon="graphic_eq" title={$t['spk.d_port']} subtitle={portText} />
      <Row icon="tune" title={$t['cal.hp']} subtitle={subText} />
      <Row icon="tune" title={$t['cal.lift']} subtitle={liftText} />
    </Group>
    <Group>
      <Row icon="graphic_eq" title={$t['spk.d_find']} href={href('system/service/calibration/port')} />
    </Group>
  </div>
{:else}
  <div class="page">
    <section class="state" aria-live="polite">
      <!-- two speakers, the port drawn when they have one -->
      <svg viewBox="0 0 220 112" width="220" height="112" aria-hidden="true">
        {#each [44, 118] as x}
          <rect x={x} y="4" width="58" height="104" rx="9" class="box" />
          <circle cx={x + 29} cy={closed ? 26 : 22} r="6" class="ln" />
          <circle cx={x + 29} cy={closed ? 64 : 56} r="18" class="ln" />
          <circle cx={x + 29} cy={closed ? 64 : 56} r="6" class="dot" />
          {#if !closed}<circle cx={x + 29} cy="92" r="6" class="port" />{/if}
        {/each}
      </svg>
      <h2 class="t">{$t[st.title]}</h2>
      {#if st.line}
        <p class="line">
          {#if p.measured && !closed}<span class="ok"><Icon name="check_circle" size={18} fill /></span>{/if}
          {fmt($t[st.line], st.args)}
        </p>
      {/if}
      <Button variant="tonal" href={href('system/speakers/setup')}>{st.again ? $t['spk.setup_again'] : $t['spk.setup']}</Button>
    </section>

    <Group caption={$t['spk.placement']}>
      <div class="bal">
        <div class="bal-h"><span>{$t['spk.balance']}</span><span class="v">{balText(bal)}</span></div>
        <CenterSlider label={$t['spk.balance']} value={bal} min={-6} max={6} step={0.5}
                      ends={[$t['spk.l'], $t['spk.r']]} format={balText}
                      on:input={(e) => balance.send(Math.round(e.detail * 10))}
                      undo={(v) => balance.send(Math.round(v * 10))} />
      </div>
    </Group>

    <Group>
      <Row title={$t['page.eq']} href={href('system/sound/eq')} />
      <Row title={$t['spk.details']} subtitle={detailsValue} href={href('system/speakers/details')} />
    </Group>
  </div>
{/if}

<style>
  .page { max-width: 640px; display: flex; flex-direction: column; gap: 4px; padding-bottom: 24px; }
  .state {
    background: var(--surface-low);
    border-radius: var(--r-l);
    padding: 26px 24px 24px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    text-align: center;
    margin-top: 8px;
  }
  .box { fill: var(--surface-high); stroke: var(--on-surface-var); stroke-width: 2; }
  .ln { fill: none; stroke: var(--on-surface-var); stroke-width: 2; }
  .dot { fill: var(--on-surface-var); }
  .port { fill: var(--surface-lowest); stroke: var(--on-surface-var); stroke-width: 2; }
  .t { margin-top: 10px; font-size: 22px; line-height: 28px; font-weight: 500; }
  .line { display: flex; align-items: center; gap: 6px; font-size: 15px; line-height: 20px; color: var(--on-surface-var); }
  .ok { color: var(--tertiary); display: flex; }
  .bal { padding: 14px 16px 6px; }
  .bal-h { display: flex; justify-content: space-between; align-items: baseline; font-size: 16px; }
  .bal-h .v { font-size: 15px; color: var(--on-surface-var); }
</style>
