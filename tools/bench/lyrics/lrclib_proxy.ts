// A logging pass-through for LRCLIB, so the app's own LRCLIB leg can be timed
// and its exact queries and answers seen, with no change to the app: run the
// app with `--set lrclib_url http://127.0.0.1:<port>`.
//
// Every request is forwarded to https://lrclib.net over a NEW connection
// (a fresh HttpClient per request), so the upstream time includes DNS + TCP +
// TLS like the app's first lookup in a process. Timestamps are epoch ms, to be
// lined up with the app's wall-clock log (QT_MESSAGE_PATTERN %{time ...}).
//
// Usage: deno run --allow-net --allow-write lrclib_proxy.ts <port> <log.jsonl> [--warm] [--fail]
//   --warm reuses one pooled client (connection kept alive between requests).
//   --fail answers every request at once with HTTP 503 and never contacts
//          LRCLIB: the app then asks YouTube Music straight away, so its
//          YouTube Music leg can be timed on its own.
const [portArg, logFile] = Deno.args;
const warm = Deno.args.includes("--warm");
const fail = Deno.args.includes("--fail");
const port = Number(portArg ?? 8765);
const upstream = "https://lrclib.net";
const shared = warm ? Deno.createHttpClient({}) : null;

function append(obj: unknown) {
  Deno.writeTextFileSync(logFile, JSON.stringify(obj) + "\n", { append: true });
}

Deno.serve({ hostname: "127.0.0.1", port, onListen: () => console.log(`proxy on ${port}`) }, async (req) => {
  const tIn = Date.now();
  const pIn = performance.now();
  const url = new URL(req.url);
  if (fail) {
    append({ tIn, tOut: Date.now(), url: url.pathname + url.search, query: Object.fromEntries(url.searchParams.entries()),
             status: 503, error: "stub", bytes: 0, ttfbMs: 0, upstreamMs: 0, results: 0, entries: [] });
    return new Response("stub: LRCLIB disabled for this measurement", { status: 503 });
  }
  const target = upstream + url.pathname + url.search;
  const client = shared ?? Deno.createHttpClient({});
  let status = 0, body = new Uint8Array(), error = "";
  let pHeaders = 0;
  try {
    const r = await fetch(target, {
      client,
      headers: { "User-Agent": req.headers.get("user-agent") ?? "Monolist", "Accept": "application/json" },
      signal: AbortSignal.timeout(30000),
    });
    pHeaders = performance.now();
    status = r.status;
    body = new Uint8Array(await r.arrayBuffer());
  } catch (e) {
    error = String(e);
  } finally {
    if (!shared) client.close();
  }
  const pDone = performance.now();
  let summary: unknown[] = [];
  try {
    const arr = JSON.parse(new TextDecoder().decode(body));
    if (Array.isArray(arr)) {
      summary = arr.map((e: any) => ({
        id: e.id, track: e.trackName, artist: e.artistName, album: e.albumName, duration: e.duration,
        instrumental: e.instrumental, synced: !!(e.syncedLyrics && String(e.syncedLyrics).trim()),
        plain: !!(e.plainLyrics && String(e.plainLyrics).trim()),
      }));
    }
  } catch (_) { /* not JSON */ }
  append({
    tIn, tOut: Date.now(), url: url.pathname + url.search,
    query: Object.fromEntries(url.searchParams.entries()),
    status, error, bytes: body.length,
    ttfbMs: pHeaders ? Math.round(pHeaders - pIn) : null,
    upstreamMs: Math.round(pDone - pIn),
    results: summary.length, entries: summary,
  });
  if (error) return new Response("proxy error: " + error, { status: 502 });
  return new Response(body, { status, headers: { "content-type": "application/json" } });
});
