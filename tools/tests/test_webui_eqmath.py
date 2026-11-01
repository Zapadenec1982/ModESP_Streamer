"""The web's EQ curve is the firmware's: webui/src/lib/eqmath.js against the vectors the C++
host test writes from the design itself (tests/host/test_eq_curve_vectors.cpp).

What must hold: the same curve on the panel and in the browser. The JS is a port of
dsp_chain.cpp, so it is held to it number by number: each band matched and bilinear (double on
both sides, so a hair apart), and the summed curve the panel draws (float there, so a
thousandth of a dB apart at most, within the +-48 dB anything is drawn in).
"""

import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_eqmath_matches_the_firmware_design():
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI EQ maths regression"
    lib = (ROOT / "webui/src/lib/eqmath.js").as_uri()
    vectors = (ROOT / "tests/host/fixtures/eq_curve/vectors.json").read_text(encoding="utf-8")
    script = f"""
import {{ designBand, svfMagnitudeDb, bandMagnitudeDb, curveDb }} from {json.dumps(lib)};
const v = {vectors};
let worstBand = 0, worstBil = 0, worstCurve = 0, n = 0;
const fail = [];
for (const c of v.cases) {{
  c.bands.forEach((b, i) => {{
    v.freqs.forEach((f, j) => {{
      const m = bandMagnitudeDb(b, c.fs, f);
      const d = b.on ? designBand(b, c.fs, false) : null;
      const bil = d ? svfMagnitudeDb(d, c.fs, f) : 0;
      const em = Math.abs(m - c.band_db[i][j]), eb = Math.abs(bil - c.bilinear_db[i][j]);
      worstBand = Math.max(worstBand, em); worstBil = Math.max(worstBil, eb);
      if (!(em < 1e-6) || !(eb < 1e-6)) fail.push(`${{c.name}} band ${{i}} at ${{f}} Hz: ${{m}} / ${{bil}}`);
      n++;
    }});
  }});
  // Below -48 dB the panel's float curve is rounding noise (edges_*: a 20 kHz high pass on a
  // 20 Hz low pass sits at -150 dB), and neither plot shows past +-24.
  const clamp = (x) => Math.max(-48, Math.min(48, x));
  curveDb(c.bands, c.fs, v.freqs).forEach((db, j) => {{
    const e = Math.abs(clamp(db) - clamp(c.curve_db[j]));
    worstCurve = Math.max(worstCurve, e);
    if (!(e < 1e-3)) fail.push(`${{c.name}} curve at ${{v.freqs[j]}} Hz: ${{db}} vs ${{c.curve_db[j]}}`);
  }});
}}
if (fail.length) {{ console.error(fail.slice(0, 10).join('\\n')); process.exit(1); }}
console.log(`${{n}} points; worst ${{worstBand}} dB matched, ${{worstBil}} dB bilinear, ${{worstCurve}} dB curve`);
"""
    result = subprocess.run([node, "--input-type=module", "-e", script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
