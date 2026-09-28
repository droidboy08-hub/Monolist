// Parses the logs written by run_lyrics.ps1 and summarises the lyrics metrics
// (paper §15.1): hit rate by class and language, time to lyrics (median/p90),
// which provider answered, per-provider time (proxy passes only), cached time.
//
// Usage: deno run --allow-read --allow-write analyze.ts <pass> [<pass> ...]
//   Pass names starting with "cold" are fresh-database runs, "cached" re-runs
//   over a cold pass's databases, "proxy" cold runs with LRCLIB through
//   lrclib_proxy.ts. Writes results\<pass>.json and results\summary.json.
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const list = JSON.parse(Deno.readTextFileSync(`${here}../lyrics_tracks.json`)).tracks;
Deno.mkdirSync(`${here}results`, { recursive: true });

type Lookup = {
  title: string; artist: string; duration: string; tStart: number; tEnd: number | null;
  state: string | null; ms: number | null; lines: number | null; source: string | null; error: string | null;
  shown: string[];
};

function wall(stamp: string): number {
  return new Date(stamp).getTime(); // local time, as the app wrote it
}

function parseLog(text: string) {
  const lookups: Lookup[] = [];
  let timedOut = false;
  for (const raw of text.split(/\r?\n/)) {
    const m = raw.match(/^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}) (.*)$/);
    if (!m) continue;
    const t = wall(m[1]);
    const msg = m[2];
    let x;
    if ((x = msg.match(/^selftest: lyrics for "(.*)" by (.*), (\d+:\d\d(?::\d\d)?)$/))) {
      lookups.push({ title: x[1], artist: x[2], duration: x[3], tStart: t, tEnd: null, state: null, ms: null,
                     lines: null, source: null, error: null, shown: [] });
    } else if ((x = msg.match(/^selftest: {3}(\w+) in (\d+) ms, (\d+) lines, from (.*?)(?: \((.*)\))?$/))) {
      const l = lookups[lookups.length - 1];
      if (l) {
        l.tEnd = t; l.state = x[1]; l.ms = Number(x[2]); l.lines = Number(x[3]);
        l.source = x[4] === "-" ? null : x[4]; l.error = x[5] ?? null;
      }
    } else if ((x = msg.match(/^selftest: {5}(.*)$/))) {
      const l = lookups[lookups.length - 1];
      if (l) l.shown.push(x[1]);
    } else if (msg.startsWith("selftest: timed out")) {
      timedOut = true;
    }
  }
  return { lookups, timedOut };
}

// Same comparison both sides of the ANSI mangling: non-ASCII -> "?".
const norm = (s: string) => s.replace(/[^\x20-\x7e]/g, "?").replace(/\?+/g, "?").trim().toLowerCase();

function classify(state: string | null, wordStamps: number): string {
  switch (state) {
    case "synced": return wordStamps > 0 ? "word-synced (shown line-synced)" : "line-synced";
    case "plain": return "plain";
    case "instrumental": return "none (instrumental flag)";
    case "none": return "none";
    case "error": return "error";
    default: return "no answer";
  }
}

function providerOf(state: string | null, source: string | null): string {
  if (!source) return state === "instrumental" ? "LRCLIB (instrumental flag)" : "-";
  if (source === "LRCLIB") return "LRCLIB";
  if (source.endsWith("via YouTube Music")) return "YouTube Music (" + source.replace(" via YouTube Music", "") + ")";
  return source;
}

// What the app does with an LRCLIB answer (lyrics.cpp handleLrclib), so the
// proxy log tells whether YouTube Music was asked next.
function lrclibVerdict(entries: any[], wantedSec: number) {
  let synced = null, plain = null, instrumental = null, nearest = null;
  let sg = 1e9, pg = 1e9, ng = 1e9;
  for (const e of entries) {
    const gap = wantedSec > 0 ? Math.abs(Number(e.duration) - wantedSec) : 0;
    if (gap <= 3) {
      if (e.synced && gap < sg) { synced = e; sg = gap; }
      if (e.plain && gap < pg) { plain = e; pg = gap; }
      if (!instrumental && e.instrumental) instrumental = e;
    } else if ((e.synced || e.plain) && gap <= 20 && gap < ng) { nearest = e; ng = gap; }
  }
  const chosen = synced ?? plain ?? instrumental;
  return { chosen, gap: synced ? sg : plain ? pg : null, nearest, nearestGap: nearest ? ng : null,
           answered: !!chosen };
}

function quantile(values: number[], q: number): number | null {
  const v = values.filter((x) => Number.isFinite(x)).sort((a, b) => a - b);
  if (!v.length) return null;
  const pos = (v.length - 1) * q;
  const lo = Math.floor(pos), hi = Math.ceil(pos);
  return Math.round(v[lo] + (v[hi] - v[lo]) * (pos - lo));
}
const stats = (v: number[]) => ({ n: v.length, median: quantile(v, 0.5), p90: quantile(v, 0.9),
                                   min: v.length ? Math.min(...v) : null, max: v.length ? Math.max(...v) : null });

const summary: any = { generated: new Date().toISOString(), passes: {} };
for (const pass of Deno.args) {
  const dir = `${here}logs/${pass}`;
  let proxy: any[] = [];
  try {
    proxy = Deno.readTextFileSync(`${dir}/lrclib_proxy.jsonl`).split("\n").filter(Boolean).map((l) => JSON.parse(l));
  } catch (_) { /* not a proxy pass */ }
  const records = [];
  for (const t of list) {
    const nn = String(t.n).padStart(2, "0");
    let text = "";
    try { text = Deno.readTextFileSync(`${dir}/${nn}.err`); } catch (_) { continue; }
    const { lookups, timedOut } = parseLog(text);
    let db: any[] = [];
    try { db = JSON.parse(Deno.readTextFileSync(`${dir}/${nn}.db.json`)); } catch (_) { /* none */ }
    const row = db.find((r) => r.videoId === t.videoId) ?? null;
    const target = lookups[0] ?? null;
    const sameTrack = !!target && norm(target.title) === norm(t.appLogTitle) && target.duration === t.duration
      && norm(target.artist) === norm(t.artist);
    const rec: any = {
      n: t.n, language: t.language, videoId: t.videoId, title: t.title, artist: t.artist, duration: t.duration,
      expect: t.expect, timedOut, sameTrack, dbRowForVideoId: !!row,
      state: target?.state ?? null, ms: target?.ms ?? null, lines: target?.lines ?? null,
      source: target?.source ?? null, error: target?.error ?? null,
      class: classify(target?.state ?? null, row?.wordStamps ?? 0),
      provider: providerOf(target?.state ?? null, target?.source ?? null),
      wordStampsInStoredLrc: row?.wordStamps ?? 0,
      firstLines: row?.firstLines ?? [],
      // The same process's later lookups: other songs (warm connection) and the
      // first one again from the database.
      extra: lookups.slice(1, 3).map((l) => ({ title: l.title, artist: l.artist, state: l.state, ms: l.ms, source: l.source })),
      repeatFromDb: lookups[3] ? { state: lookups[3].state, ms: lookups[3].ms, sameAsFirst: lookups[3].state === target?.state } : null,
    };
    if (proxy.length && target && target.tEnd) {
      // LRCLIB calls made during the first lookup.
      const calls = proxy.filter((p) => p.tIn >= target.tStart - 5 && p.tIn <= target.tEnd!);
      const strict = calls.find((c) => c.query.track_name !== undefined);
      const loose = calls.find((c) => c.query.q !== undefined);
      const last = loose ?? strict;
      const verdict = last ? lrclibVerdict(last.entries, t.durationMs / 1000) : null;
      const lrclibInApp = calls.reduce((s, c) => s + (c.tOut - c.tIn), 0);
      const askedYouTube = !!last && !(verdict?.answered) && last.status === 200 ? true
        : (!!last && last.status !== 200);
      const ytmMs = askedYouTube && last ? target.tEnd - last.tOut : null;
      rec.split = {
        lrclibQuery: strict ? strict.url : null,
        lrclibStrictMs: strict ? strict.tOut - strict.tIn : null,
        lrclibStrictUpstreamMs: strict?.upstreamMs ?? null,
        lrclibStrictResults: strict?.results ?? null,
        lrclibLooseQuery: loose ? loose.url : null,
        lrclibLooseMs: loose ? loose.tOut - loose.tIn : null,
        lrclibLooseResults: loose?.results ?? null,
        lrclibTotalMs: lrclibInApp,
        askedYouTube,
        youtubeMs: ytmMs,
        overheadMs: target.ms! - lrclibInApp - (ytmMs ?? 0),
        slowestProvider: ytmMs !== null && ytmMs > lrclibInApp ? "YouTube Music" : "LRCLIB",
        slowestProviderMs: Math.max(lrclibInApp, ytmMs ?? 0),
        lrclibChosen: verdict?.chosen ? { track: verdict.chosen.track, artist: verdict.chosen.artist,
                                          album: verdict.chosen.album, duration: verdict.chosen.duration,
                                          gapSec: verdict.gap, synced: verdict.chosen.synced } : null,
        lrclibNearest: verdict?.nearest ? { track: verdict.nearest.track, artist: verdict.nearest.artist,
                                            duration: verdict.nearest.duration, gapSec: verdict.nearestGap } : null,
        lrclibEntries: (last?.entries ?? []).slice(0, 8).map((e: any) =>
          `${e.track} | ${e.artist} | ${e.duration}s${e.synced ? " S" : ""}${e.plain ? " P" : ""}${e.instrumental ? " I" : ""}`),
      };
    }
    records.push(rec);
  }
  Deno.writeTextFileSync(`${here}results/${pass}.json`, JSON.stringify(records, null, 2));

  const valid = records.filter((r) => r.sameTrack && !r.timedOut);
  const byClass: Record<string, number> = {};
  const byProvider: Record<string, number> = {};
  const byLanguage: Record<string, Record<string, number>> = {};
  for (const r of valid) {
    byClass[r.class] = (byClass[r.class] ?? 0) + 1;
    byProvider[r.provider] = (byProvider[r.provider] ?? 0) + 1;
    const lang = ["Hindi", "Punjabi"].includes(r.language) ? "Hindi/Punjabi"
      : ["French", "German"].includes(r.language) ? "French/German" : r.language;
    byLanguage[lang] ??= {};
    byLanguage[lang][r.class] = (byLanguage[lang][r.class] ?? 0) + 1;
  }
  const timesBy = (f: (r: any) => boolean) => stats(valid.filter(f).map((r) => r.ms));
  const p: any = {
    tracks: records.length, sameTrack: valid.length,
    mismatched: records.filter((r) => !r.sameTrack).map((r) => r.n),
    timedOut: records.filter((r) => r.timedOut).map((r) => r.n),
    byClass, byProvider, byLanguage,
    timeToLyricsAll: timesBy(() => true),
    timeToLyricsHits: timesBy((r) => r.state === "synced" || r.state === "plain"),
    timeToLyricsByProvider: Object.fromEntries(Object.keys(byProvider).map((k) => [k, timesBy((r) => r.provider === k)])),
    timeToNoLyrics: timesBy((r) => r.state === "none" || r.state === "instrumental"),
    repeatFromDbSameProcess: stats(valid.map((r) => r.repeatFromDb?.ms).filter((x) => x !== undefined && x !== null)),
    extraLookupsWarmConnection: stats(valid.flatMap((r) => r.extra.map((e: any) => e.ms)).filter((x) => x !== null)),
  };
  if (proxy.length) {
    const s = valid.filter((r) => r.split);
    p.split = {
      lrclibStrictMs: stats(s.map((r) => r.split.lrclibStrictMs).filter((x) => x !== null)),
      lrclibStrictUpstreamMs: stats(s.map((r) => r.split.lrclibStrictUpstreamMs).filter((x) => x !== null)),
      lrclibLooseMs: stats(s.map((r) => r.split.lrclibLooseMs).filter((x) => x !== null)),
      youtubeMs: stats(s.map((r) => r.split.youtubeMs).filter((x) => x !== null)),
      askedYouTube: s.filter((r) => r.split.askedYouTube).length,
      usedLoose: s.filter((r) => r.split.lrclibLooseQuery).length,
      slowestProviderMs: stats(s.map((r) => r.split.slowestProviderMs)),
      slowestIs: s.reduce((acc: any, r) => { acc[r.split.slowestProvider] = (acc[r.split.slowestProvider] ?? 0) + 1; return acc; }, {}),
      overheadMs: stats(s.map((r) => r.split.overheadMs)),
    };
  }
  summary.passes[pass] = p;
}

// Pooled over every pass of each kind.
const kinds: Record<string, string[]> = {};
for (const pass of Deno.args) {
  const kind = pass.replace(/\d+$/, "");
  (kinds[kind] ??= []).push(pass);
}
summary.pooled = {};
for (const [kind, passes] of Object.entries(kinds)) {
  const all = passes.flatMap((ps) => JSON.parse(Deno.readTextFileSync(`${here}results/${ps}.json`)))
    .filter((r: any) => r.sameTrack && !r.timedOut);
  const perTrack: Record<number, number[]> = {};
  for (const r of all) (perTrack[r.n] ??= []).push(r.ms);
  summary.pooled[kind] = {
    passes: passes.length, samples: all.length,
    timeToLyricsAll: stats(all.map((r: any) => r.ms)),
    timeToLyricsHits: stats(all.filter((r: any) => r.state === "synced" || r.state === "plain").map((r: any) => r.ms)),
    timeByProvider: Object.fromEntries([...new Set(all.map((r: any) => r.provider))].map((k) =>
      [k, stats(all.filter((r: any) => r.provider === k).map((r: any) => r.ms))])),
    repeatFromDbSameProcess: stats(all.map((r: any) => r.repeatFromDb?.ms).filter((x: any) => x !== undefined && x !== null)),
    extraLookupsWarmConnection: stats(all.flatMap((r: any) => r.extra.map((e: any) => e.ms)).filter((x: any) => x !== null)),
    perTrackMedian: Object.fromEntries(Object.entries(perTrack).map(([n, v]) => [n, quantile(v, 0.5)])),
    classStability: Object.fromEntries(list.map((t: any) => [t.n,
      [...new Set(all.filter((r: any) => r.n === t.n).map((r: any) => `${r.class}/${r.provider}`))].join(" ; ")])),
  };
  if (kind.startsWith("proxy")) {
    const s = all.filter((r: any) => r.split);
    summary.pooled[kind].split = {
      lrclibStrictMs: stats(s.map((r: any) => r.split.lrclibStrictMs).filter((x: any) => x !== null)),
      lrclibStrictUpstreamMs: stats(s.map((r: any) => r.split.lrclibStrictUpstreamMs).filter((x: any) => x !== null)),
      lrclibLooseMs: stats(s.map((r: any) => r.split.lrclibLooseMs).filter((x: any) => x !== null)),
      youtubeMs: stats(s.map((r: any) => r.split.youtubeMs).filter((x: any) => x !== null)),
      slowestProviderMs: stats(s.map((r: any) => r.split.slowestProviderMs)),
      overheadMs: stats(s.map((r: any) => r.split.overheadMs)),
    };
  }
}
Deno.writeTextFileSync(`${here}results/summary.json`, JSON.stringify(summary, null, 2));
console.log(JSON.stringify(summary, null, 2));
