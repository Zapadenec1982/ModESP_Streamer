<script>
  // Налаштування › Кросовер: the four ways the
  // outputs can be used — Стерео, + сабвуфер, Бі-ампінг, 4 колонки — each drawn as the mock-up draws
  // it. A scheme works only once the firmware says it plays it (audio.out_caps, lib/xover.js); the
  // rest say «ще не підтримується». This board has one stereo pair, so stereo plays, and the page
  // says why it needs no crossover. The schemes' own controls come with the four-channel DAC plan.
  import { state } from '../../stores/state.js';
  import { t, K } from '../../stores/i18n.js';
  import { schemesOf, playing } from '../../lib/xover.js';

  const NAMES = { stereo: K('xover.stereo'), sub: K('xover.sub_short'), biamp: K('xover.biamp'), four: K('xover.four') };

  $: schemes = schemesOf($state['audio.out_caps']);
  $: now = playing($state['audio.out_caps']);
</script>

<div class="page">
  <div class="grid" role="radiogroup" aria-label={$t['xover.title']}>
    {#each schemes as s (s.id)}
      <div class="card" class:on={s.id === now} class:off={!s.supported} role="radio"
           aria-checked={s.id === now} aria-disabled={!s.supported}>
        <svg viewBox="0 0 60 36" width="60" height="36" aria-hidden="true">
          {#if s.id === 'stereo'}
            <rect x="10" y="4" width="14" height="28" rx="3" /><rect x="36" y="4" width="14" height="28" rx="3" />
            <circle cx="17" cy="22" r="4" class="thin" /><circle cx="43" cy="22" r="4" class="thin" />
          {:else if s.id === 'sub'}
            <rect x="4" y="4" width="12" height="24" rx="3" /><rect x="44" y="4" width="12" height="24" rx="3" />
            <rect x="21" y="16" width="18" height="17" rx="3" /><circle cx="30" cy="25" r="4.5" class="thin" />
          {:else if s.id === 'biamp'}
            <rect x="22" y="2" width="16" height="32" rx="3" /><circle cx="30" cy="9" r="2.5" class="thin" />
            <circle cx="30" cy="23" r="5" class="thin" /><path d="M8,9 H22 M8,23 H22" class="thin" />
            <rect x="2" y="5" width="6" height="8" rx="1" class="solid" /><rect x="2" y="19" width="6" height="8" rx="1" class="solid" />
          {:else}
            <rect x="12" y="2" width="11" height="14" rx="2" /><rect x="37" y="2" width="11" height="14" rx="2" />
            <rect x="12" y="20" width="11" height="14" rx="2" /><rect x="37" y="20" width="11" height="14" rx="2" />
          {/if}
        </svg>
        <span class="name">{$t[NAMES[s.id]]}</span>
        {#if !s.supported}<span class="later">{$t['xover.unsupported']}</span>{/if}
      </div>
    {/each}
  </div>

  {#if now === 'stereo'}
    <div class="note">
      <h2>{$t['xover.stereo_title']}</h2>
    </div>
  {/if}
</div>

<style>
  .page { max-width: 640px; padding: 8px 0 24px; }
  .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
  .card {
    min-height: 104px;
    border-radius: var(--r-s);
    border: 2px solid transparent;
    background: var(--surface-low);
    color: var(--on-surface);
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 6px;
    padding: 10px 4px;
    text-align: center;
  }
  .card.on { border-color: var(--primary); background: var(--surface-container); }
  svg rect, svg circle, svg path { fill: none; stroke: var(--on-surface-var); stroke-width: 2; }
  svg .thin { stroke-width: 1.5; }
  svg .solid { fill: var(--on-surface-var); stroke: none; }
  .on svg rect, .on svg circle, .on svg path { stroke: var(--primary); }
  .on svg .solid { fill: var(--primary); }
  .off { color: var(--outline); }
  .off svg { opacity: 0.45; }
  .name { font-size: 13px; line-height: 17px; font-weight: 500; }
  /* Two by two where four in a row would not hold the longest line: «jeszcze nieobsługiwane» is
     115 px, so a card wants 123 and four of them a 550-px page. At 375 px four in a row left 76 px
     a card: the Polish ran out of it and «4 Lautsprecher» broke after the 4. */
  @media (max-width: 600px) {
    .grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  }
  .later { font-size: 11px; line-height: 14px; color: var(--outline); }
  .note {
    margin-top: 16px;
    background: var(--surface-low);
    border-radius: var(--r-m);
    padding: 18px;
  }
  .note h2 { font-size: 17px; line-height: 24px; font-weight: 500; margin-bottom: 4px; }
</style>
