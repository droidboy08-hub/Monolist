// Shows what LRCLIB's /api/search returns for one query, and which entry the
// app's rule (closest synced within 3 s, else plain within 3 s) would take.
// Also asks /api/get (exact title/artist/duration), which the app never uses.
// Usage: deno run --allow-net lrclib_inspect.ts "<track_name>" "<artist_name>" <durationSec> [album]
const [track, artist, dur, album] = Deno.args;
const wanted = Number(dur);
const ua = { "User-Agent": "Monolist/0.1 (desktop music player)" };
const q = `track_name=${encodeURIComponent(track)}&artist_name=${encodeURIComponent(artist)}`;
let t = performance.now();
const res = await (await fetch(`https://lrclib.net/api/search?${q}`, { headers: ua })).json();
console.log(`search: ${res.length} results in ${Math.round(performance.now() - t)} ms`);
let best: any = null, bestGap = 1e9;
for (const e of res) {
  const gap = Math.abs(e.duration - wanted);
  const synced = !!(e.syncedLyrics ?? "").trim();
  if (gap <= 3 && synced && gap < bestGap) { best = e; bestGap = gap; }
  const first = (e.syncedLyrics ?? e.plainLyrics ?? "").split("\n").map((l: string) => l.replace(/^\[[^\]]*\]\s*/, "").trim())
    .filter(Boolean).slice(0, 2).join(" / ");
  console.log(`  id=${e.id} | ${e.trackName} | ${e.artistName} | ${e.albumName} | ${e.duration}s gap=${gap.toFixed(1)} ${synced ? "S" : ""}${e.plainLyrics ? "P" : ""}${e.instrumental ? "I" : ""} | ${first.slice(0, 80)}`);
}
console.log(`app would take: ${best ? `id=${best.id} ${best.trackName} / ${best.artistName} (gap ${bestGap.toFixed(1)} s)` : "no synced within 3 s"}`);
const g = `track_name=${encodeURIComponent(track)}&artist_name=${encodeURIComponent(artist)}&duration=${wanted}` +
  (album ? `&album_name=${encodeURIComponent(album)}` : "");
t = performance.now();
const gr = await fetch(`https://lrclib.net/api/get?${g}`, { headers: ua });
const gj = gr.ok ? await gr.json() : null;
console.log(`get: HTTP ${gr.status} in ${Math.round(performance.now() - t)} ms` +
  (gj ? ` -> id=${gj.id} | ${gj.trackName} | ${gj.artistName} | ${gj.albumName} | ${gj.duration}s ${gj.syncedLyrics ? "S" : ""}${gj.plainLyrics ? "P" : ""} | ` +
    (gj.syncedLyrics ?? gj.plainLyrics ?? "").split("\n").map((l: string) => l.replace(/^\[[^\]]*\]\s*/, "").trim())
      .filter(Boolean).slice(0, 2).join(" / ").slice(0, 80) : ""));
