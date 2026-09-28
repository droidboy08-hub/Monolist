// Builds bench/lyrics_tracks.json from candidates.json + search_results.json
// (the app's own --search output). The app's log goes through the ANSI code
// page, so non-Latin titles arrive as "????"; the Unicode title and channel
// are filled in from YouTube's oEmbed for the record only (the app itself
// looks up with the title its own search returns).
// Usage: deno run --allow-read --allow-write --allow-net build_list.ts
const here = new URL(".", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const candidates = JSON.parse(Deno.readTextFileSync(`${here}candidates.json`));
const searches = JSON.parse(Deno.readTextFileSync(`${here}search_results.json`));

// Where the first song result is not the intended recording.
const overrides: Record<number, { query: string; videoId: string; title: string; artist: string; album: string; duration: string; note: string }> = {
  25: {
    query: "Ue o Muite Arukou Kyu Sakamoto",
    videoId: "F284iB65-QU",
    title: "上を向いて歩こう - Sukiyaki", artist: "Kyu Sakamoto", album: "Best 9!", duration: "3:09",
    note: "Replaces Plastic Love (Mariya Takeuchi): the original is not in this region's YouTube Music search; only covers/remixes come back.",
  },
};

function toMs(clock: string): number {
  return clock.split(":").reduce((acc, part) => acc * 60 + Number(part), 0) * 1000;
}

const tracks = [];
for (const c of candidates) {
  const o = overrides[c.n];
  const s = searches.find((x: any) => x.n === c.n);
  const first = o ?? s.results[0];
  let youtubeTitle = "", channel = "";
  try {
    const r = await fetch(`https://www.youtube.com/oembed?format=json&url=${encodeURIComponent("https://www.youtube.com/watch?v=" + first.videoId)}`);
    if (r.ok) {
      const j = await r.json();
      youtubeTitle = j.title;
      channel = j.author_name;
    }
  } catch (_) { /* keep the search title */ }
  // The title YouTube Music's search gave the app (what the lyrics lookup
  // uses). Where the log mangled it, the video title is used when it is the
  // same length (checked by eye: 14, 16, 24).
  let unicodeTitle = first.title;
  if (unicodeTitle.includes("?") && youtubeTitle.length === unicodeTitle.length) unicodeTitle = youtubeTitle;
  tracks.push({
    n: c.n,
    language: c.language,
    genre: c.genre,
    era: c.era,
    query: o ? o.query : c.query,
    videoId: first.videoId,
    title: unicodeTitle,
    artist: first.artist,
    album: first.album,
    duration: first.duration,
    durationMs: toMs(first.duration),
    appLogTitle: o ? "???????? - Sukiyaki" : first.title,
    youtubeTitle,
    channel,
    expect: c.expect,
    ...(o ? { note: o.note } : {}),
  });
}
const out = `${here}..\\lyrics_tracks.json`;
Deno.writeTextFileSync(out, JSON.stringify({
  description: "Fixed lyrics test list (Step 2, lyrics area). Each entry: the query given to monolist.exe --lyrics / --search (songs filter), the videoId its FIRST song result must be, and that result's title/artist/duration as YouTube Music reported them. Built 2026-09-26 with the app's own --search.",
  tracks,
}, null, 2));
console.log(`wrote ${tracks.length} tracks to ${out}`);
for (const t of tracks) console.log(`${t.n}\t${t.videoId}\t${t.duration}\t${t.title}\t${t.artist}`);
