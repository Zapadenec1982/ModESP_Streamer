/*
 * The long tail: whatever the generated pages (data/ui.json) carry that no
 * hand-written page shows, rendered in Service as the generated cards. A module that gains a key
 * with a widget shows up there by itself, and tools/generate_ui.py stays as it is (rule 6).
 *
 * Each generated widget is accounted for below: shown by a hand-written page through its key, or
 * through the endpoint the page uses instead of a key, or left out on purpose. What is in none of
 * the three is the tail. tools/tests/test_webui_service.py holds every entry to a page that
 * really uses it, and every generated key a hand-written page uses to an entry here.
 *
 * No imports: the test loads this file in Node.
 */

/** Shown by a hand-written page, which reads the key. */
export const SHOWN = [
  // Player, Source
  'audio.source', 'audio.volume', 'audio.mute',
  'radio.state', 'radio.name', 'radio.now_playing', 'radio.format', 'radio.fav_add',
  'airplay.active_mode_label',
  // DSP, and the sound (Player's sheet, Налаштування › Звук)
  'audio.eq_enabled', 'audio.loudness', 'audio.loud_db', 'audio.bass_db', 'audio.treble_db',
  'audio.vol_max', 'audio.vol_start',
  // Налаштування › Колонки
  'audio.balance_db10',
  // System
  'ui.backlight', 'ui.standby_bl', 'radio.covers',
  'airplay.enabled', 'airplay.name', 'airplay.mode', 'airplay.mode_status', 'airplay.mode_error',
  'wifi.ssid', 'wifi.ip',
  // Service
  'audio.cpu_pct', 'audio.underruns', 'audio.lim_gr', 'audio.bass_gr', 'audio.dsp_pct',
  'audio.dsp_max_pct', 'audio.eq_headroom_db', 'audio.out_clamps', 'audio.dsp_faults',
  'audio.bt_clock_follow',
  'system.uptime', 'system.boot_reason', 'system.main_stack_min', 'system.stalled',
  'system.safe_mode', 'system.main_loop_max_ms', 'system.heap_free', 'system.heap_min',
  // Service › Calibration: the speakers' port
  'audio.port_hz', 'audio.port_meas', 'audio.port_meas_ch',
  'audio.port_meas_status', 'audio.port_meas_hz',
  'audio.tone_mode', 'audio.tone_hz', 'audio.tone_db10', 'audio.tone_ch',
  // Налаштування › Тонке налаштування
  'audio.hp_hz', 'audio.bass_xo_hz', 'audio.port_lift_db',
];

/** Shown by a hand-written page through its own endpoint: endpoint → the generated widgets it replaces. */
export const VIA = {
  '/api/wifi': ['_action.wifi_scan', 'wifi.password', '_action.wifi_save'],
  '/api/wifi/ap': ['wifi.ap_ssid', 'wifi.ap_password', 'wifi.ap_channel', '_action.ap_save'],
  '/api/time': ['time.ntp_enabled', 'time.timezone', 'time.manual_datetime', '_action.time_save'],
  '/api/auth': ['_action.auth_save'],
  '/api/ota': ['_ota.version', '_ota.board', '_ota.date', '_ota.upload'],
  // backup and restore (System › Backup), restart (System), factory reset (Service)
  '/api/factory-reset': ['_action.grid'],
};

/** Left out on purpose. */
export const DROPPED = [
  // The cards "Відтворення", "DSP" and "Діагностика" of the generated Audio page carry nothing a
  // person acts on (customer, 12.10.2026): the source and the volume are on the Player, the EQ
  // on DSP, the load and the underruns in Service's figures.
  'audio.state', 'audio.eq_reload',
  'audio.in_l', 'audio.in_r', 'audio.level_l', 'audio.level_r', 'audio.in_rate', 'audio.asrc_ppm',
  // The generated catalog card's query language ("top:UA", "play row N"): Stations › catalog
  // does all of it over radio.web_*.
  'radio.cat_query', 'radio.cat_sort', 'radio.cat_quality', 'radio.cat_prev', 'radio.cat_next',
  'radio.cat_state', 'radio.cat_page', 'radio.cat_count', 'radio.cat_list', 'radio.cat_error',
  'radio.cat_play', 'radio.cat_fav',
  // Never written by the firmware (wifi_service.cpp); the Wi-Fi page shows the scan's signal.
  'wifi.rssi',
  // The amplifier's ceiling, the drive into it and the bass power cap (customer, 18.10.2026):
  // «Максимум» is the one limit a person sets, and these stay what the datasheet gives.
  'audio.amp_clip_db10', 'audio.amp_cal', 'audio.ceiling_dbfs', 'audio.drive_db', 'audio.rms_cap_db10',
];

/**
 * Generated pages left out whole: the Dashboard repeats other pages' first cards, and the
 * bindings page (its keys are the board's hardware, _binding.<id>) is Service › Hardware, read
 * from /api/bindings.
 */
export const SKIPPED_PAGES = ['dashboard', 'bindings'];

/**
 * The generated pages cut down to the tail: each page's cards with only the widgets nobody else
 * shows, empty cards and pages dropped. A card that lost widgets loses its subtitle and summary
 * too: they spoke of the whole card (the catalog's said how to type a query it no longer has).
 */
export function longTail(pages) {
  const known = new Set([...SHOWN, ...Object.values(VIA).flat(), ...DROPPED]);
  const cut = (c) => {
    const widgets = (c.widgets || []).filter((w) => !known.has(w.key));
    return widgets.length === (c.widgets || []).length ? c : { ...c, widgets, subtitle: '', summary_keys: [] };
  };
  return (pages || [])
    .filter((p) => !SKIPPED_PAGES.includes(p.id))
    .map((p) => ({ ...p, cards: (p.cards || []).map(cut).filter((c) => c.widgets.length) }))
    .filter((p) => p.cards.length);
}
