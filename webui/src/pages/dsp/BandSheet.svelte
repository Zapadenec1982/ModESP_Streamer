<script>
  // A band, opened from the equalizer's list: its
  // type, chosen from a list of the six as the glass offers them and by the names REW and
  // Equalizer APO use, then its frequency, gain and width with − and +, and «Готово». «Увімкнена»
  // on top, as the glass's band panel has it: off keeps the numbers (`turn`); a step on a band
  // that is off switches it on, as touching its point does on both (`edit`). Each step is heard
  // at once; the steps are the panel's (screen_eq.cpp adjust): 1/12 octave, 0.5 dB, 0.1 of Q, a
  // pass's slope along PASS_Q.
  import { createEventDispatcher } from 'svelte';
  import { t, K, fmt, language } from '../../stores/i18n.js';
  import { decimal } from '../../lib/numbers.js';
  import { withType, passQIndex, PASS_Q, Q_LR12, Q_BW12, F_MIN, F_MAX, GAIN_MAX, Q_MIN, Q_MAX, SHELF_Q_MAX } from '../../lib/eqband.js';
  import { hasGain, isPass } from '../../lib/eqmath.js';
  import BottomSheet from '../../m3/BottomSheet.svelte';
  import Button from '../../m3/Button.svelte';
  import Switch from '../../m3/Switch.svelte';
  import StepRow from './StepRow.svelte';

  export let band;
  /** Its number: its place in the set, as the glass labels its point. */
  export let n = 1;

  const dispatch = createEventDispatcher();
  // in BandType's order (audio_dsp/dsp_chain.h), as the glass's list has them
  const TYPES = [['peak', K('eq.type.peak')], ['lowshelf', K('eq.type.lowshelf')], ['highshelf', K('eq.type.highshelf')],
                 ['lowpass', K('eq.type.lowpass')], ['highpass', K('eq.type.highpass')], ['notch', K('eq.type.notch')]];

  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
  const edit = (b) => dispatch('edit', { ...b, on: true });

  function step(which, dir) {
    const b = { ...band };
    if (which === 'f') {
      b.f = clamp(Math.round(band.f * (dir > 0 ? 1.0595 : 1 / 1.0595) * 10) / 10, F_MIN, F_MAX);
    } else if (which === 'gain') {
      b.gain = clamp(Math.round((band.gain + dir * 0.5) * 10) / 10, -GAIN_MAX, GAIN_MAX);
    } else if (isPass(band.type)) {
      b.q = PASS_Q[clamp(passQIndex(band.q) + dir, 0, PASS_Q.length - 1)];
    } else {
      const max = band.type === 'lowshelf' || band.type === 'highshelf' ? SHELF_Q_MAX : Q_MAX;
      b.q = clamp(Math.round((band.q + dir * 0.1) * 100) / 100, Q_MIN, max);
    }
    edit(b);
  }
  // typed: lib/eq.js holds every number to the device's limits
  const typed = (which, v) => edit({ ...band, [which]: v });

  $: fText = band.f >= 1000 ? `${decimal(band.f / 1000, 2, $language)} ${$t['unit.khz']}` : `${band.f.toFixed(0)} ${$t['unit.hz']}`;
  $: gText = hasGain(band.type) ? `${band.gain > 0 ? '+' : ''}${decimal(band.gain, 1, $language)} ${$t['unit.db']}` : '—';
  // a named alignment is a slope, anything else a width (screen_eq.cpp q_text)
  $: named = isPass(band.type) && Math.abs(band.q - Q_LR12) < 0.02 ? 'LR 12'
    : isPass(band.type) && Math.abs(band.q - Q_BW12) < 0.02 ? 'BW 12' : '';
  // the glass's caption (screen_eq.cpp q_text): a slope where it is one, a width everywhere else
  $: qLabel = named ? $t['eq.slope'] : $t['eq.q_width'];
</script>

<BottomSheet title={fmt($t['eq.band_n'], { n })} on:close>
  <div class="on">
    <span class="tl">{$t['eq.band_on']}</span>
    <Switch checked={band.on} label={$t['eq.band_on']} on:change={(e) => dispatch('turn', e.detail)} />
  </div>
  <div class="type">
    <span class="tl">{$t['eq.type']}</span>
    <select class="pick" aria-label={$t['eq.type']} value={band.type}
            on:change={(e) => e.target.value !== band.type && edit(withType(band, e.target.value))}>
      {#each TYPES as [ty, key] (ty)}<option value={ty}>{$t[key]}</option>{/each}
    </select>
  </div>
  <StepRow label={$t['eq.freq']} text={fText} raw={Math.round(band.f)} decLabel={$t['eq.less']} incLabel={$t['eq.more_step']}
           on:dec={() => step('f', -1)} on:inc={() => step('f', 1)} on:set={(e) => typed('f', e.detail)} />
  <StepRow label={$t['eq.gain']} text={gText} raw={decimal(band.gain, 1, $language)} disabled={!hasGain(band.type)} signed
           decLabel={$t['eq.less']} incLabel={$t['eq.more_step']}
           on:dec={() => step('gain', -1)} on:inc={() => step('gain', 1)} on:set={(e) => typed('gain', e.detail)} />
  <StepRow label={qLabel} text={named || decimal(band.q, 2, $language)} raw={decimal(band.q, 2, $language)} decLabel={$t['eq.less']} incLabel={$t['eq.more_step']}
           on:dec={() => step('q', -1)} on:inc={() => step('q', 1)} on:set={(e) => typed('q', e.detail)} />
  <div class="acts">
    <Button on:click={() => dispatch('close')}>{$t['eq.done']}</Button>
  </div>
</BottomSheet>

<style>
  .on, .type { display: flex; align-items: center; gap: 16px; min-height: 48px; }
  .tl { flex: 1; font-size: 16px; color: var(--on-surface); }
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
    font-weight: 500;
  }
  .pick option { background: var(--surface-container); color: var(--on-surface); }
  .acts { display: flex; align-items: center; justify-content: space-between; margin-top: 16px; }
</style>
