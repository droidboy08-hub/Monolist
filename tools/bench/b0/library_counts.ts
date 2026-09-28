// B0 (1): aggregate-only counts from a COPY of a Monolist library database.
// Prints numbers only: no titles, artists, video ids or timestamps of single plays.
// Usage: deno run --allow-read --allow-write library_counts.ts <copy of monolist.db>
import { DatabaseSync } from "node:sqlite";

const path = Deno.args[0];
const db = new DatabaseSync(path);

const GiB = 1024 * 1024 * 1024;
const MiB = 1024 * 1024;
// Whole-file size estimate: the Opus 251 / AAC 140 file the app picks, measured on
// the 12 benchmark tracks (qt_probe/loudness.json clen / duration): median 16.8 kB/s,
// range 15.8-18.3 kB/s.
const BYTES_PER_SEC = 16800;

function tables(): string[] {
  return (db.prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").all() as any[]).map((r) => r.name);
}
function count(sql: string): number {
  const r = db.prepare(sql).get() as any;
  return Number(Object.values(r)[0]);
}
function pct(a: number, b: number) { return b ? Math.round((1000 * a) / b) / 10 : null; }
function median(xs: number[]) { if (!xs.length) return null; const s = [...xs].sort((a, b) => a - b); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; }

const out: any = { tables: {} };
for (const t of tables()) out.tables[t] = count(`SELECT COUNT(*) FROM "${t}"`);

const hasPE = tables().includes("play_events");
if (hasPE) {
  out.playEventKinds = Object.fromEntries((db.prepare("SELECT kind, COUNT(*) n FROM play_events GROUP BY kind").all() as any[]).map((r) => [r.kind, r.n]));
  // Only short enum-like source words are shown; anything else is pooled, so no name leaks.
  out.playSources = {} as Record<string, number>;
  for (const r of db.prepare("SELECT source, COUNT(*) n FROM play_events WHERE kind='play' GROUP BY source").all() as any[]) {
    const k = !r.source ? "(empty)" : /^[A-Za-z_]{1,16}$/.test(r.source) ? r.source : "(other)";
    out.playSources[k] = (out.playSources[k] || 0) + Number(r.n);
  }
  const span = db.prepare("SELECT MIN(started_at) a, MAX(started_at) b, COUNT(DISTINCT substr(started_at,1,10)) days FROM play_events WHERE kind='play'").get() as any;
  // Only the span in days, not the dates themselves.
  out.playSpanDays = span.a && span.b ? Math.round((Date.parse(span.b.replace(" ", "T") + "Z") - Date.parse(span.a.replace(" ", "T") + "Z")) / 864e5 * 10) / 10 : null;
  out.distinctPlayDays = span.days;

  // Durations known from the recent table, for plays whose track_ms is 0.
  const recentDur = new Map<string, number>();
  if (tables().includes("recent")) for (const r of db.prepare("SELECT video_id, duration_ms FROM recent").all() as any[]) recentDur.set(r.video_id, Number(r.duration_ms));

  const plays = db.prepare("SELECT video_id v, track_ms t, listened_ms l, completed c, skipped s, repeat_in_session r FROM play_events WHERE kind='play' ORDER BY started_at, id").all() as any[];
  const knownDur = plays.map((p) => Number(p.t) || recentDur.get(p.v) || 0).filter((x) => x > 0);
  const fallbackMs = median(knownDur) ?? 210000;
  let noId = 0, durUnknown = 0;
  const lru = new Map<string, number>(); // insertion order = recency
  let lruBytes = 0;
  const everSeen = new Set<string>();
  let hitsLRU = 0, hitsEver = 0, hitsLRU512 = 0, backToBack = 0, repeatFlag = 0;
  const lru512 = new Map<string, number>(); let lru512Bytes = 0;
  let prev = "";
  let long450 = 0, long8MiB = 0, longListenMs = 0, allListenMs = 0, longBytes = 0, allBytes = 0;
  let totalBytes = 0;
  const durations: number[] = [];
  for (const p of plays) {
    const v = String(p.v || "");
    if (!v) { noId++; continue; }
    let ms = Number(p.t) || recentDur.get(v) || 0;
    if (!ms) { durUnknown++; ms = fallbackMs; }
    durations.push(ms);
    const bytes = (ms / 1000) * BYTES_PER_SEC;
    totalBytes += bytes;
    if (everSeen.has(v)) hitsEver++;
    everSeen.add(v);
    if (lru.has(v)) { hitsLRU++; lru.delete(v); } else { lruBytes += bytes; }
    lru.set(v, bytes);
    for (const [k, b] of lru) { if (lruBytes <= GiB) break; lru.delete(k); lruBytes -= b; }
    if (lru512.has(v)) { hitsLRU512++; lru512.delete(v); } else { lru512Bytes += bytes; }
    lru512.set(v, bytes);
    for (const [k, b] of lru512) { if (lru512Bytes <= GiB / 2) break; lru512.delete(k); lru512Bytes -= b; }
    if (v === prev) backToBack++;
    prev = v;
    if (Number(p.r)) repeatFlag++;
    const listened = Math.max(0, Number(p.l) || 0);
    allListenMs += listened; allBytes += bytes;
    if (ms > 450000) { long450++; longListenMs += listened; longBytes += bytes; }
    if (bytes > 8 * MiB) long8MiB++;
  }
  const n = plays.length - noId;
  out.plays = {
    total: plays.length, withVideoId: n, withoutVideoId: noId, durationUnknownUsedMedian: durUnknown,
    distinctSongs: everSeen.size,
    estimatedListeningBytesGiB: Math.round((totalBytes / GiB) * 100) / 100,
    medianTrackMin: durations.length ? Math.round((median(durations)! / 60000) * 100) / 100 : null,
  };
  out.replay = {
    repeatOfAnyEarlierPlay: { n: hitsEver, pct: pct(hitsEver, n) },
    hitIn1GiBLRU: { n: hitsLRU, pct: pct(hitsLRU, n) },
    hitIn512MiBLRU: { n: hitsLRU512, pct: pct(hitsLRU512, n) },
    sameSongAsPreviousPlay: { n: backToBack, pct: pct(backToBack, n) },
    repeatInSessionFlag: { n: repeatFlag, pct: pct(repeatFlag, n) },
  };
  out.long = {
    playsOver7m30s: { n: long450, pct: pct(long450, n) },
    playsOver8MiBEstimated: { n: long8MiB, pct: pct(long8MiB, n), note: "track_s x 16.8 kB/s > 8 MiB, i.e. > ~8.3 min" },
    shareOfListenedTimeOver7m30s: pct(longListenMs, allListenMs),
    shareOfEstimatedBytesOver7m30s: pct(longBytes, allBytes),
  };
  // Distinct songs longer than 7.5 min, among distinct songs played.
  const distinctLong = new Set<string>();
  for (const p of plays) { const ms = Number(p.t) || recentDur.get(p.v) || 0; if (p.v && ms > 450000) distinctLong.add(p.v); }
  out.long.distinctSongsOver7m30s = { n: distinctLong.size, pct: pct(distinctLong.size, everSeen.size) };
}

// The recent table as a cross-check: play_count per song (replays of the same song).
if (tables().includes("recent")) {
  const r = db.prepare("SELECT COUNT(*) songs, COALESCE(SUM(play_count),0) plays, SUM(CASE WHEN play_count>1 THEN 1 ELSE 0 END) repeated, SUM(CASE WHEN duration_ms>450000 THEN 1 ELSE 0 END) long_songs, SUM(CASE WHEN duration_ms>450000 THEN play_count ELSE 0 END) long_plays FROM recent").get() as any;
  out.recentTable = {
    songs: r.songs, plays: r.plays, songsPlayedMoreThanOnce: r.repeated,
    playsThatRepeatASong: r.plays - r.songs, pctPlaysThatRepeat: pct(r.plays - r.songs, r.plays),
    songsOver7m30s: r.long_songs, playsOfSongsOver7m30s: r.long_plays, pctPlaysOver7m30s: pct(r.long_plays, r.plays),
  };
}
if (tables().includes("history")) {
  const h = db.prepare("SELECT COUNT(*) n, COUNT(DISTINCT track_id) d FROM history").get() as any;
  out.historyTable = { plays: h.n, distinctTracks: h.d, pctRepeat: pct(h.n - h.d, h.n) };
}
console.log(JSON.stringify(out, null, 2));
