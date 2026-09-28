// For every track of bench/lyrics_tracks.json: the entry the app's LRCLIB rule
// picks from /api/search (with the app's query cleaning), against what
// /api/get (exact title + artist + duration, server-side match) returns, and
// the first lines of each, so a wrong pick shows. Also times both calls and
// records the payload sizes. Run it when nothing else is measuring.
// Usage: deno run --allow-net --allow-read --allow-write lrclib_audit.ts
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const list = JSON.parse(Deno.readTextFileSync(`${here}../lyrics_tracks.json`)).tracks;
const ua = { "User-Agent": "Monolist/0.1 (desktop music player)" };

// lyrics.cpp leadArtist / searchTitle, reimplemented for the audit only.
function leadArtist(a: string) {
  return a.split(/\s*(?:,|&|\bfeat\.?|\bft\.?)\s*/i).filter(Boolean)[0]?.trim() ?? "";
}
function searchTitle(title: string, artist: string) {
  const dash = title.indexOf(" - ");
  const lead = leadArtist(artist);
  if (dash > 0 && lead && title.slice(0, dash).toLowerCase().includes(lead.toLowerCase())) title = title.slice(dash + 3);
  title = title.replace(/\s*[\(\[][^\)\]]*\b(?:official|video|audio|lyrics?|visuali[sz]er|mv|hd|hq|4k|remaster(?:ed)?)\b[^\)\]]*[\)\]]/gi, "");
  title = title.replace(/\s*[\(\[]\s*(?:feat\.?|ft\.?|featuring|with)\s[^\)\]]*[\)\]]/gi, "");
  title = title.replace(/\s+(?:feat\.?|ft\.?|featuring)\s.*$/i, "");
  return title.replace(/\s+/g, " ").trim();
}
const firstLines = (e: any) => ((e?.syncedLyrics || e?.plainLyrics) ?? "").split("\n")
  .map((l: string) => l.replace(/^(\[[^\]]*\])+\s*/, "").trim()).filter(Boolean).slice(0, 2).join(" / ").slice(0, 90);

const rows = [];
for (const t of list) {
  const title = searchTitle(t.title, t.artist), lead = leadArtist(t.artist), wanted = t.durationMs / 1000;
  let p = performance.now();
  const sr = await fetch(`https://lrclib.net/api/search?track_name=${encodeURIComponent(title)}&artist_name=${encodeURIComponent(lead)}`, { headers: ua });
  const sBody = await sr.text();
  const searchMs = Math.round(performance.now() - p);
  const results = JSON.parse(sBody);
  let pick: any = null, gap = 1e9;
  for (const e of results) {
    const g = Math.abs(e.duration - wanted);
    if (g <= 3 && (e.syncedLyrics ?? "").trim() && g < gap) { pick = e; gap = g; }
  }
  if (!pick) for (const e of results) {
    const g = Math.abs(e.duration - wanted);
    if (g <= 3 && (e.plainLyrics ?? "").trim() && g < gap) { pick = e; gap = g; }
  }
  p = performance.now();
  const gr = await fetch(`https://lrclib.net/api/get?track_name=${encodeURIComponent(title)}&artist_name=${encodeURIComponent(lead)}&duration=${Math.round(wanted)}&album_name=${encodeURIComponent(t.album ?? "")}`, { headers: ua });
  const gBody = await gr.text();
  const getMs = Math.round(performance.now() - p);
  const got = gr.ok ? JSON.parse(gBody) : null;
  const sameText = !!pick && !!got && firstLines(pick) === firstLines(got);
  rows.push({
    n: t.n, title: t.title, query: { track_name: title, artist_name: lead, duration: wanted },
    search: { ms: searchMs, bytes: sBody.length, results: results.length,
              pick: pick ? { id: pick.id, track: pick.trackName, artist: pick.artistName, album: pick.albumName, duration: pick.duration, synced: !!(pick.syncedLyrics ?? "").trim(), first: firstLines(pick) } : null,
              distinctTexts: new Set(results.map((e: any) => firstLines(e))).size },
    get: { ms: getMs, status: gr.status, bytes: gBody.length,
           entry: got ? { id: got.id, track: got.trackName, artist: got.artistName, album: got.albumName, duration: got.duration, synced: !!(got.syncedLyrics ?? "").trim(), instrumental: got.instrumental, first: firstLines(got) } : null },
    sameText,
  });
  console.log(`${t.n}\tsearch ${searchMs}ms ${sBody.length}B pick=${pick?.id ?? "-"}\tget ${gr.status} ${getMs}ms ${gBody.length}B id=${got?.id ?? "-"}\tsameText=${sameText}`);
}
Deno.writeTextFileSync(`${here}results/lrclib_audit.json`, JSON.stringify(rows, null, 2));
