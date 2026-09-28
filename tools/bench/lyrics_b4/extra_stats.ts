// Extra numbers from results/*.json: warm-connection lookups split by
// provider, the no-lyrics path's split, and how often each provider was the
// slowest one asked. Usage: deno run --allow-read extra_stats.ts
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const read = (p: string) => JSON.parse(Deno.readTextFileSync(`${here}results/${p}.json`));
function q(values: number[], p: number) {
  const v = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!v.length) return null;
  const pos = (v.length - 1) * p, lo = Math.floor(pos), hi = Math.ceil(pos);
  return Math.round(v[lo] + (v[hi] - v[lo]) * (pos - lo));
}
const st = (v: number[]) => `n=${v.length} median=${q(v, 0.5)} p90=${q(v, 0.9)} min=${Math.min(...v)} max=${Math.max(...v)}`;
const cold = Array.from({ length: 10 }, (_, i) => read(`cold${String(i + 1).padStart(2, "0")}`)).flat();
const proxy = Array.from({ length: 5 }, (_, i) => read(`proxy${String(i + 1).padStart(2, "0")}`)).flat();
const extras = cold.flatMap((r: any) => r.extra);
const byProv: Record<string, number[]> = {};
for (const e of extras) {
  if (e.ms === null) continue;
  const k = `${e.state}/${e.source ?? "-"}`;
  (byProv[k] ??= []).push(e.ms);
}
console.log("Warm-connection lookups (2nd and 3rd song of each cold process; other songs, not the fixed list):");
for (const [k, v] of Object.entries(byProv).sort((a, b) => b[1].length - a[1].length)) console.log(`  ${k.padEnd(45)} ${st(v)}`);
const lrclibOnlyFirst = cold.filter((r: any) => r.sameTrack && r.source === "LRCLIB");
console.log(`First lookup per process (fresh TLS to lrclib.net), LRCLIB answer: ${st(lrclibOnlyFirst.map((r: any) => r.ms))}`);
const valid = proxy.filter((r: any) => r.sameTrack && r.split);
console.log("\nProxy passes, per lookup:");
console.log(`  LRCLIB strict leg (in-app window) ${st(valid.map((r: any) => r.split.lrclibStrictMs))}`);
console.log(`  LRCLIB loose q= search used: ${valid.filter((r: any) => r.split.lrclibLooseQuery).length} of ${valid.length}`);
console.log(`  YouTube Music asked: ${valid.filter((r: any) => r.split.askedYouTube).length} of ${valid.length}`);
const slow: Record<string, number> = {};
for (const r of valid) slow[r.split.slowestProvider] = (slow[r.split.slowestProvider] ?? 0) + 1;
console.log(`  slowest provider asked: ${JSON.stringify(slow)}; slowest-provider time ${st(valid.map((r: any) => r.split.slowestProviderMs))}`);
for (const r of valid.filter((r: any) => r.split.askedYouTube))
  console.log(`  #${r.n} ${r.state} total=${r.ms} LRCLIB=${r.split.lrclibStrictMs} YTM=${r.split.youtubeMs} overhead=${r.split.overheadMs}`);
