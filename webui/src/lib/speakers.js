/*
 * Колонки: what the page says the speakers are, what the
 * wizard and the calibration page write, what the port meter's message means, and «Звичайна
 * гучність»'s reference. The device keeps the rules (modules/audio): closed speakers have no port
 * whatever number is kept, the meter only offers what it found, and the reference is a % of the
 * effective level. This file says them the way the pages need them.
 *
 * No imports: tools/tests/test_webui_speakers.py loads this file in Node.
 */

/** audio.spk_type and audio.port_src, the manifest's options. */
export const SPK_UNSET = 0, SPK_PORTED = 1, SPK_CLOSED = 2;
export const SRC_UNSET = 0, SRC_MANUAL = 1, SRC_MEASURED = 3;

/** The port the DSP has, and where it came from: 0 for closed speakers, as read_port() rules. */
export function port(s) {
  const hz = s['audio.spk_type'] === SPK_CLOSED ? 0 : s['audio.port_hz'] || 0;
  return { hz, measured: hz > 0 && s['audio.port_src'] === SRC_MEASURED };
}

/**
 * The speakers as catalog keys: `title`, then `line` with its `args` where there is a port to say
 * ('' where the title says it all — closed, or not set up); `again` when the wizard has been
 * through, so the button says «знову». No note under them: values, not explanations.
 */
export function speakerState(s) {
  const type = s['audio.spk_type'] ?? SPK_UNSET;
  const p = port(s);
  if (type === SPK_CLOSED) return { title: 'spk.closed', line: '', args: {}, again: true };
  const line = p.hz ? (p.measured ? 'spk.port_measured' : 'spk.port_manual') : 'spk.port_none';
  if (type === SPK_PORTED) return { title: 'spk.ported', line, args: { hz: p.hz }, again: true };
  // Not asked yet. A port somebody set before the wizard existed still plays, so it is said.
  return p.hz ? { title: 'spk.unset', line, args: { hz: p.hz }, again: false }
    : { title: 'spk.unset', line: '', args: {}, again: false };
}

/**
 * What a step of the wizard writes — the settings to send, or null for nothing at all. Choosing a
 * type writes nothing; «Пропустити» and «Лишити як було» write nothing; only «Готово» for closed
 * speakers and «Прийняти» for a port found in 30…80 Hz do.
 */
export function wizardWrites(type, action, hz) {
  if (action === 'done' && type === SPK_CLOSED) return { 'audio.spk_type': SPK_CLOSED };
  if (action === 'accept' && type === SPK_PORTED && Number.isFinite(hz)) {
    const f = Math.round(hz);
    if (f < 30 || f > 80) return null;
    return { 'audio.spk_type': SPK_PORTED, 'audio.port_hz': f, 'audio.port_src': SRC_MEASURED };
  }
  return null;
}

/** «Застосувати» on the calibration page: a measured frequency or one found by hand, 30…80 Hz. */
export function portWrites(hz, measured) {
  const f = Math.max(30, Math.min(80, Math.round(hz)));
  return { 'audio.spk_type': SPK_PORTED, 'audio.port_hz': f, 'audio.port_src': measured ? SRC_MEASURED : SRC_MANUAL };
}

/**
 * The port meter's last word (audio.port_meas_status, a "@cal.m_*" message) as a state of the
 * wizard: running while it measures, found / doubtful / failed when it has finished, idle for
 * anything else — not begun, stopped, interrupted.
 */
export function meterOutcome(status, measuring) {
  if (measuring) return 'running';
  const key = String(status || '').split('\x1f')[0];
  if (key === '@cal.m_found') return 'found';
  if (key === '@cal.m_doubtful') return 'doubtful';
  if (['@cal.m_outside', '@cal.m_no_dip', '@cal.m_no_tone', '@cal.m_mic_open', '@cal.m_mic_read',
       '@cal.m_no_memory'].includes(key)) return 'failed';
  return 'idle';
}

/** The subsonic under a port: 0.8 of it (DspChain::set_port). */
export const subUnder = (hz) => Math.round(0.8 * hz);

/**
 * «Звичайна гучність»: the reference for the volume that plays — % of the effective level, which
 * «Максимум» compresses, held to 20…100 as the device holds it — and back to the volume
 * scale for saying it.
 */
export function refOf(volume, volMax = 100) {
  const e = Math.round((Math.max(0, Math.min(100, volume)) * volMax) / 100);
  return Math.max(20, Math.min(100, e));
}
export function refShown(ref, volMax = 100) {
  return Math.min(100, Math.round((ref * 100) / (volMax || 100)));
}
