"""Exercise the shipped WebUI API module with fetch and storage doubles."""

import base64
from pathlib import Path
import shutil
import subprocess


def test_get_post_auth_and_expired_credentials():
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI API regression"
    api = (Path(__file__).resolve().parents[2] / "webui/src/lib/api.js").read_text(encoding="utf-8")
    api = api.replace(
        "import { writable } from 'svelte/store';",
        "const writable = value => ({ value, set(next) { this.value = next; } });",
    )
    script = (
        api
        + """
const storage = new Map();
globalThis.sessionStorage = {
  getItem: key => storage.get(key),
  setItem: (key, value) => storage.set(key, value),
  removeItem: key => storage.delete(key),
};
let expired = false;
const calls = [];
globalThis.fetch = async (url, options = {}) => {
  calls.push({ url, options });
  return expired
    ? { ok: false, status: 401, text: async () => 'Unauthorized' }
    : { ok: true, status: 200, json: async () => ({ ok: true }) };
};
function check(value, message) { if (!value) throw Error(message); }
setAuth('user', 'pass');
await apiGet('/api/state');
await apiPost('/api/settings', {});
check(calls[0].options.headers.Authorization === 'Basic dXNlcjpwYXNz', 'GET omitted credentials');
check(calls[1].options.headers.Authorization === calls[0].options.headers.Authorization,
      'GET and POST authorization differ');
expired = true;
try { await apiGet('/api/bindings'); throw Error('GET accepted expired credentials'); }
catch (e) { check(e.message === 'Unauthorized', `GET: ${e.message}`); }
check(needsLogin.value === true, 'GET 401 did not request login');
needsLogin.set(false);
try { await apiPost('/api/settings', {}); throw Error('POST accepted expired credentials'); }
catch (e) { check(e.message === 'Unauthorized', `POST: ${e.message}`); }
check(needsLogin.value === true, 'POST 401 did not request login');
console.log('WebUI GET/POST auth and expired credentials passed');
"""
    )
    result = subprocess.run([node, "--input-type=module", "-e", script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_a_lockout_is_told_from_a_wrong_password():
    """Five wrong logins lock the panel's login for 30 s and every request gets 429 (NETWORK.md,
    "Authentication"). The sign-in read any failure as "wrong username or password"; the Error now
    carries the status, so it can say the panel is locked instead."""
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI API regression"
    api = (Path(__file__).resolve().parents[2] / "webui/src/lib/api.js").read_text(encoding="utf-8")
    api = api.replace(
        "import { writable } from 'svelte/store';",
        "const writable = value => ({ value, set(next) { this.value = next; } });",
    )
    script = (
        api
        + """
globalThis.sessionStorage = { getItem: () => undefined, setItem: () => {}, removeItem: () => {} };
globalThis.fetch = async () => ({ ok: false, status: 429,
                                  text: async () => 'Too many failed attempts. Try again later.' });
function check(value, message) { if (!value) throw Error(message); }
let err = null;
try { await apiGet('/api/state'); } catch (e) { err = e; }
check(err, 'a 429 was taken');
check(err.status === 429, `status: ${err.status}`);
check(err.message === 'Too many failed attempts. Try again later.', `the panel's text: ${err.message}`);
check(needsLogin.value === false, 'a lockout is not a request to sign in');
console.log('WebUI lockout passed');
"""
    )
    result = subprocess.run([node, "--input-type=module", "-e", script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_credentials_are_base64_of_utf8():
    """A Cyrillic password: the panel stores it as UTF-8, so Basic auth must carry those bytes.
    btoa() on the UTF-16 string threw, and the browser could never sign in with it again."""
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI API regression"
    api = (Path(__file__).resolve().parents[2] / "webui/src/lib/api.js").read_text(encoding="utf-8")
    api = api.replace(
        "import { writable } from 'svelte/store';",
        "const writable = value => ({ value, set(next) { this.value = next; } });",
    )
    want = base64.b64encode("адмін:пароль-ї".encode("utf-8")).decode()
    ascii_want = base64.b64encode(b"user:pass").decode()
    script = (
        api
        + """
const storage = new Map();
globalThis.sessionStorage = {
  getItem: key => storage.get(key),
  setItem: (key, value) => storage.set(key, value),
  removeItem: key => storage.delete(key),
};
function check(value, message) { if (!value) throw Error(message); }
setAuth('адмін', 'пароль-ї');
check(storage.get('modesp_auth') === '%s', `Cyrillic: ${storage.get('modesp_auth')}`);
check(authPass() === 'пароль-ї', `read back: ${authPass()}`);
setAuth('user', 'pass');
check(storage.get('modesp_auth') === '%s', 'ASCII changed');
check(authPass() === 'pass', 'ASCII read back');
setAuth('a', 'x:y');
check(authPass() === 'x:y', 'a colon in the password');
clearAuth();
check(authPass() === null, 'no credentials');
check(utf8Bytes('пароль') === 12 && utf8Bytes('pass') === 4, 'byte count');
console.log('UTF-8 credentials passed');
"""
        % (want, ascii_want)
    )
    result = subprocess.run([node, "--input-type=module", "-e", script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
