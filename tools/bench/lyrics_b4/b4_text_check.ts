// B4: is the text the app keeps the right song's, and what did LRCLIB send?
//
// For every track of the fixed list and every given pass, the first lines the
// app stored for the list's videoId (dumpdb's .db.json, UTF-8 intact) against
// Step 2's reference (bench\lyrics\results\lrclib_audit.json): /api/get's
// entry where it had one, else the search pick (#16, get 404). Equal to the
// get entry = "exact"; equal to Step 2's search pick (another transcription
// of the same song, judged right in Step 2 for every track but #24) =
// "search text"; anything else is printed for a look by hand.
// Bytes: the app's own log line for the list's videoId
//   "lyrics: LRCLIB <outcome> in N ms for <id>: <trail>; <bytes> bytes"
// (every byte LRCLIB sent for that lookup, requests given up on included).
// Usage: deno run --allow-read b4_text_check.ts <pass> [<pass> ...]
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const list = JSON.parse(Deno.readTextFileSync(`${here}../lyrics_tracks.json`)).tracks;
const audit: any[] = JSON.parse(Deno.readTextFileSync(`${here}../lyrics/results/lrclib_audit.json`));
const norm = (s: string) => s.normalize("NFC").replace(/\s+/g, " ").trim().toLowerCase();
const first2 = (lines: string[]) => norm(lines.slice(0, 2).join(" / ").slice(0, 90));

function q(values: number[], p: number) {
  const v = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!v.length) return null;
  const pos = (v.length - 1) * p, lo = Math.floor(pos), hi = Math.ceil(pos);
  return Math.round(v[lo] + (v[hi] - v[lo]) * (pos - lo));
}

const perTrack: Record<number, Record<string, number>> = {};
const bytes: number[] = [];
const exactCalls: number[] = [];
const hedged: number[] = [];
let lookups = 0;
const odd: string[] = [];
for (const pass of Deno.args) {
  for (const t of list) {
    const nn = String(t.n).padStart(2, "0");
    let db: any[] = [], log = "";
    try { db = JSON.parse(Deno.readTextFileSync(`${here}logs/${pass}/${nn}.db.json`)); } catch (_) { continue; }
    try { log = Deno.readTextFileSync(`${here}logs/${pass}/${nn}.err`); } catch (_) { /* none */ }
    const row = db.find((r) => r.videoId === t.videoId);
    const ref = audit.find((a) => a.n === t.n);
    let verdict = "not stored";
    if (row) {
      const got = first2(row.firstLines ?? []);
      const getRef = ref?.get?.entry ? norm(ref.get.entry.first) : null;
      const searchRef = ref?.search?.pick ? norm(ref.search.pick.first) : null;
      if (row.source === "instrumental") verdict = "instrumental";
      else if (!got) verdict = "none";
      else if (getRef !== null && got === getRef) verdict = "exact";
      else if (searchRef !== null && got === searchRef) verdict = t.n === 24 ? "WRONG (#24 search text)" : "search text";
      else { verdict = "other"; odd.push(`${pass} #${t.n}: ${row.firstLines.slice(0, 2).join(" / ")}`); }
    }
    (perTrack[t.n] ??= {})[verdict] = ((perTrack[t.n] ??= {})[verdict] ?? 0) + 1;
    // The log line for the list's videoId (the first lookup of the process).
    for (const line of log.split(/\r?\n/)) {
      const m = line.match(/lyrics: LRCLIB (\w+) in (\d+) ms for (\S+): (.*); (\d+) bytes$/);
      if (!m || m[3] !== t.videoId) continue;
      lookups++;
      bytes.push(Number(m[5]));
      const sent = m[4].match(/get (?:found|empty|404)? ?(\d+) ms after sent/) ?? m[4].match(/get \w+ (\d+) ms after sent/);
      if (sent) exactCalls.push(Number(sent[1]));
      if (m[4].includes("search asked too")) hedged.push(t.n);
      break;
    }
  }
}
console.log("Per track, over passes " + Deno.args.join(" "));
for (const t of list) console.log(`  #${String(t.n).padEnd(2)} ${JSON.stringify(perTrack[t.n] ?? {})}`);
const vocal = list.filter((t: any) => t.expect === "lyrics").map((t: any) => t.n);
const rightAlways = vocal.filter((n: number) => {
  const v = perTrack[n] ?? {};
  return Object.keys(v).length > 0 && Object.keys(v).every((k) => k === "exact" || k === "search text");
});
console.log(`Vocal tracks whose stored text was the right song's in every pass: ${rightAlways.length}/${vocal.length}`);
console.log(`Vocal tracks with the exact (/api/get) text in every pass: ${vocal.filter((n: number) => Object.keys(perTrack[n] ?? {}).join() === "exact").length}/${vocal.length}`);
console.log(`LRCLIB bytes per first lookup: n=${bytes.length} median=${q(bytes, 0.5)} p90=${q(bytes, 0.9)} min=${Math.min(...bytes)} max=${Math.max(...bytes)}`);
console.log(`/api/get answer after sent: n=${exactCalls.length} median=${q(exactCalls, 0.5)} p90=${q(exactCalls, 0.9)}`);
console.log(`Search asked as well because /api/get was slow: ${hedged.length} of ${lookups} first lookups`);
if (odd.length) console.log("Other text, to look at:\n  " + odd.join("\n  "));
