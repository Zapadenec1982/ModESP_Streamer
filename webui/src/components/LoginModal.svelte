<script>
  // Sign-in, when the panel answers 401. The credentials are checked with GET /api/state, kept for
  // this tab only (sessionStorage, lib/api.js), and the page reloads so every first request is
  // repeated with them. Five wrong logins in a row lock the panel's login for 30 s, for every
  // browser (429, docs/NETWORK.md "Authentication"): that is said as such, not as a wrong
  // password, and what was typed is not kept — the panel did not check it.
  import { t } from '../stores/i18n.js';
  import { needsLogin, setAuth, clearAuth, apiGet } from '../lib/api.js';
  import Dialog from '../m3/Dialog.svelte';
  import TextField from '../m3/TextField.svelte';
  import Button from '../m3/Button.svelte';

  let user = 'admin';
  let pass = '';
  let error = false;
  let locked = false;
  let loading = false;

  async function login() {
    if (!user || !pass || loading) return;
    error = false;
    loading = true;
    setAuth(user, pass);
    try {
      await apiGet('/api/state');
      needsLogin.set(false);
      location.reload();
    } catch (e) {
      error = true;
      locked = e.status === 429;
      if (e.message === 'Unauthorized' || locked) clearAuth();
    } finally {
      loading = false;
    }
  }

  function onKeydown(e) {
    if (e.key === 'Enter') login();
  }
</script>

<Dialog title={$t['auth.title']} persistent>
  <form class="form" on:submit|preventDefault={login}>
    <TextField label={$t['auth.user']} bind:value={user} autocomplete="username" on:keydown={onKeydown} />
    <TextField label={$t['auth.pass']} type="password" bind:value={pass} autocomplete="current-password" on:keydown={onKeydown} />
    {#if error}<p class="err" role="alert">{locked ? $t['auth.locked'] : $t['auth.error']}</p>{/if}
  </form>
  <svelte:fragment slot="actions">
    <Button variant="filled" disabled={loading || !user || !pass} on:click={login}>{$t['auth.login']}</Button>
  </svelte:fragment>
</Dialog>

<style>
  .form { display: flex; flex-direction: column; gap: 14px; }
  .err { color: var(--error); font-size: 14px; line-height: 20px; }
</style>
