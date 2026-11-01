<script>
  // The DSP page's plot (screen_eq.cpp): 20 Hz – 20 kHz on a log axis, ±12 dB — ±24 once a pass
  // or a notch is on, so its knee is not flattened against the floor — the summed curve from the
  // firmware's own design (lib/eqmath.js), and a handle per band at the band's own response at
  // its own frequency, which is where a shelf or a pass actually is.
  //
  // The bands that are on have a point, labelled with the band's place in the set as the glass
  // labels it; one that is off is not drawn (deleting a band turns it off). The curve has its
  // area filled to 0 dB; with `stages` (lib/syscurve.js) the whole system's curve is over it, dashed.
  //
  // A band is taken by its point only (lib/gesture.js, the customer, 14.10.2026): a finger
  // anywhere else on the plot scrolls the page. On a point it picks the band, and the point moves
  // once it has been held HOLD_MS or the finger went SLOP px away; a mouse takes it at once. x is
  // frequency, y is gain (twice the target for a shelf, whose corner carries half of it; nothing
  // for a pass or a notch). `grab` fires when a band starts to move, `release` when it is let go.
  import { createEventDispatcher, onDestroy } from 'svelte';
  import { curveDb, bandMagnitudeDb, hasGain } from '../../lib/eqmath.js';
  import { pointGesture, HOLD_MS } from '../../lib/gesture.js';
  import { systemCurveDb } from '../../lib/syscurve.js';

  export let bands = [];
  export let fs = 48000;
  export let sel = 0;
  /** PEQ on: the curve is what is heard. Off, it is what switching on would do — drawn grey. */
  export let on = true;
  export let label = '';
  /** The stages around the EQ (lib/syscurve.js systemBands): the whole system is drawn too; null: not. */
  export let stages = null;

  const dispatch = createEventDispatcher();

  const F_LO = 20, DECADES = 3;
  const N = 193;                  // the panel's 97 points and one between each pair
  const GRID_F = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
  const GRID_L = ['50', '100', '200', '500', '1k', '2k', '5k', '10k'];
  const LEFT = 34, RIGHT = 10, TOP = 12, BOTTOM = 24;
  const R = 13;                   // a handle's radius
  const HIT = 24;                 // how far from a handle's centre a finger takes it

  let width = 320;
  let height = 240;
  let svg;
  let drag = -1;                  // the band that moves
  let lifted = -1;                // the band picked up by a hold, drawn larger

  const fx = (f) => Math.log10(f / F_LO) / DECADES;
  const xf = (x) => F_LO * Math.pow(10, x * DECADES);

  const freqs = Array.from({ length: N }, (_, i) => xf(i / (N - 1)));
  $: sysDb = stages ? systemCurveDb(bands, stages, fs, freqs, on) : null;
  // ±24 also when the whole system goes past ±12: the loudness at a low volume does, and the subsonic
  $: range = bands.some((b) => b.on && !hasGain(b.type)) || sysDb?.some((v) => Math.abs(v) > 12) ? 24 : 12;
  $: pw = Math.max(10, width - LEFT - RIGHT);
  $: ph = Math.max(10, height - TOP - BOTTOM);
  $: X = (x) => LEFT + x * pw;
  $: Y = (db) => TOP + (0.5 - Math.max(-range, Math.min(range, db)) / (2 * range)) * ph;

  // reactive, so a new size or range redraws what is drawn with it
  $: line = (dbs) => dbs.map((db, i) => `${i ? 'L' : 'M'}${X(i / (N - 1)).toFixed(1)},${Y(db).toFixed(1)}`).join('');
  $: curve = curveDb(bands, fs, freqs);
  $: path = line(curve);
  $: area = `${path}L${X(1).toFixed(1)},${Y(0).toFixed(1)}L${X(0).toFixed(1)},${Y(0).toFixed(1)}Z`;
  $: sysPath = sysDb ? line(sysDb) : '';
  $: dbLines = [range, range / 2, 0, -range / 2, -range];
  $: handles = bands.map((b, i) => ({
    i,
    x: X(Math.min(1, Math.max(0, fx(b.f)))),
    y: Y(bandMagnitudeDb(b, fs, b.f)),
    on: b.on,
  }));
  // the selected handle on top, as the panel moves it to the foreground
  $: drawn = [...handles.filter((h) => h.i !== sel), ...handles.filter((h) => h.i === sel)];

  // ── one press on a point (lib/gesture.js pointGesture) ──
  let g = null;
  let gi = -1;                    // its band
  let gid = null;                 // its pointer
  let off = { x: 0, y: 0 };       // the finger from the point's centre, kept while it moves
  let holdTimer = null;

  const local = (e) => {
    const r = svg.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };

  function down(e, h) {
    if (e.pointerType === 'mouse' && e.button !== 0) return;
    // a press starts afresh — also when the end of the one before never came
    clearTimeout(holdTimer);
    g = pointGesture(e.pointerType);
    gi = h.i;
    gid = e.pointerId;
    const p = local(e);
    off = { x: p.x - h.x, y: p.y - h.y };
    try { e.currentTarget.setPointerCapture(e.pointerId); } catch (err) { /* gone */ }
    if (h.i !== sel) dispatch('select', h.i);
    if (g.down(e.clientX, e.clientY, e.timeStamp) === 'drag') {
      e.preventDefault();             // a mouse: no text selection while it drags
      begin();
      return;
    }
    holdTimer = setTimeout(() => {
      if (g && g.tick(performance.now()) === 'lift') lifted = gi;
    }, HOLD_MS);
  }

  function begin() {
    drag = gi;
    lifted = gi;
    dispatch('grab', gi);
  }

  function move(e) {
    if (!g || e.pointerId !== gid) return;
    const was = g.state;
    const s = g.move(e.clientX, e.clientY, e.timeStamp);
    if (s === 'lift') lifted = gi;
    if (s !== 'drag') return;
    if (was !== 'drag') begin();
    const p = local(e);
    const nx = Math.min(1, Math.max(0, (p.x - off.x - LEFT) / pw));
    const ny = Math.min(1, Math.max(0, (p.y - off.y - TOP) / ph));
    const b = bands[drag];
    if (!b) return;
    const target = (0.5 - ny) * 2 * range;
    let gain = b.gain;
    if (hasGain(b.type)) gain = Math.max(-12, Math.min(12, b.type === 'peak' ? target : target * 2));
    // a tenth of a dB and three significant figures of Hz: what the steppers show
    const f = Number(xf(nx).toPrecision(3));
    dispatch('edit', { i: drag, band: { ...b, f, gain: Math.round(gain * 10) / 10, on: true } });
  }

  function up(e) {
    if (!g || e.pointerId !== gid) return;
    clearTimeout(holdTimer);
    g.end();
    g = null;
    gid = null;
    lifted = -1;
    try { if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId); } catch (err) { /* gone */ }
    if (drag >= 0) {
      const i = drag;
      drag = -1;
      dispatch('release', i);
    }
  }
  onDestroy(() => clearTimeout(holdTimer));
</script>

<div class="plot" class:dragging={drag >= 0} bind:clientWidth={width} bind:clientHeight={height}>
  <svg bind:this={svg} {width} {height} role="img" aria-label={label}>
    {#each GRID_F as f, k}
      <line class="grid" x1={X(fx(f))} x2={X(fx(f))} y1={TOP} y2={TOP + ph} />
      <text class="lbl" x={X(fx(f))} y={height - 6} text-anchor="middle">{GRID_L[k]}</text>
    {/each}
    {#each dbLines as db}
      <line class:zero={db === 0} class="grid" x1={LEFT} x2={LEFT + pw} y1={Y(db)} y2={Y(db)} />
      <text class="lbl" x={LEFT - 6} y={Y(db) + 4} text-anchor="end">{db > 0 ? '+' + db : db < 0 ? '−' + -db : db}</text>
    {/each}
    <path class="area" class:off={!on} d={area} />
    <path class="curve" class:off={!on} d={path} />
    {#if sysPath}<path class="system" d={sysPath} />{/if}
    {#each drawn as h (h.i)}
      <g class="h" class:sel={h.i === sel} class:on={h.on} transform="translate({h.x},{h.y})">
        <circle r={h.i === lifted ? R + 4 : R} />
        <text y="4.5" text-anchor="middle">{h.i + 1}</text>
      </g>
    {/each}
  </svg>
  <!-- what a finger takes: the points, and nothing else of the plot. In the bands' order, the
       selected one raised: a node moved in the document would lose the pointer it holds. -->
  {#each handles as h (h.i)}
    <span class="hit" class:top={h.i === sel} aria-hidden="true"
          style="left:{h.x - HIT}px;top:{h.y - HIT}px;width:{2 * HIT}px;height:{2 * HIT}px"
          on:pointerdown={(e) => down(e, h)} on:pointermove={move} on:pointerup={up} on:pointercancel={up}></span>
  {/each}
</div>

<style>
  .plot {
    position: relative;
    height: 240px;
    background: var(--surface-low);
    border-radius: var(--r-l);
    overflow: hidden;
    touch-action: pan-y;      /* the plot scrolls with the page; only a point is taken */
  }
  @media (min-width: 840px) {
    .plot { height: 320px; }
  }
  svg { display: block; user-select: none; }
  .hit {
    position: absolute;
    border-radius: 50%;
    touch-action: none;       /* a point that is pressed is the band's, not the page's */
    cursor: grab;
    z-index: 1;
  }
  .hit.top { z-index: 2; }
  .dragging, .dragging .hit { cursor: grabbing; }
  .grid { stroke: var(--outline-var); stroke-width: 1; }
  .grid.zero { stroke: var(--outline); }
  .lbl { fill: var(--outline); font-size: 11px; }
  .curve {
    fill: none;
    stroke: var(--primary);
    stroke-width: 3;
    stroke-linejoin: round;
    stroke-linecap: round;
  }
  .curve.off { stroke: var(--outline); }
  .area { fill: var(--primary); opacity: 0.12; }
  .area.off { fill: var(--outline); opacity: 0.08; }
  .system {
    fill: none;
    stroke: var(--tertiary);
    stroke-width: 2;
    stroke-dasharray: 6 4;
    stroke-linejoin: round;
  }
  .h circle {
    fill: transparent;
    stroke: var(--outline);
    stroke-width: 2;
    transition: r 120ms;
  }
  .h text { fill: var(--outline); font-size: 13px; font-weight: 600; }
  .h.on circle { fill: var(--secondary-container); stroke: none; }
  .h.on text { fill: var(--on-secondary-container); }
  .h.sel circle { fill: var(--primary); stroke: none; }
  .h.sel text { fill: var(--on-primary); }
</style>
