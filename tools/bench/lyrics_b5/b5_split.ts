// Where a cold first lookup's time goes, from the app's own lines: LRCLIB's
// total, how long after /api/get was sent it answered (so the rest is the
// connection: DNS, TCP, TLS), the race's decision, and the time from the
// decision to the lines on show (the self-test's line).
// Usage: deno run --allow-read b5_split.ts <pass> [<pass> ...]
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
function quantile(values: number[], q: number): number | null {
  const v = values.filter((x) => Number.isFinite(x)).sort((a, b) => a - b);
  if (!v.length) return null;
  const pos = (v.length - 1) * q;
  const lo = Math.floor(pos), hi = Math.ceil(pos);
  return Math.round(v[lo] + (v[hi] - v[lo]) * (pos - lo));
}
const st = (v: number[]) => v.length ? `n=${v.length} median=${quantile(v, 0.5)} p90=${quantile(v, 0.9)} max=${Math.max(...v)}` : "n=0";
const kinds: Record<string, string[]> = {};
for (const p of Deno.args) (kinds[p.replace(/\d+$/, "")] ??= []).push(p);
const wall = (s: string) => new Date(s).getTime();
for (const [kind, passes] of Object.entries(kinds)) {
  const total: number[] = [], toSent: number[] = [], afterSent: number[] = [], ytm: number[] = [], render: number[] = [], shown: number[] = [];
  for (const pass of passes) {
    for (let n = 1; n <= 30; n++) {
      let text = "";
      try { text = Deno.readTextFileSync(`${here}logs/${pass}/${String(n).padStart(2, "0")}.err`); } catch (_) { continue; }
      const lines = text.split(/\r?\n/);
      let decidedAt: number | null = null, lr: RegExpMatchArray | null = null, rc: RegExpMatchArray | null = null;
      for (const l of lines) {
        let m;
        if (!lr && (m = l.match(/^(\S+) lyrics: LRCLIB \w+ in (\d+) ms for \S+: get found (\d+) ms after sent/))) lr = m;
        if (!rc && (m = l.match(/^(\S+) lyrics: race for \S+ decided in (\d+) ms: .*YouTube Music (?:plain|none|failed) (\d+) ms/))) rc = m;
        if (decidedAt === null && (m = l.match(/^(\S+) lyrics: race for \S+ decided in/))) decidedAt = wall(m[1]);
        // Before the race (the control), LRCLIB's own answer decided it.
        if (decidedAt === null && (m = l.match(/^(\S+) lyrics: LRCLIB (?:synced|plain|instrumental) in/))) decidedAt = wall(m[1]);
        if ((m = l.match(/^(\S+) selftest: {3}(\w+) in (\d+) ms/))) {
          shown.push(Number(m[3]));
          if (decidedAt !== null) render.push(wall(m[1]) - decidedAt);
          break;
        }
      }
      if (lr) { total.push(Number(lr[2])); afterSent.push(Number(lr[3])); toSent.push(Number(lr[2]) - Number(lr[3])); }
      if (rc) ytm.push(Number(rc[3]));
    }
  }
  console.log(`== ${kind}`);
  console.log(`  first lookup on screen:        ${st(shown)}`);
  console.log(`  LRCLIB total:                  ${st(total)}`);
  console.log(`  ... before /api/get was sent:  ${st(toSent)}`);
  console.log(`  ... after it was sent:         ${st(afterSent)}`);
  console.log(`  YouTube Music's own answer:    ${st(ytm)}`);
  console.log(`  race decided -> lines on show: ${st(render)}`);
}
