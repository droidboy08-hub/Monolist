// Warm-connection lookups (the 2nd and 3rd song of each process: other songs
// than the fixed list) by outcome, for the B4 cold passes against the
// same-day control (base). Same numbers as extra_stats.ts's first block.
// Usage: deno run --allow-read warm_compare.ts
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const read = (p: string) => JSON.parse(Deno.readTextFileSync(`${here}results/${p}.json`));
function q(values: number[], p: number) {
  const v = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!v.length) return null;
  const pos = (v.length - 1) * p, lo = Math.floor(pos), hi = Math.ceil(pos);
  return Math.round(v[lo] + (v[hi] - v[lo]) * (pos - lo));
}
for (const kind of ["cold", "base"]) {
  const rows = Array.from({ length: 10 }, (_, i) => read(`${kind}${String(i + 1).padStart(2, "0")}`)).flat();
  const by: Record<string, number[]> = {};
  for (const e of rows.flatMap((r: any) => r.extra)) {
    if (e.ms === null) continue;
    const k = `${e.state}/${e.source ?? "-"}`;
    (by[k] ??= []).push(e.ms);
  }
  console.log(`${kind}:`);
  for (const [k, v] of Object.entries(by).sort((a, b) => b[1].length - a[1].length))
    console.log(`  ${k.padEnd(40)} n=${v.length} median=${q(v, 0.5)} p90=${q(v, 0.9)}`);
  const noLyrics = rows.flatMap((r: any) => r.extra).filter((e: any) => e.state === "none" || e.state === "error").map((e: any) => e.ms);
  console.log(`  "none" or "error" together: n=${noLyrics.length} median=${q(noLyrics, 0.5)} p90=${q(noLyrics, 0.9)}`);
}
