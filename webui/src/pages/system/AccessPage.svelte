<script>
  // Access: who may change the panel from a browser — GET /api/auth
  // (public: on or off, and the user name) and POST /api/auth (behind the login). The rules are the
  // handler's (http_service.cpp, handle_post_auth): a new password needs the current one and at
  // least 4 characters; the name and the switch need only the login itself. The panel keeps a name
  // of up to 31 bytes and a password of up to 63, in UTF-8 — a Cyrillic letter is two — and a ':'
  // in the name would split Basic auth's "user:pass" in the wrong place.
  import { onMount } from 'svelte';
  import { t } from '../../stores/i18n.js';
  import { apiGet, apiPost, setAuth, authPass, clearAuth, utf8Bytes } from '../../lib/api.js';
  import { toastSuccess, toastError, toastWarn } from '../../stores/toast.js';
  import Group from '../../m3/Group.svelte';
  import Row from '../../m3/Row.svelte';
  import Switch from '../../m3/Switch.svelte';
  import Button from '../../m3/Button.svelte';
  import Dialog from '../../m3/Dialog.svelte';
  import TextField from '../../m3/TextField.svelte';

  const MIN = 4;
  const NAME_MAX = 31;     // bytes
  const PASS_MAX = 63;     // bytes

  let loaded = false;
  let enabled = true;
  let username = 'admin';
  let current = '';
  let next = '';
  let repeat = '';
  let busy = false;
  let askOff = false;
  // The switch flips itself as it is tapped; when the login stays as it was (the question was
  // cancelled, the device refused) it is built again from `enabled`, or it would show "off" over
  // a login that is on.
  let switchKey = 0;
  function keepAsItWas() {
    askOff = false;
    switchKey++;
  }

  // Not read: say so with another try — the page showed "login on, admin" as if the device had.
  let loadFailed = false;
  async function load() {
    loadFailed = false;
    try {
      const d = await apiGet('/api/auth');
      enabled = !!d.enabled;
      username = d.username || 'admin';
      loaded = true;
    } catch (e) {
      loadFailed = true;
    }
  }
  onMount(load);

  function reason(e) {
    try {
      const r = JSON.parse(e.message);
      if (r.error === 'wrong_password') return $t['access.wrong'];
    } catch (x) { /* plain text */ }
    return e.message;
  }

  async function setEnabled(on) {
    askOff = false;
    busy = true;
    try {
      await apiPost('/api/auth', { enabled: on });
      enabled = on;
      if (!on) clearAuth();
      toastSuccess(on ? $t['access.on_done'] : $t['access.off_done']);
    } catch (e) {
      toastError(reason(e));
      switchKey++;
    } finally {
      busy = false;
    }
  }

  $: nameOk = username.trim().length > 0 && utf8Bytes(username.trim()) <= NAME_MAX && !username.includes(':');
  async function saveName() {
    busy = true;
    try {
      const name = username.trim();
      await apiPost('/api/auth', { username: name });
      // The panel checks the name as well: this tab signs in with the new one from now on. Its
      // requests with the old one would each count as a wrong login, and five lock the panel's
      // login for every browser.
      const pass = authPass();
      if (pass !== null) setAuth(name, pass);
      toastSuccess($t['access.saved']);
    } catch (e) {
      toastError(reason(e));
    } finally {
      busy = false;
    }
  }

  $: passOk = current.length > 0 && next.length >= MIN && utf8Bytes(next) <= PASS_MAX && next === repeat;
  async function savePass() {
    if (next !== repeat) { toastWarn($t['access.mismatch']); return; }
    busy = true;
    try {
      await apiPost('/api/auth', { username: username.trim(), current_pass: current, new_pass: next });
      // keep this tab signed in with what the panel now expects
      setAuth(username.trim(), next);
      current = next = repeat = '';
      toastSuccess($t['access.pass_done']);
    } catch (e) {
      toastError(reason(e));
    } finally {
      busy = false;
    }
  }

  function signOut() {
    clearAuth();
    location.reload();
  }
</script>

{#if loadFailed}
  <p class="msg">{$t['app.load_failed']}</p>
  <div class="again"><Button variant="tonal" icon="refresh" on:click={load}>{$t['app.retry']}</Button></div>
{:else if loaded}
  <Group>
    <Row icon="lock" title={$t['access.login']} subtitle={enabled ? $t['access.login_on'] : $t['access.login_off']}>
      {#key switchKey}
        <Switch checked={enabled} label={$t['access.login']} disabled={busy}
                on:change={(e) => (e.detail ? setEnabled(true) : (askOff = true))} />
      {/key}
    </Row>
  </Group>

  {#if enabled}
    <Group caption={$t['access.name']}>
      <div class="form">
        <TextField label={$t['access.name']} bind:value={username} autocomplete="username" maxlength={NAME_MAX} />
        <p class="s">{$t['access.name_rule']}</p>
        <div class="end">
          <Button variant="tonal" disabled={busy || !nameOk} on:click={saveName}>{$t['access.save']}</Button>
        </div>
      </div>
    </Group>

    <Group caption={$t['access.pass']}>
      <div class="form">
        <TextField label={$t['access.current']} type="password" bind:value={current} autocomplete="current-password" />
        <TextField label={$t['access.new']} type="password" bind:value={next} autocomplete="new-password" maxlength={PASS_MAX} />
        <TextField label={$t['access.repeat']} type="password" bind:value={repeat} autocomplete="new-password" maxlength={PASS_MAX} />
        <p class="s">{$t['access.rule']}</p>
        <div class="end">
          <Button disabled={busy || !passOk} on:click={savePass}>{$t['access.change']}</Button>
        </div>
      </div>
    </Group>

    <Group>
      <Row button icon="logout" title={$t['access.sign_out']} on:click={signOut} />
    </Group>
  {/if}
{/if}

{#if askOff}
  <Dialog title={$t['access.off_q']} on:close={keepAsItWas}>
    <p class="b">{$t['access.off_body']}</p>
    <svelte:fragment slot="actions">
      <Button variant="text" on:click={keepAsItWas}>{$t['sys.cancel']}</Button>
      <Button danger on:click={() => setEnabled(false)}>{$t['access.off']}</Button>
    </svelte:fragment>
  </Dialog>
{/if}

<style>
  .form { display: flex; flex-direction: column; gap: 12px; padding: 12px 16px; }
  .s { font-size: 14px; line-height: 20px; color: var(--on-surface-var); }
  .end { display: flex; justify-content: flex-end; }
  .b { font-size: 16px; line-height: 24px; color: var(--on-surface-var); }
  .msg { text-align: center; color: var(--on-surface-var); padding: 32px 16px 8px; }
  .again { display: flex; justify-content: center; }
</style>
