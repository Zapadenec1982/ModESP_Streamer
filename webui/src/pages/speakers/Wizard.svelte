<script>
  // The speakers' wizard. Step 1 asks what
  // the speakers are; closed ones need nothing more and «Готово» says so. Step 2 finds the port —
  // here with the panel's microphone on a bench, or by hand on the calibration page; the phone's
  // microphone is the app's. Nothing is written by a choice, a skip or a measurement: only
  // «Готово» and «Прийняти» write (lib/speakers.js wizardWrites, tested).
  import { onDestroy } from 'svelte';
  import { state } from '../../stores/state.js';
  import { t, fmt } from '../../stores/i18n.js';
  import { href, done } from '../../stores/router.js';
  import { live } from '../../lib/live.js';
  import { createSettingSender, writeNow } from '../../lib/settings.js';
  import { toastError } from '../../stores/toast.js';
  import { wizardWrites, meterOutcome, port, SPK_PORTED, SPK_CLOSED } from '../../lib/speakers.js';
  import Icon from '../../m3/Icon.svelte';
  import Button from '../../m3/Button.svelte';
  import Chip from '../../m3/Chip.svelte';

  let step = 1;
  let type = null;              // SPK_PORTED | SPK_CLOSED, chosen on step 1
  let help = false;
  let asked = false;            // this visit started the measurement: its outcome is ours to show

  $: s = $state;
  // the meter's words come without WebSocket deltas: read with the poll while this page is open
  $: ro = (k) => ($live[k] !== undefined ? $live[k] : s[k]);
  $: measuring = !!s['audio.port_meas'];
  $: status = String(ro('audio.port_meas_status') || '');
  $: outcome = asked || measuring ? meterOutcome(status, measuring) : 'idle';
  $: found = Number(ro('audio.port_meas_hz')) || 0;
  $: hz = Math.round(found);
  $: progress = (() => {
    const m = /k=(\d+)\x1fn=(\d+)/.exec(status);
    return m ? Math.round((100 * Number(m[1])) / Number(m[2])) : 0;
  })();
  $: now = port(s);
  $: ch = s['audio.port_meas_ch'] ?? 1;
  const chSend = createSettingSender('audio.port_meas_ch', { debounceMs: 0 });
  onDestroy(() => chSend.cleanup());

  async function act(fn) {
    try { await fn(); } catch (e) { toastError(e.message); }
  }
  // Written, then back to «Колонки». Not written: the wizard stays where it was, with the error,
  // to be accepted again — «Колонки» would show what the device never took.
  async function write(action) {
    const keys = wizardWrites(type, action, found);
    if (keys) {
      try {
        await writeNow(keys);
      } catch (e) {
        toastError(e.message);
        return;
      }
    }
    done('system/speakers');
  }
  const meter = (on) => act(async () => {
    if (on) asked = true;
    await writeNow({ 'audio.port_meas': on });      // shown at once, taken back if refused
  });
  const leave = () => done('system/speakers');
</script>

<div class="wiz">
  <div class="bar" aria-hidden="true"><span class="on"></span><span class:on={step === 2}></span></div>
  <p class="count">{fmt($t['spk.step'], { i: step, n: 2 })}</p>

  {#if step === 1}
    <h2 class="q">{$t['spk.q_type']}</h2>
    <div class="types" role="radiogroup" aria-label={$t['spk.d_type']}>
      <button type="button" role="radio" class="type" class:on={type === SPK_PORTED} aria-checked={type === SPK_PORTED}
              on:click={() => (type = SPK_PORTED)}>
        <svg viewBox="0 0 60 100" width="54" height="90" aria-hidden="true">
          <rect x="2" y="2" width="56" height="96" rx="9" class="box" />
          <circle cx="30" cy="19" r="6" class="ln" /><circle cx="30" cy="50" r="17" class="ln" />
          <circle cx="30" cy="50" r="5" class="dot" /><circle cx="30" cy="84" r="7" class="port" />
        </svg>
        <span class="tt"><span class="tn">{$t['spk.t_ported']}</span><span class="tx">{$t['spk.t_ported_x']}</span></span>
        <span class="radio">{#if type === SPK_PORTED}<Icon name="check" size={16} />{/if}</span>
      </button>
      <button type="button" role="radio" class="type" class:on={type === SPK_CLOSED} aria-checked={type === SPK_CLOSED}
              on:click={() => (type = SPK_CLOSED)}>
        <svg viewBox="0 0 60 100" width="54" height="90" aria-hidden="true">
          <rect x="2" y="2" width="56" height="96" rx="9" class="box" />
          <circle cx="30" cy="22" r="6" class="ln" /><circle cx="30" cy="60" r="19" class="ln" />
          <circle cx="30" cy="60" r="6" class="dot" />
        </svg>
        <span class="tt"><span class="tn">{$t['spk.t_closed']}</span><span class="tx">{$t['spk.t_closed_x']}</span></span>
        <span class="radio">{#if type === SPK_CLOSED}<Icon name="check" size={16} />{/if}</span>
      </button>
    </div>
    <button type="button" class="how" aria-expanded={help} on:click={() => (help = !help)}>{$t['spk.how']}</button>
    {#if help}
      <div class="help">
        <p>{$t['spk.how_1']}</p>
        <p>{$t['spk.how_2']}</p>
        <p>{$t['spk.how_3']}</p>
      </div>
    {/if}
    <div class="acts">
      {#if type === SPK_CLOSED}
        <Button on:click={() => write('done')}>{$t['spk.done']}</Button>
      {:else}
        <Button disabled={type === null} on:click={() => (step = 2)}>{$t['spk.next']}</Button>
      {/if}
      <Button variant="text" on:click={leave}>{$t['spk.skip']}</Button>
    </div>

  {:else if outcome === 'found'}
    <span class="big ok"><Icon name="check_circle" size={56} fill /></span>
    <h2 class="q" role="status">{fmt($t['spk.result'], { hz })}</h2>
    <div class="table">
      <div class="tr"><span>{$t['spk.now']}</span><span class="dim">{now.hz ? `${now.hz} ${$t['unit.hz']}` : $t['cal.port_none']}</span></div>
      <div class="tr"><span>{$t['spk.after']}</span><span>{fmt($t['spk.after_v'], { hz })}</span></div>
    </div>
    <div class="acts">
      <Button on:click={() => write('accept')}>{$t['spk.accept']}</Button>
      <Button variant="outlined" on:click={() => write('keep')}>{$t['spk.keep']}</Button>
    </div>

  {:else if outcome === 'failed' || outcome === 'doubtful'}
    <span class="big warn"><Icon name="warning" size={40} fill /></span>
    <h2 class="q">{$t['spk.failed']}</h2>
    {#if outcome === 'doubtful' && hz}<p class="x">{fmt($t['spk.doubtful'], { hz })}</p>{/if}
    <p class="x">{$t['spk.failed_x']}</p>
    <ul class="why">
      <li>{$t['spk.why_noise']}</li>
      <li>{$t['spk.why_far']}</li>
      <li>{$t['spk.why_closed']}</li>
    </ul>
    <div class="acts">
      <Button on:click={() => meter(true)}>{$t['spk.retry']}</Button>
      <Button variant="outlined" href={href('system/service/calibration/port')}>{$t['spk.by_hand']}</Button>
      <Button variant="text" on:click={() => write('keep')}>{$t['spk.keep']}</Button>
    </div>

  {:else if outcome === 'running'}
    <h2 class="q">{$t['spk.q_listen']}</h2>
    <div class="ring" role="status" aria-label={$t['spk.listening']} style="--p:{progress}">
      <span>{progress} %</span>
    </div>
    <p class="x center">{$t['spk.listening']}</p>
    <div class="acts">
      <Button variant="outlined" icon="stop" on:click={() => meter(false)}>{$t['spk.stop']}</Button>
    </div>

  {:else}
    <h2 class="q">{$t['spk.q_listen']}</h2>
    <p class="app"><Icon name="smartphone" size={18} />{$t['spk.phone']}</p>
    <div class="how-m">
      <h3>{$t['spk.by_panel']}</h3>
      <p class="x">{$t['spk.by_panel_x']}</p>
      <div class="chips">
        <Chip button selected={ch === 1} on:click={() => chSend.send(1)}>{$t['cal.speaker_left']}</Chip>
        <Chip button selected={ch === 2} on:click={() => chSend.send(2)}>{$t['cal.speaker_right']}</Chip>
      </div>
      <p class="x">{$t['spk.start_x']}</p>
    </div>
    <div class="acts">
      <Button icon="play_arrow" on:click={() => meter(true)}>{$t['spk.start']}</Button>
      <Button variant="outlined" href={href('system/service/calibration/port')}>{$t['spk.by_hand']}</Button>
      <!-- a wrong type is put right here, the choice kept; ← leaves the wizard -->
      <Button variant="text" icon="arrow_back" on:click={() => (step = 1)}>{$t['common.back']}</Button>
      <Button variant="text" on:click={leave}>{$t['spk.skip']}</Button>
    </div>
  {/if}
</div>

<style>
  .wiz { max-width: 560px; margin: 0 auto; padding: 4px 4px 24px; display: flex; flex-direction: column; }
  .bar { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }
  .bar span { height: 4px; border-radius: 2px; background: var(--surface-highest); }
  .bar span.on { background: var(--primary); }
  .count { align-self: flex-end; font-size: 14px; color: var(--outline); margin: 6px 0 0; }
  .q { font-size: 28px; line-height: 34px; font-weight: 500; margin: 20px 0 8px; }
  .x { font-size: 16px; line-height: 24px; color: var(--on-surface-var); margin: 0 0 8px; }
  .center { text-align: center; }
  .types { display: flex; flex-direction: column; gap: 12px; margin-top: 16px; }
  .type {
    display: flex;
    align-items: center;
    gap: 18px;
    min-height: 120px;
    padding: 0 18px 0 22px;
    border-radius: 22px;
    border: 2px solid var(--surface-low);
    background: var(--surface-low);
    color: var(--on-surface);
    font: inherit;
    text-align: left;
  }
  .type.on { border-color: var(--primary); background: var(--surface-container); }
  .box { fill: var(--surface-high); stroke: var(--on-surface-var); stroke-width: 2; }
  .ln { fill: none; stroke: var(--on-surface-var); stroke-width: 2; }
  .dot { fill: var(--on-surface-var); }
  .port { fill: var(--surface-lowest); stroke: var(--primary); stroke-width: 2.5; }
  .tt { flex: 1; display: flex; flex-direction: column; gap: 4px; }
  .tn { font-size: 18px; font-weight: 500; }
  .tx { font-size: 14px; line-height: 20px; color: var(--outline); }
  .radio {
    width: 24px;
    height: 24px;
    border-radius: 12px;
    border: 2px solid var(--outline-var);
    flex: none;
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--on-primary);
  }
  .type.on .radio { border-color: var(--primary); background: var(--primary); }
  .how {
    align-self: center;
    min-height: 44px;
    margin-top: 8px;
    padding: 0 16px;
    border: 0;
    background: none;
    color: var(--primary);
    font: inherit;
    font-size: 15px;
    font-weight: 500;
  }
  .help {
    border-radius: 18px;
    background: var(--surface-low);
    padding: 14px 16px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    font-size: 14px;
    line-height: 20px;
    color: var(--on-surface-var);
  }
  .acts { display: flex; flex-direction: column; align-items: stretch; gap: 8px; margin-top: 24px; }
  .big { align-self: center; display: flex; margin-top: 16px; }
  .ok { color: var(--tertiary); }
  .warn { color: var(--primary); align-self: flex-start; }
  .table { background: var(--surface-low); border-radius: 20px; padding: 4px 18px; margin-top: 12px; }
  .tr { display: flex; justify-content: space-between; align-items: center; min-height: 52px; font-size: 16px; }
  .tr + .tr { border-top: 1px solid var(--surface-high); }
  .dim { color: var(--outline); }
  .why { list-style: none; padding: 6px 18px; margin: 8px 0 0; background: var(--surface-low); border-radius: 20px; }
  .why li { padding: 12px 0; font-size: 15px; line-height: 21px; }
  .why li + li { border-top: 1px solid var(--surface-high); }
  .ring {
    align-self: center;
    margin: 24px 0 8px;
    width: 120px;
    height: 120px;
    border-radius: 50%;
    background: conic-gradient(var(--primary) calc(var(--p) * 1%), var(--surface-high) 0);
    display: grid;
    place-items: center;
  }
  .ring span {
    width: 104px;
    height: 104px;
    border-radius: 50%;
    background: var(--surface);
    display: grid;
    place-items: center;
    font-size: 26px;
    font-weight: 300;
    font-variant-numeric: tabular-nums;
  }
  .app {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    align-self: flex-start;
    padding: 6px 12px 6px 10px;
    border-radius: 16px;
    background: var(--surface-container);
    color: var(--on-surface-var);
    font-size: 13px;
    line-height: 18px;
    margin: 4px 0 8px;
  }
  .how-m { background: var(--surface-low); border-radius: 20px; padding: 14px 18px 6px; margin-top: 8px; }
  .how-m h3 { font-size: 16px; font-weight: 500; margin: 0 0 6px; }
  .chips { display: flex; gap: 8px; margin: 4px 0 10px; }
</style>
