// B5 numbers that analyze.ts does not give:
//  - for --lyrics passes: when the first lines were on screen (the race's
//    interim answer, "interim <state> shown after N ms", else the answer
//    itself), beside when the answer was final; per lookup kind (first lookup
//    of the process, the two warm ones, the one from the database);
//  - "no lyrics" (state none) times, first and warm lookups;
//  - for --lyrics-pane passes: time from opening to the first lines and to
//    the answer, per arm (on/off) and step (current/next), and how often a
//    lookup was still out when the lyrics were opened.
// Quantiles interpolated, as analyze.ts computes them.
// Usage: deno run --allow-read b5_analyze.ts <pass> [<pass> ...]
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const list = JSON.parse(Deno.readTextFileSync(`${here}../lyrics_tracks.json`)).tracks;

function quantile(values: number[], q: number): number | null {
  const v = values.filter((x) => Number.isFinite(x)).sort((a, b) => a - b);
  if (!v.length) return null;
  const pos = (v.length - 1) * q;
  const lo = Math.floor(pos), hi = Math.ceil(pos);
  return Math.round(v[lo] + (v[hi] - v[lo]) * (pos - lo));
}
const st = (v: number[]) => v.length
  ? `n=${v.length} median=${quantile(v, 0.5)} p90=${quantile(v, 0.9)} min=${Math.min(...v)} max=${Math.max(...v)}`
  : "n=0";

type Lookup = { interim: number | null; interimState: string | null; state: string | null; ms: number | null; source: string | null };

function lookups(text: string): Lookup[] {
  const out: Lookup[] = [];
  for (const raw of text.split(/\r?\n/)) {
    const msg = raw.replace(/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3} /, "");
    let x;
    if (/^selftest: lyrics for "/.test(msg)) {
      out.push({ interim: null, interimState: null, state: null, ms: null, source: null });
    } else if ((x = msg.match(/^selftest: {3}interim (\w+) shown after (\d+) ms/))) {
      const l = out[out.length - 1];
      if (l && l.interim === null) { l.interim = Number(x[2]); l.interimState = x[1]; }
    } else if ((x = msg.match(/^selftest: {3}(\w+) in (\d+) ms, (\d+) lines, from (.*?)(?: \((.*)\))?$/))) {
      const l = out[out.length - 1];
      if (l && l.state === null) { l.state = x[1]; l.ms = Number(x[2]); l.source = x[4]; }
    }
  }
  return out;
}

const lyricsPasses = Deno.args.filter((p) => !p.startsWith("pane_"));
const panePasses = Deno.args.filter((p) => p.startsWith("pane_"));

// Pooled by kind (the pass name without its number).
const kinds: Record<string, string[]> = {};
for (const p of lyricsPasses) (kinds[p.replace(/\d+$/, "")] ??= []).push(p);
for (const [kind, passes] of Object.entries(kinds)) {
  const firstShown: number[] = [], firstFinal: number[] = [], firstShownFound: number[] = [];
  const noneFirst: number[] = [], noneWarm: number[] = [], interimFirst: number[] = [];
  const states: Record<string, number> = {};
  let firstWithInterim = 0, firstCount = 0;
  for (const pass of passes) {
    for (const t of list) {
      const nn = String(t.n).padStart(2, "0");
      let text = "";
      try { text = Deno.readTextFileSync(`${here}logs/${pass}/${nn}.err`); } catch (_) { continue; }
      const ls = lookups(text);
      const first = ls[0];
      if (!first || first.ms === null) continue;
      firstCount++;
      states[first.state!] = (states[first.state!] ?? 0) + 1;
      const shown = first.interim ?? first.ms;
      firstShown.push(shown);
      firstFinal.push(first.ms);
      if (first.state === "synced" || first.state === "plain") firstShownFound.push(shown);
      if (first.interim !== null) { firstWithInterim++; interimFirst.push(first.interim); }
      if (first.state === "none") noneFirst.push(first.ms);
      for (const w of ls.slice(1, 3)) if (w.state === "none" && w.ms !== null) noneWarm.push(w.ms);
    }
  }
  console.log(`== ${kind} (${passes.join(", ")})`);
  console.log(`  first lookup states: ${JSON.stringify(states)}; with an interim answer: ${firstWithInterim} of ${firstCount}`);
  console.log(`  first lookup, first lines on screen (lyrics found): ${st(firstShownFound)}`);
  console.log(`  first lookup, first lines on screen (all):          ${st(firstShown)}`);
  console.log(`  first lookup, the interim answer itself:            ${st(interimFirst)}`);
  console.log(`  first lookup, final answer:                         ${st(firstFinal)}`);
  console.log(`  "none", first lookup:                               ${st(noneFirst)}`);
  console.log(`  "none", warm lookups (2nd and 3rd song):            ${st(noneWarm)}`);
}

if (panePasses.length) {
  const groups: Record<string, { first: number[]; final: number[]; out: number; n: number; states: Record<string, number> }> = {};
  for (const pass of panePasses) {
    const arm = pass.replace(/^pane_/, "").replace(/\d+$/, "");
    for (const t of list) {
      const nn = String(t.n).padStart(2, "0");
      let text = "";
      try { text = Deno.readTextFileSync(`${here}logs/${pass}/${nn}.err`); } catch (_) { continue; }
      for (const raw of text.split(/\r?\n/)) {
        const m = raw.match(/selftest: pane-open step=(\w+) dwell=(\d+) first=(-?\d+) final=(\d+) state=(\w+) lines=(\d+) out=(\w+)/);
        if (!m) continue;
        const key = `${arm}/${m[1]}`;
        const g = (groups[key] ??= { first: [], final: [], out: 0, n: 0, states: {} });
        g.n++;
        g.first.push(Number(m[3]));
        g.final.push(Number(m[4]));
        if (m[7] === "yes") g.out++;
        g.states[m[5]] = (g.states[m[5]] ?? 0) + 1;
      }
    }
  }
  console.log(`== pane (${panePasses.join(", ")})`);
  for (const [key, g] of Object.entries(groups).sort()) {
    console.log(`  ${key}: opened ${g.n} times, a lookup still out ${g.out}; states ${JSON.stringify(g.states)}`);
    console.log(`    open -> first lines: ${st(g.first)}`);
    console.log(`    open -> answer:      ${st(g.final)}`);
  }
  // Both steps together, per arm.
  for (const arm of ["on", "off"]) {
    const first = Object.entries(groups).filter(([k]) => k.startsWith(arm + "/")).flatMap(([, g]) => g.first);
    if (first.length) console.log(`  ${arm}, both steps: open -> first lines ${st(first)}`);
  }
}
