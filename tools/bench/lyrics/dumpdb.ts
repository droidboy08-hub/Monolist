// Dumps the lyrics table of a Monolist data dir as JSON (UTF-8 intact, unlike
// the app's qWarning output, which goes through the ANSI code page).
// Usage: deno run --allow-read --allow-write dumpdb.ts <dataDir> <out.json>
import { DatabaseSync } from "node:sqlite";

const [dataDir, outFile] = Deno.args;
const db = new DatabaseSync(`${dataDir}/monolist.db`, { readOnly: true });
const rows = db.prepare("SELECT video_id, synced, plain, source, fetched_at FROM lyrics").all();
db.close();
const out = rows.map((r: any) => {
  const synced = (r.synced ?? "") as string;
  const plain = (r.plain ?? "") as string;
  // enhanced-LRC word stamps <mm:ss.xx> or A2 extension
  const wordStamps = (synced.match(/<\d{1,3}:\d{1,2}(?:[.:]\d{1,3})?>/g) ?? []).length;
  const syncedLines = synced.split("\n").filter((l) => /^\[\d{1,3}:\d{1,2}/.test(l.trim()));
  const firstSynced = syncedLines.map((l) => l.replace(/^(\[[^\]]*\])+/, "").replace(/<[^>]*>/g, "").trim())
    .filter((t) => t.length > 0).slice(0, 3);
  const firstPlain = plain.split("\n").map((l) => l.trim()).filter((t) => t.length > 0).slice(0, 3);
  return {
    videoId: r.video_id,
    source: r.source,
    fetchedAt: r.fetched_at,
    syncedChars: synced.length,
    plainChars: plain.length,
    syncedLineCount: syncedLines.length,
    wordStamps,
    firstLines: firstSynced.length ? firstSynced : firstPlain,
  };
});
const text = JSON.stringify(out, null, 2);
if (outFile) Deno.writeTextFileSync(outFile, text);
else console.log(text);
