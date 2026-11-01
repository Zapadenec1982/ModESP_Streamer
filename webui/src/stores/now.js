import { derived } from 'svelte/store';
import { state } from './state.js';
import { K } from './i18n.js';
import { badge } from '../lib/numbers.js';

/*
 * What is playing, one shape for every source — the same faces, fallbacks and commands as the
 * panel's Player (modules/ui_lvgl/src/screen_now.cpp), so the web and the glass say and do the
 * same thing. Fields:
 *   kind        off | tone | aux | bt | card | radio | dlna | airplay | usb
 *   source      audio.source value of this face (DLNA rides 5, the radio's)
 *   icon        Material Symbols name of the source
 *   nameKey     catalog key of the source's name
 *   title, subtitle         text from the device, or null
 *   titleKey, subtitleKey   catalog key to show when the text is null
 *   format      the codec badge ("44.1 kHz 128 kbps", "LHDC V5 96.0 kHz"), or ''
 *   fromKey     catalog key of the line over the cover — the source's name, as the glass prints it
 *               («Bluetooth»; it said «Грає з Bluetooth» with nothing playing) — or null where the
 *               sound is not played from anywhere (the tone, AUX, off); USB says «USB»
 *   device      the device it plays from, under that line: the phone's name, or ''
 *   playing     true while sound is coming
 *   live        the transport has someone to command (Bluetooth needs a phone; AirPlay needs
 *               the sender's DACP remote, which AirPlay 2 senders do not give)
 *   toggle, prev, next      { key, value } commands, null where the source has none
 *   stopIcon    radio stops rather than pauses
 *   progress    'track' (pos/dur), or null
 *   pos, dur    seconds; dur 0 = unknown
 *   seekKey     where to write a position, or null where seeking does not exist
 *   repeat, shuffle   the card's queue modes (null elsewhere)
 */

const join = (...parts) => parts.filter((p) => p).join(' — ');
const base = (path) => (path ? String(path).split('/').pop().replace(/\.[^.]+$/, '') : '');
const cmd = (key, value) => ({ key, value });
// the panel drops " 2 ch" from the format chip (screen_now.cpp:297-307)
const compact = (f) => (f ? String(f).replace(' 2 ch', '') : '');

function face(s) {
  const src = s['audio.source'];
  const radioState = s['radio.state'] || 'idle';

  if (src === 4) {
    const playing = s['player.state'] === 'playing';
    const count = s['player.count'] || 0;
    return {
      kind: 'card', source: 4, icon: 'sd_card', nameKey: K('src.card'), fromKey: K('src.card'), device: '',
      title: s['player.title'] || base(s['player.track']) || '—',
      subtitle: join(s['player.artist'], s['player.album']) || (count ? `${(s['player.index'] || 0) + 1} / ${count}` : ''),
      format: compact(s['player.format']),
      playing, live: true,
      toggle: cmd('player.cmd', 6), prev: cmd('player.cmd', 5), next: cmd('player.cmd', 4),
      progress: 'track', pos: s['player.pos_s'] || 0, dur: s['player.dur_s'] || 0, seekKey: 'player.seek_s',
      repeat: s['player.repeat'] || 0, shuffle: !!s['player.shuffle'],
    };
  }

  if (src === 5 && s['radio.mode'] === 'push') {
    const playing = s['dlna.state'] === 'PLAYING';
    const own = s['radio.url'] && s['radio.url'] === s['dlna.uri'] ? s['dlna.title'] : '';
    const title = own || s['radio.now_playing'] || null;
    return {
      kind: 'dlna', source: 5, icon: 'cast', nameKey: K('src.dlna'), fromKey: K('src.dlna'), device: '',
      title, titleKey: title ? null : K('now.dlna_track'),
      subtitle: join(s['dlna.artist'], s['dlna.album']),
      format: compact(s['radio.format']),
      playing, live: true,
      toggle: cmd('dlna.cmd', playing ? 3 : 1), prev: cmd('dlna.cmd', 5), next: cmd('dlna.cmd', 4),
      progress: 'track', pos: s['radio.pos_s'] || 0, dur: s['radio.dur_s'] || 0,
      seekKey: s['radio.seekable'] ? 'radio.seek_s' : null,
      repeat: null, shuffle: null,
    };
  }

  if (src === 5) {
    // The station face: who plays — the station — and under it what, the ICY title once it
    // plays, the state in words until then. No progress; ‹ › are prev and next. The play/stop
    // here is the mini-player's: the Player's station card has none (PlayerPage.svelte).
    const sounding = ['playing', 'buffering', 'connecting', 'reconnecting'].includes(radioState);
    const name = s['radio.name'];
    const stateKey = {
      connecting: K('now.connecting'),
      buffering: K('now.buffering'),
      reconnecting: K('now.reconnecting'),
    }[radioState] || (radioState === 'playing' ? null : K('now.stopped'));
    return {
      kind: 'radio', source: 5, icon: 'radio', nameKey: K('src.radio'), fromKey: K('src.radio'), device: '',
      title: name || null, titleKey: name ? null : K('src.radio'),
      subtitle: radioState === 'playing' ? s['radio.now_playing'] || null : null, subtitleKey: stateKey,
      format: compact(s['radio.format']),
      playing: sounding, live: true, stopIcon: true,
      toggle: cmd('radio.cmd', radioState === 'idle' ? 1 : 2), prev: cmd('radio.cmd', 4), next: cmd('radio.cmd', 3),
      progress: null, pos: 0, dur: 0, seekKey: null,
      repeat: null, shuffle: null,
    };
  }

  if (src === 3) {
    const phone = !!s['bt.peer'];
    const rate = s['bt.rate_hz'] ? ` ${(s['bt.rate_hz'] / 1000).toFixed(1)} kHz` : '';
    return {
      kind: 'bt', source: 3, icon: 'bluetooth', nameKey: K('src.bt'), fromKey: K('src.bt'), device: s['bt.peer'] || '',
      // no phone: «Телефон не підключено», once — not «Немає пристрою» over «телефон не підключено»
      title: s['bt.title'] || (phone ? 'Bluetooth' : null), titleKey: s['bt.title'] || phone ? null : K('now.no_device'),
      subtitle: join(s['bt.artist'], s['bt.album']) || null, subtitleKey: null,
      format: s['bt.codec'] ? s['bt.codec'] + rate : '',
      playing: s['bt.play_state'] === 'playing', live: phone,
      toggle: cmd('bt.cmd', 6), prev: cmd('bt.cmd', 5), next: cmd('bt.cmd', 4),
      progress: 'track', pos: s['bt.pos_s'] || 0, dur: s['bt.dur_s'] || 0, seekKey: null,
      repeat: null, shuffle: null,
    };
  }

  if (src === 6) {
    const active = ['connected', 'playing', 'paused'].includes(s['airplay.state']);
    return {
      kind: 'airplay', source: 6, icon: 'airplay', nameKey: K('src.airplay'), fromKey: K('src.airplay'), device: '',
      title: s['airplay.title'] || (active ? 'AirPlay' : null), titleKey: s['airplay.title'] || active ? null : K('now.airplay_waiting'),
      subtitle: join(s['airplay.artist'], s['airplay.album']) || null, subtitleKey: active ? null : K('now.airplay_pick'),
      format: 'AirPlay',
      playing: s['airplay.state'] === 'playing', live: !!s['airplay.remote'],
      toggle: cmd('airplay.cmd', 6), prev: cmd('airplay.cmd', 5), next: cmd('airplay.cmd', 4),
      progress: 'track', pos: s['airplay.pos_s'] || 0, dur: s['airplay.dur_s'] || 0, seekKey: null,
      repeat: null, shuffle: null,
    };
  }

  const still = { fromKey: null, device: '', format: '', playing: false, live: false, toggle: null, prev: null, next: null,
                  progress: null, pos: 0, dur: 0, seekKey: null, repeat: null, shuffle: null };
  if (src === 1) {
    return { ...still, kind: 'tone', source: 1, icon: 'graphic_eq', nameKey: K('src.tone'), title: null, titleKey: K('now.tone'),
             subtitle: s['audio.tone_hz'] ? `${s['audio.tone_hz']} Hz` : '', playing: true };
  }
  if (src === 2) {
    return { ...still, kind: 'aux', source: 2, icon: 'input', nameKey: K('src.aux'), title: null, titleKey: K('now.aux'),
             subtitle: '', playing: true };
  }
  if (src === 7) {
    // A computer, over USB: no track and no transport — the computer is its own remote. The rate
    // and width are the line under the title, as the glass draws them (screen_now.cpp).
    const st = s['usb_audio.state'];
    const streaming = st === 'playing';
    const rate = s['usb_audio.rate'];
    const fmt = streaming && rate ? `${(rate / 1000).toFixed(1)} kHz ${s['usb_audio.bits'] || ''} bit` : '';
    return { ...still, kind: 'usb', source: 7, icon: 'usb', nameKey: K('src.usb'), fromKey: K('src.usb'),
             title: null, titleKey: streaming || st === 'ready' ? K('now.usb') : K('now.usb_waiting'),
             subtitle: fmt || null, subtitleKey: fmt ? null : K('now.usb_pick'), playing: streaming };
  }
  return { ...still, kind: 'off', source: 0, icon: 'power_settings_new', nameKey: K('src.off'), title: null, titleKey: K('now.off'),
           subtitle: null, subtitleKey: K('now.pick') };
}

export const now = derived(state, ($s) => face($s));

// the units badge() says, named for the catalog's check (tools/tests/test_webui_i18n.py)
K('unit.khz'); K('unit.hz'); K('unit.kbps'); K('unit.bit'); K('unit.ch');

/**
 * The line under the title as a page shows it: the device's text — in the language's units where it
 * is a technical line, USB's rate and the tone's frequency (numbers.js badge) — or the catalog's.
 */
export function subLine(np, t, lang) {
  if (np.subtitle == null) return np.subtitleKey ? t[np.subtitleKey] : '';
  return np.kind === 'usb' || np.kind === 'tone' ? badge(np.subtitle, t, lang) : np.subtitle;
}
