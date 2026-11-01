import { writable } from 'svelte/store';

const BASE = '';
const AUTH_KEY = 'modesp_auth';

/** Store: true when a 401 response needs login */
export const needsLogin = writable(false);

/** The bytes a string takes on the panel, in UTF-8: its limits (a 31-byte name, a 63-byte
 *  password) count these, not characters. */
export function utf8Bytes(s) {
  return new TextEncoder().encode(s).length;
}

/** Зберегти credentials (base64 "user:pass") — of the UTF-8 bytes, as the panel stores them.
 *  btoa() alone takes Latin-1 and throws on a Cyrillic password: the panel saved it, and the
 *  browser could never send it back. For ASCII the result is the same. */
export function setAuth(user, pass) {
  let bin = '';
  for (const b of new TextEncoder().encode(user + ':' + pass)) bin += String.fromCharCode(b);
  sessionStorage.setItem(AUTH_KEY, btoa(bin));
}

/** The password this tab signs in with, or null when it has none. */
export function authPass() {
  try {
    const creds = sessionStorage.getItem(AUTH_KEY);
    if (!creds) return null;
    const s = new TextDecoder().decode(Uint8Array.from(atob(creds), (c) => c.charCodeAt(0)));
    return s.substring(s.indexOf(':') + 1);
  } catch (e) {
    return null;
  }
}

/** Очистити credentials */
export function clearAuth() {
  sessionStorage.removeItem(AUTH_KEY);
}

/** Auth header для protected requests */
function authHeaders() {
  const creds = sessionStorage.getItem(AUTH_KEY);
  return creds ? { 'Authorization': `Basic ${creds}` } : {};
}

/** A refused GET's Error carries the HTTP status: the login tells the panel's lockout (429) from
 *  a wrong password by it. */
export async function apiGet(url) {
  const r = await fetch(BASE + url, { headers: authHeaders() });
  if (r.status === 401) {
    needsLogin.set(true);
    throw new Error('Unauthorized');
  }
  if (!r.ok) {
    const text = await r.text().catch(() => '');
    throw Object.assign(new Error(text || `GET ${url}: ${r.status}`), { status: r.status });
  }
  return r.json();
}

/**
 * A GET whose answer is not JSON (the panel's screenshot, the cover): the body as a Blob.
 * `ask: false` is for a picture nobody asked for — a 401 then leaves the login dialog shut.
 */
export async function apiBlob(url, { ask = true } = {}) {
  const r = await fetch(BASE + url, { headers: authHeaders() });
  if (r.status === 401) {
    if (ask) needsLogin.set(true);
    throw new Error('Unauthorized');
  }
  if (!r.ok) {
    const text = await r.text().catch(() => '');
    throw new Error(text || `GET ${url}: ${r.status}`);
  }
  return r.blob();
}

export async function apiPost(url, data) {
  const r = await fetch(BASE + url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...authHeaders() },
    body: JSON.stringify(data)
  });
  if (r.status === 401) {
    needsLogin.set(true);
    throw new Error('Unauthorized');
  }
  if (!r.ok) {
    const text = await r.text().catch(() => '');
    throw new Error(text || httpStatusMessage(r.status, url));
  }
  return r.json();
}

/**
 * A POST that outlives the page — sent as the tab closes or the user leaves (fetch keepalive).
 * Nothing comes back to act on, so nothing is awaited.
 */
export function apiPostKeepalive(url, data) {
  try {
    fetch(BASE + url, {
      method: 'POST',
      keepalive: true,
      headers: { 'Content-Type': 'application/json', ...authHeaders() },
      body: JSON.stringify(data)
    }).catch(() => {});
  } catch (e) {
    /* the page is going away; there is nobody to tell */
  }
}

export async function apiUpload(url, file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', BASE + url);
    const creds = sessionStorage.getItem(AUTH_KEY);
    if (creds) xhr.setRequestHeader('Authorization', `Basic ${creds}`);
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) {
        onProgress(Math.round(e.loaded / e.total * 100), e.loaded);
      }
    };
    xhr.onload = () => {
      if (xhr.status === 200) resolve(JSON.parse(xhr.responseText || '{}'));
      else if (xhr.status === 401) {
        needsLogin.set(true);
        reject(new Error('Unauthorized'));
      }
      else reject(new Error(xhr.responseText || httpStatusMessage(xhr.status, url)));
    };
    xhr.onerror = () => reject(new Error('Connection error'));
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.send(file);
  });
}

function httpStatusMessage(status, url) {
  const map = {
    400: 'Bad request',
    401: 'Unauthorized',
    404: 'Not found',
    413: 'Too large',
    422: 'Invalid value',
    500: 'Server error',
  };
  return map[status] || `${url}: ${status}`;
}
