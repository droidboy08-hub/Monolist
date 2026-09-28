// The lyrics table of a data dir: its columns and rows (lengths only).
// Usage: deno run --allow-read lyrics_table.ts <dataDir>
import { DatabaseSync } from "node:sqlite";
const db = new DatabaseSync(`${Deno.args[0]}/monolist.db`, { readOnly: true });
console.log("columns: " + JSON.stringify(db.prepare("PRAGMA table_info(lyrics)").all().map((r: any) => r.name)));
const hasProvisional = db.prepare("PRAGMA table_info(lyrics)").all().some((r: any) => r.name === "provisional");
const rows = db.prepare(`SELECT video_id, source, length(synced) AS s, length(plain) AS p, fetched_at${hasProvisional ? ", provisional" : ""} FROM lyrics`).all();
for (const r of rows) console.log(JSON.stringify(r));
db.close();
