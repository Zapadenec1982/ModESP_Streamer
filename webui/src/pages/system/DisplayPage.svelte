<script>
  // Налаштування › Екран, as on the glass (screen_display.cpp): the brightness, the standby face's
  // level while music plays, the auto-sleep and the scheme. As a group on Налаштування itself it
  // would make that page the longest of the web; the glass has it as a page of its own too.
  import { onDestroy } from 'svelte';
  import { t, fmt } from '../../stores/i18n.js';
  import { state } from '../../stores/state.js';
  import { createSettingSender } from '../../lib/settings.js';
  import { SCHEMES, schemeKey } from '../../lib/schemes.js';
  import Group from '../../m3/Group.svelte';
  import Row from '../../m3/Row.svelte';
  import Slider from '../../m3/Slider.svelte';

  // the panel's auto-sleep steps (screen_display.cpp SLEEP_STEPS)
  const SLEEP = [0, 5, 10, 15, 20, 30, 45, 60];

  $: s = $state;
  $: scheme = s['ui.theme'] ?? 0;

  const backlight = createSettingSender('ui.backlight', { debounceMs: 150, live: true });
  const standbyBl = createSettingSender('ui.standby_bl', { debounceMs: 150, live: true });
  const now = (key) => createSettingSender(key, { debounceMs: 0 });
  const sleep = now('ui.standby_min');
  const theme = now('ui.theme');
  onDestroy(() => [backlight, standbyBl, sleep, theme].forEach((x) => x.cleanup()));
</script>

<div class="page">
  <Group>
    <div class="set">
      <span class="t">{$t['sys.brightness']}</span>
      <Slider label={$t['sys.brightness']} min={0} max={100} step={5} value={s['ui.backlight'] ?? 80}
              icon="brightness_6" format={(v) => `${v} %`} on:input={(e) => backlight.send(e.detail)} />
    </div>
    <div class="set">
      <span class="t">{$t['sys.standby_bl']}</span>
      <Slider label={$t['sys.standby_bl']} min={5} max={100} step={5} value={s['ui.standby_bl'] ?? 25}
              icon="graphic_eq" format={(v) => `${v} %`} on:input={(e) => standbyBl.send(e.detail)} />
    </div>
    <Row icon="bedtime" title={$t['sys.sleep']}>
      <select class="pick" aria-label={$t['sys.sleep']} value={s['ui.standby_min'] ?? 5}
              on:change={(e) => sleep.send(Number(e.target.value))}>
        {#each SLEEP as m}<option value={m}>{m ? fmt($t['sys.minutes'], { n: m }) : $t['sys.never']}</option>{/each}
      </select>
    </Row>
    <Row icon="palette" title={$t['sys.scheme']} subtitle={$t[schemeKey(scheme)]}>
      <div class="swatches" role="radiogroup" aria-label={$t['sys.scheme']}>
        {#each SCHEMES as sc}
          <button type="button" class="sw" class:on={sc.v === scheme} style="--c:{sc.c}" role="radio"
                  aria-checked={sc.v === scheme ? 'true' : 'false'} aria-label={$t[sc.key]}
                  on:click={() => theme.send(sc.v)}></button>
        {/each}
      </div>
    </Row>
  </Group>
</div>

<style>
  .page { max-width: 640px; padding-bottom: 24px; }
  .set { padding: 12px 16px 8px; display: flex; flex-direction: column; }
  .t { font-size: 16px; line-height: 22px; color: var(--on-surface); }
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
  .swatches { display: flex; gap: 12px; flex: none; }
  .sw {
    width: 32px;
    height: 32px;
    border-radius: 16px;
    border: none;
    padding: 0;
    background: var(--c);
    outline: 2px solid transparent;
    outline-offset: 3px;
    cursor: pointer;
  }
  .sw.on { outline-color: var(--on-surface); }
</style>
