const PROXY = 'https://api.codetabs.com/v1/proxy?quest=';

// Piped API has Access-Control-Allow-Origin: * — no proxy needed in any context.
// Returns direct YouTube CDN audio URLs, safe for <audio> element.
// NOT safe to fetch() bytes from browser (YouTube CDN blocks cross-origin fetch).
const PIPED_INSTANCES = [
  'pipedapi.kavin.rocks',
  'pipedapi.moomoo.me',
  'piped-api.garudalinux.org',
  'api.piped.projectsegfau.lt',
  'piped.privacydev.net',
];

// Invidious instances that support ?local=true (proxied streams)
const INVIDIOUS_INSTANCES = [
  'yewtu.be',
  'invidious.projectsegfau.lt',
  'iv.ggtyler.dev',
  'inv.nadeko.net',
  'invidious.nerdvpn.de',
  'invidious.privacydev.net',
  'yt.artemislena.eu',
  'invidious.fdn.fr',
  'invidious.slipfox.xyz',
  'invidious.lunar.icu',
  'invidious.vps.sh',
  'inv.tux.pizza',
  'invidious.io.lol',
];

// Electron has webSecurity: false — all cross-origin requests work without a proxy.
export const isElectron = (): boolean =>
  typeof navigator !== 'undefined' &&
  navigator.userAgent.toLowerCase().includes('electron');

const proxyWrap = (url: string): string =>
  isElectron() ? url : `${PROXY}${encodeURIComponent(url)}`;

// Race all Piped instances; first valid JSON with audio streams wins.
const tryPiped = (videoId: string): Promise<string> =>
  Promise.any(
    PIPED_INSTANCES.map(async (host) => {
      const res = await fetch(`https://${host}/streams/${videoId}`, {
        signal: AbortSignal.timeout(8000),
      });
      if (!res.ok) throw new Error('bad');
      const data = await res.json();
      const streams: { url: string; mimeType?: string; bitrate?: number }[] =
        data.audioStreams ?? [];
      const best =
        streams
          .filter((s) => s.mimeType?.includes('mp4'))
          .sort((a, b) => (b.bitrate ?? 0) - (a.bitrate ?? 0))[0] ?? streams[0];
      if (!best?.url) throw new Error('no url');
      return best.url;
    })
  );

// Race all Invidious instances; first one that answers HEAD wins.
// In Electron: direct URL. In browser: wrapped through codetabs proxy.
const tryInvidious = (videoId: string): Promise<string> =>
  Promise.any(
    INVIDIOUS_INSTANCES.map(async (host) => {
      const raw = `https://${host}/latest_version?id=${videoId}&itag=140&local=true`;
      const url = proxyWrap(raw);
      const res = await fetch(url, { method: 'HEAD', signal: AbortSignal.timeout(6000) });
      if (!res.ok) throw new Error('bad');
      return url;
    })
  );

// For playback: race Piped and Invidious.
// In Electron: Try Invidious first (reliable proxy). Fallback to Piped if Invidious fails.
export const resolveStreamUrl = async (videoId: string): Promise<string | null> => {
  try {
    if (isElectron()) {
      try {
        return await tryInvidious(videoId);
      } catch {
        return await tryPiped(videoId); // Last resort in Electron
      }
    }
    return await Promise.any([tryPiped(videoId), tryInvidious(videoId)]);
  } catch {
    return null;
  }
};

// For downloading bytes: Invidious is preferred because the URL is server-proxied
// (fetchable from browser without CORS issues). In Electron, also try Piped as fallback
// since webSecurity: false removes the YouTube CDN CORS restriction.
export const resolveDownloadUrl = async (videoId: string): Promise<string | null> => {
  try {
    return await tryInvidious(videoId);
  } catch {
    if (isElectron()) {
      try {
        return await tryPiped(videoId);
      } catch {}
    }
    return null;
  }
};
