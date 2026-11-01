<script>
  // Налаштування › Тонке налаштування: «Бас і стерео» and «Для фахівців» in one section of
  // their own, names and values only. The web's alone: the glass has the loudness as a switch in
  // «Звук» and none of the rest.
  //
  // «Звичайна гучність» is here too: one slider under the loudness, heard as it moves. It is said
  // on the volume's scale, which «Максимум» compresses (lib/speakers.js refOf/refShown); its top
  // is the old rule, the loudness counted from the top of the scale; it is dimmed while the
  // loudness is off.
  //
  // Two of them follow the port (lib/speakers.js port()): with a port frequency the subsonic sits
  // under it and the slider has nothing to set, so it shows that value, dimmed; the lift acts only
  // with a port, so without one it is dimmed at «вимк.».
  import { onDestroy } from 'svelte';
  import { t, fmt } from '../../stores/i18n.js';
  import { state } from '../../stores/state.js';
  import { createSettingSender } from '../../lib/settings.js';
  import { port, subUnder, refOf, refShown } from '../../lib/speakers.js';
  import Group from '../../m3/Group.svelte';
  import Slider from '../../m3/Slider.svelte';

  const sender = (key) => createSettingSender(key, { debounceMs: 150, live: true });   // sliders
  const loudness = sender('audio.loudness');
  const usual = sender('audio.loud_ref');
  const width = sender('audio.width');
  const monoBass = sender('audio.mono_bass_hz');
  const hp = sender('audio.hp_hz');
  const xo = sender('audio.bass_xo_hz');
  const lift = sender('audio.port_lift_db');
  onDestroy(() => [loudness, usual, width, monoBass, hp, xo, lift].forEach((x) => x.cleanup()));

  $: s = $state;
  $: p = port(s);
  $: volMax = s['audio.vol_max'] ?? 100;
  $: toRef = (v) => (v >= 100 ? 100 : refOf(v, volMax));
  $: hzOrOff = (unit) => (v) => (v ? `${v} ${unit}` : $t['eq.off']);
</script>

<div class="page">
  <Group>
    <div class="set">
      <span class="t">{$t['eq.loudness']}</span>
      <Slider label={$t['eq.loudness']} min={0} max={100} step={10} value={s['audio.loudness'] ?? 70}
              format={(v) => (v ? `${v} %` : $t['eq.off'])} on:input={(e) => loudness.send(e.detail)}
              undo={(v) => loudness.send(v)} />
    </div>
    <div class="set">
      <span class="t">{$t['usual.title']}</span>
      <Slider label={$t['usual.title']} min={refShown(20, volMax)} max={100} step={1}
              value={refShown(s['audio.loud_ref'] ?? 100, volMax)} disabled={!(s['audio.loudness'] ?? 70)}
              format={(v) => `${v} %`} on:input={(e) => usual.send(toRef(e.detail))} undo={(v) => usual.send(toRef(v))} />
    </div>
    <div class="set">
      <span class="t">{$t['eq.width']}</span>
      <Slider label={$t['eq.width']} min={0} max={200} step={5} value={s['audio.width'] ?? 100}
              format={(v) => `${v} %`} on:input={(e) => width.send(e.detail)} undo={(v) => width.send(v)} />
    </div>
    <div class="set">
      <span class="t">{$t['eq.mono_bass']}</span>
      <Slider label={$t['eq.mono_bass']} min={0} max={300} step={10} value={s['audio.mono_bass_hz'] ?? 0}
              format={hzOrOff($t['unit.hz'])} on:input={(e) => monoBass.send(e.detail)} undo={(v) => monoBass.send(v)} />
    </div>
    <div class="set">
      <span class="t">{$t['cal.hp']}</span>
      <Slider label={$t['cal.hp']} min={0} max={80} step={5} value={s['audio.hp_hz'] ?? 40} disabled={p.hz > 0}
              format={p.hz > 0 ? () => fmt($t['spk.d_sub_port'], { hz: subUnder(p.hz) }) : hzOrOff($t['unit.hz'])}
              on:input={(e) => hp.send(e.detail)} undo={(v) => hp.send(v)} />
    </div>
    <div class="set">
      <span class="t">{$t['cal.xo']}</span>
      <Slider label={$t['cal.xo']} min={0} max={250} step={10} value={s['audio.bass_xo_hz'] ?? 120}
              format={hzOrOff($t['unit.hz'])} on:input={(e) => xo.send(e.detail)} undo={(v) => xo.send(v)} />
    </div>
    <div class="set">
      <span class="t">{$t['cal.lift']}</span>
      <Slider label={$t['cal.lift']} min={0} max={6} step={1} value={s['audio.port_lift_db'] ?? 3} disabled={!p.hz}
              format={p.hz ? (v) => `${v} ${$t['unit.db']}` : () => $t['eq.off']}
              on:input={(e) => lift.send(e.detail)} undo={(v) => lift.send(v)} />
    </div>
  </Group>
</div>

<style>
  .page { max-width: 640px; padding-bottom: 24px; }
  .set { padding: 12px 16px 8px; display: flex; flex-direction: column; }
  .t { font-size: 16px; line-height: 22px; color: var(--on-surface); }
</style>
