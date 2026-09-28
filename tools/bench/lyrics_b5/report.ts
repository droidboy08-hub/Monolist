// Per-track and per-language tables from results/<pass>.json (run analyze.ts
// on the passes first). Writes results/report.txt and results/report.json.
// Usage: deno run --allow-read --allow-write report.ts
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const list = JSON.parse(Deno.readTextFileSync(`${here}../lyrics_tracks.json`)).tracks;
const load = (prefix: string) => {
  const out: any[] = [];
  for (const f of Deno.readDirSync(`${here}results`)) {
    if (f.name.match(new RegExp(`^${prefix}\\d+\\.json$`)))
      out.push(...JSON.parse(Deno.readTextFileSync(`${here}results/${f.name}`)).map((r: any) => ({ ...r, pass: f.name })));
  }
  return out;
};
function q(values: number[], p: number) {
  const v = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!v.length) return null;
  const pos = (v.length - 1) * p, lo = Math.floor(pos), hi = Math.ceil(pos);
  return Math.round(v[lo] + (v[hi] - v[lo]) * (pos - lo));
}
const mode = (xs: string[]) => {
  const c: Record<string, number> = {};
  for (const x of xs) c[x] = (c[x] ?? 0) + 1;
  return Object.entries(c).sort((a, b) => b[1] - a[1]).map(([k, n]) => `${k} x${n}`).join(", ");
};
const cold = load("cold"), proxy = load("proxy"), cached = load("cached"), yfail = load("proxyfail");
const measured = [...cold, ...proxy.filter((r) => !r.pass.startsWith("proxyfail"))];
const lines: string[] = [];
const rows = [];
lines.push("n  lang        title                                   cold med/p90 (n)   proxy LRCLIB leg med   cached med   class (all cold+proxy runs)                         YTM-alone class");
for (const t of list) {
  const c = cold.filter((r) => r.n === t.n && r.sameTrack);
  const pr = proxy.filter((r) => r.n === t.n && r.sameTrack && !r.pass.startsWith("proxyfail"));
  const ca = cached.filter((r) => r.n === t.n && r.sameTrack);
  const yf = yfail.filter((r) => r.n === t.n && r.sameTrack);
  const all = measured.filter((r) => r.n === t.n && r.sameTrack);
  const row = {
    n: t.n, language: t.language, title: t.title, artist: t.artist,
    coldMedian: q(c.map((r) => r.ms), 0.5), coldP90: q(c.map((r) => r.ms), 0.9), coldN: c.length,
    lrclibLegMedian: q(pr.map((r) => r.split?.lrclibStrictMs), 0.5),
    ytmLegMedianWhenAsked: q(pr.map((r) => r.split?.youtubeMs).filter((x) => x !== null && x !== undefined), 0.5),
    cachedMedian: q(ca.map((r) => r.ms), 0.5),
    classes: mode(all.map((r) => `${r.class}/${r.provider}`)),
    ytmAlone: mode(yf.map((r) => `${r.class}/${r.provider}`)),
    ytmAloneMedian: q(yf.map((r) => r.split?.youtubeMs), 0.5),
    firstLines: (c[0] ?? all[0])?.firstLines ?? [],
    driftRuns: [...cold, ...proxy, ...cached].filter((r) => r.n === t.n && !r.sameTrack).length,
  };
  rows.push(row);
  lines.push(`${String(t.n).padEnd(3)}${t.language.padEnd(12)}${(t.title + " - " + t.artist).slice(0, 40).padEnd(40)}${String(row.coldMedian).padStart(5)}/${String(row.coldP90).padEnd(5)}(${row.coldN})`.padEnd(90) +
    `${String(row.lrclibLegMedian).padEnd(22)}${String(row.cachedMedian).padEnd(13)}${row.classes.padEnd(52)}${row.ytmAlone}`);
}
// Hit rate by language on the modal class of each track.
const byLang: Record<string, Record<string, number>> = {};
for (const r of rows) {
  const lang = ["Hindi", "Punjabi"].includes(r.language) ? "Hindi/Punjabi"
    : ["French", "German"].includes(r.language) ? "French/German" : r.language;
  const cls = r.classes.split(" x")[0].split("/")[0];
  byLang[lang] ??= {};
  byLang[lang][cls] = (byLang[lang][cls] ?? 0) + 1;
}
lines.push("", "Modal class by language:");
for (const [k, v] of Object.entries(byLang)) lines.push(`  ${k.padEnd(14)} ${JSON.stringify(v)}`);
lines.push("", "First lines (first cold run):");
for (const r of rows) lines.push(`  ${String(r.n).padEnd(3)} ${r.firstLines.join(" / ").slice(0, 120)}`);
Deno.writeTextFileSync(`${here}results/report.txt`, lines.join("\n"));
Deno.writeTextFileSync(`${here}results/report.json`, JSON.stringify({ rows, byLanguage: byLang }, null, 2));
console.log(lines.join("\n"));
