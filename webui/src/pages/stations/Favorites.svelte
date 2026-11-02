<script>
  // The station list and its presets — what the panel's
  // «Улюблені» shows, plus what is awkward on the glass: renaming, ordering, deleting, adding a
  // stream by its address, a playlist file in and out. The device keeps the list; every edit
  // goes back whole, with the generation it was made from (lib/stations.js), and the page
  // reloads when radio.list_rev says the list moved — by its own hand or the panel's.
  import { onMount, onDestroy } from 'svelte';
  import { state } from '../../stores/state.js';
  import { t, fmt, plural, language, K } from '../../stores/i18n.js';
  import { writeNow } from '../../lib/settings.js';
  import { toastError, toastSuccess, toastWarn } from '../../stores/toast.js';
  import * as S from '../../lib/stations.js';
  import Group from '../../m3/Group.svelte';
  import Row from '../../m3/Row.svelte';
  import Icon from '../../m3/Icon.svelte';
  import IconButton from '../../m3/IconButton.svelte';
  import Button from '../../m3/Button.svelte';
  import Chip from '../../m3/Chip.svelte';
  import Dialog from '../../m3/Dialog.svelte';
  import TextField from '../../m3/TextField.svelte';

  let list = null;          // { rev, stations, presets }
  let failed = false;
  // An edit is on its way: from the POST until the device has moved radio.list_rev past the
  // generation it was made from. The device answers 202 and takes the edit within a tick; the new
  // generation reaches us only through the state, so an edit sent before it — a second «Вище» —
  // would carry the old one and be refused as "changed" (or "busy"). The controls wait instead.
  let saving = false;
  let waitRev = null;       // the generation an accepted edit was made from
  let waitTimer = null;
  let sheet = -1;           // the station whose actions are open
  let renameTo = '';
  let adding = false;
  let addName = '';
  let addUrl = '';
  let fileInput;

  async function reload() {
    try {
      list = await S.loadStations();
      failed = false;
    } catch (e) {
      failed = true;
    }
  }
  onMount(reload);

  // radio.list_rev is the store's generation: when it is not the one we hold, the list moved.
  // Read once for each generation the state names: reload() replaces `list`, which runs this
  // again, and with the state's generation stuck (no WebSocket) it would read the list for ever.
  $: rev = $state['radio.list_rev'];
  let readFor;
  $: if (list && rev !== undefined && rev !== list.rev && rev !== readFor && !saving) {
    readFor = rev;
    reload();
  }
  $: if (waitRev !== null && rev !== undefined && rev !== waitRev) settle();
  onDestroy(() => clearTimeout(waitTimer));

  $: s = $state;
  $: radioOn = s['audio.source'] === 5 && s['radio.mode'] !== 'push';
  $: current = radioOn ? s['radio.station'] : -2;
  $: sounding = radioOn && ['playing', 'buffering', 'connecting', 'reconnecting'].includes(s['radio.state']);
  // a preset can be stored from what plays only if it is a list station
  $: storable = radioOn && current >= 0;

  /** True when the device took the edit; nothing happens while another is on its way. */
  async function commit(next, okKey) {
    if (!list || saving) return false;
    saving = true;
    const before = list;
    list = { ...list, ...next };                         // shown at once; the device confirms
    let ok = false;
    try {
      const r = await S.saveStations(before.rev, list.stations, list.presets);
      if (r === 'changed') {
        toastWarn($t['st.conflict']);
        await reload();
      } else {
        ok = true;
        if (okKey) toastSuccess($t[okKey]);
      }
    } catch (e) {
      toastError(`${$t['st.save_failed']}: ${e.message}`);
      await reload();
    }
    if (!ok) {
      saving = false;
      return false;
    }
    // the device moves radio.list_rev within a tick; the reload then brings the new generation
    waitRev = before.rev;
    clearTimeout(waitTimer);
    waitTimer = setTimeout(settle, 3000);
    return true;
  }

  /** The device moved past the edit (or did not say so in 3 s): its list, and edits again. */
  async function settle() {
    if (waitRev === null) return;
    waitRev = null;
    clearTimeout(waitTimer);
    await reload();
    saving = false;
  }

  async function play(i) {
    const body = i === s['radio.station'] && radioOn
      ? { 'audio.source': 5, 'radio.cmd': 1 }
      : { 'audio.source': 5, 'radio.station': i };
    try {
      await writeNow(body);           // shown at once, taken back if the device refuses it
    } catch (e) {
      toastError(e.message);
    }
  }

  function presetTap(slot) {
    const i = list.presets[slot];
    if (i >= 0) play(i);
    else if (storable) commit(S.setPreset(list, current, slot), K('st.saved'));
  }

  function presetOf(i) {
    return list.presets.indexOf(i);
  }

  // Reactive, not a plain function: the template calls subtitle(st, i), and Svelte re-runs that
  // call only when something in the call itself changes. $t is read inside, so a plain function
  // kept the bundled Ukrainian text after the English catalog arrived («кнопка 1» on the English
  // Stations page).
  $: subtitle = (st, i) => {
    const p = presetOf(i);
    return p >= 0 ? `${S.host(st.url)} · ${fmt($t['st.preset_n'], { n: p + 1 })}` : S.host(st.url);
  };

  function openSheet(i) {
    sheet = i;
    renameTo = list.stations[i].name;
  }

  function addFromDialog() {
    const url = addUrl.trim();
    const why = S.refuse(list, addName, url);
    if (why) {
      const dup = list.stations.find((x) => x.url === url);
      toastWarn(fmt($t[why], { name: dup ? dup.name : '' }));
      return;
    }
    adding = false;
    commit(S.add(list, addName, url), K('st.saved'));
    addName = addUrl = '';
  }

  async function importFile(e) {
    const file = e.target.files && e.target.files[0];
    e.target.value = '';
    if (!file || !list) return;
    let next = { stations: list.stations, presets: list.presets };
    let n = 0;
    for (const it of S.parsePlaylist(await file.text())) {
      if (S.refuse(next, it.name, it.url)) continue;
      next = S.add(next, it.name, it.url);
      n++;
    }
    if (!n) toastWarn($t['st.import_none']);
    else if (await commit(next)) toastSuccess(fmt($t['st.imported'], { n }));
  }

  function exportFile() {
    const blob = new Blob([S.toM3u(list.stations)], { type: 'audio/x-mpegurl' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'p4-player-stations.m3u';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }
</script>

{#if failed && !list}
  <p class="msg">{$t['st.load_failed']}</p>
  <div class="center"><Button variant="tonal" icon="refresh" on:click={reload}>{$t['app.retry']}</Button></div>
{:else if !list}
  <p class="msg">{$t['app.loading']}</p>
{:else}
  <h2 class="cap">{$t['st.presets']}</h2>
  <div class="presets">
    {#each list.presets as p, slot}
      {@const st = p >= 0 ? list.stations[p] : null}
      {@const on = st && p === current}
      <button class="preset" class:on type="button" on:click={() => presetTap(slot)}
              aria-label={`${slot + 1}: ${st ? st.name : storable ? $t['st.preset_store'] : $t['st.preset_empty']}`}>
        <span class="num">{slot + 1}</span>
        <span class="pname" class:store={!st && storable}>{st ? st.name : storable ? $t['st.preset_store'] : $t['st.preset_empty']}</span>
      </button>
    {/each}
  </div>

  <div class="bar">
    <h2 class="cap grow">{fmt($t['st.all'], { n: list.stations.length })}</h2>
    <IconButton icon="add_link" label={$t['st.add']} on:click={() => (adding = true)} />
    <IconButton icon="upload_file" label={$t['st.import']} on:click={() => fileInput.click()} />
    <IconButton icon="download" label={$t['st.export']} on:click={exportFile} />
    <input class="file" type="file" accept=".m3u,.m3u8,.pls,audio/x-mpegurl,audio/x-scpls" bind:this={fileInput} on:change={importFile} />
  </div>

  {#if list.stations.length}
    <Group>
      {#each list.stations as st, i (st.url)}
        <div class="item">
          <Row list button icon="radio" title={st.name} subtitle={subtitle(st, i)} selected={i === current} on:click={() => play(i)}>
            {#if i === current && sounding}<span class="live"><Icon name="graphic_eq" label={$t['transport.play']} /></span>{/if}
          </Row>
          <IconButton icon="more_vert" label={fmt($t['st.actions'], { name: st.name })} on:click={() => openSheet(i)} />
        </div>
      {/each}
    </Group>
  {:else}
    <p class="msg">{$t['srcsub.radio_none']}</p>
  {/if}
{/if}

{#if sheet >= 0 && list && list.stations[sheet]}
  {@const i = sheet}
  {@const st = list.stations[i]}
  <Dialog title={st.name} on:close={() => (sheet = -1)}>
    <div class="sheet">
      <p class="url">{st.url}</p>
      <Button variant="tonal" icon="play_arrow" on:click={() => { play(i); sheet = -1; }}>{$t['st.play']}</Button>

      <div class="rename">
        <TextField label={$t['st.name']} bind:value={renameTo} maxlength={S.NAME_MAX} />
        <Button variant="text" disabled={saving || !renameTo.trim() || renameTo === st.name}
                on:click={() => { commit(S.rename(list, i, renameTo), K('st.saved')); sheet = -1; }}>{$t['btn.save']}</Button>
      </div>

      <span class="lbl">{$t['st.to_preset']}</span>
      <div class="chips">
        {#each Array(S.PRESETS) as _, slot}
          <Chip button selected={list.presets[slot] === i} disabled={saving} on:click={() => commit(S.setPreset(list, i, slot), K('st.saved'))}>{slot + 1}</Chip>
        {/each}
        {#if presetOf(i) >= 0}
          <Chip button disabled={saving} on:click={() => commit(S.setPreset(list, i, -1), K('st.saved'))}>{$t['st.unpreset']}</Chip>
        {/if}
      </div>

      <div class="moves">
        <!-- the dialog follows the station only when the device took the move -->
        <Button variant="outlined" icon="swap_vert" disabled={saving || i === 0}
                on:click={async () => { if (await commit(S.move(list, i, i - 1))) sheet = i - 1; }}>{$t['st.up']}</Button>
        <Button variant="outlined" icon="swap_vert" disabled={saving || i === list.stations.length - 1}
                on:click={async () => { if (await commit(S.move(list, i, i + 1))) sheet = i + 1; }}>{$t['st.down']}</Button>
      </div>

      {#if i === current && sounding}
        <p class="hint">{$t['st.delete_playing']}</p>
      {/if}
    </div>
    <svelte:fragment slot="actions">
      <Button variant="text" danger icon="delete" disabled={saving || (i === current && sounding) || list.stations.length <= 1}
              on:click={() => { commit(S.remove(list, i), K('st.saved')); sheet = -1; }}>{$t['st.delete']}</Button>
      <Button variant="text" on:click={() => (sheet = -1)}>{$t['common.close']}</Button>
    </svelte:fragment>
  </Dialog>
{/if}

{#if adding}
  <Dialog title={$t['st.add_title']} on:close={() => (adding = false)}>
    <form class="sheet" on:submit|preventDefault={addFromDialog}>
      <TextField label={$t['st.url']} bind:value={addUrl} placeholder="https://" />
      <TextField label={$t['st.name']} bind:value={addName} maxlength={S.NAME_MAX} />
    </form>
    <svelte:fragment slot="actions">
      <Button variant="text" on:click={() => (adding = false)}>{$t['common.cancel']}</Button>
      <Button disabled={!addUrl.trim()} on:click={addFromDialog}>{$t['st.add']}</Button>
    </svelte:fragment>
  </Dialog>
{/if}

<style>
  .cap {
    font-size: 14px;
    line-height: 20px;
    font-weight: 500;
    color: var(--on-surface-var);
    padding: 16px 16px 8px;
  }
  .grow { flex: 1; padding-right: 0; }
  .bar { display: flex; align-items: center; }
  .presets {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
    gap: 8px;
  }
  .preset {
    height: 52px;
    border: 0;
    border-radius: var(--r-m);
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 0 12px 0 10px;
    text-align: left;
    background: var(--surface-container);
    color: var(--on-surface);
    min-width: 0;
  }
  .preset.on { background: var(--secondary-container); color: var(--on-secondary-container); }
  .num {
    width: 32px;
    height: 32px;
    border-radius: 16px;
    flex: none;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 15px;
    font-weight: 500;
    background: var(--surface-highest);
    color: var(--on-surface-var);
  }
  .on .num { background: var(--primary); color: var(--on-primary); }
  .pname { font-size: 14px; font-weight: 500; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; }
  .pname.store { color: var(--primary); }
  .live { color: var(--primary); display: flex; padding-right: 4px; }
  .item { display: flex; align-items: center; gap: 2px; }
  .item :global(.row) { flex: 1; min-width: 0; }
  .file { display: none; }
  .msg { text-align: center; color: var(--on-surface-var); padding: 32px 16px 16px; }
  .center { display: flex; justify-content: center; }
  .sheet { display: flex; flex-direction: column; gap: 14px; }
  .url { font-size: 13px; color: var(--outline); word-break: break-all; }
  .rename { display: flex; align-items: flex-end; gap: 8px; }
  .rename :global(.tf) { flex: 1; }
  .lbl { font-size: 14px; color: var(--on-surface-var); }
  .chips { display: flex; flex-wrap: wrap; gap: 8px; }
  .moves { display: flex; gap: 8px; flex-wrap: wrap; }
  .hint { font-size: 14px; line-height: 20px; color: var(--on-surface-var); }
</style>
