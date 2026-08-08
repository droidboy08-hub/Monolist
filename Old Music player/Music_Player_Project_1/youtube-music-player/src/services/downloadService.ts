import { resolveDownloadUrl, isElectron } from './streamService';

// Safe IPC accessor — never throws even if require isn't available
const getIpc = (): any | null => {
  try {
    if (!isElectron()) return null;
    return (window as any).require('electron').ipcRenderer ?? null;
  } catch {
    return null;
  }
};

export interface DownloadedSong {
  id: string;
  title: string;
  artist: string;
  thumbnail: string;
  duration: string;
}

interface StoredSong extends DownloadedSong {
  audioBase64?: string;
}

const DB_NAME    = 'MelodyOfflineDB';
const DB_VERSION = 2;
const STORE_NAME = 'songs';

const getDB = (): Promise<IDBDatabase> =>
  new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (db.objectStoreNames.contains(STORE_NAME)) db.deleteObjectStore(STORE_NAME);
      db.createObjectStore(STORE_NAME, { keyPath: 'id' });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror   = () => reject(req.error);
  });

export const getAllDownloadedSongs = async (): Promise<DownloadedSong[]> => {
  const db = await getDB();
  return new Promise((resolve, reject) => {
    const tx  = db.transaction(STORE_NAME, 'readonly');
    const req = tx.objectStore(STORE_NAME).getAll();
    req.onsuccess = () => {
      const records: StoredSong[] = (req as IDBRequest).result;
      resolve(records.map(({ audioBase64: _a, ...meta }) => meta));
    };
    req.onerror = () => reject(req.error);
  });
};

export const getAudioBase64 = async (id: string): Promise<string | null> => {
  const ipc = getIpc();
  if (ipc) return await ipc.invoke('read-song', id);

  const db = await getDB();
  return new Promise((resolve, reject) => {
    const tx  = db.transaction(STORE_NAME, 'readonly');
    const req = tx.objectStore(STORE_NAME).get(id);
    req.onsuccess = () => resolve((req as IDBRequest).result?.audioBase64 ?? null);
    req.onerror   = () => reject(req.error);
  });
};

// Convert raw chunks to base64.
// In Electron: use Node.js Buffer (fast, no DOM dependency).
// In browser:  fall back to FileReader.
const chunksToBase64 = (chunks: Uint8Array[]): Promise<string> => {
  const ipc = getIpc();
  if (ipc) {
    // Electron path — Node.js Buffer
    try {
      const NodeBuffer: any = (window as any).require('buffer').Buffer;
      const total  = chunks.reduce((n, c) => n + c.length, 0);
      const merged = NodeBuffer.alloc(total);
      let offset = 0;
      for (const chunk of chunks) {
        NodeBuffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength).copy(merged, offset);
        offset += chunk.byteLength;
      }
      return Promise.resolve(merged.toString('base64'));
    } catch {
      // fall through to FileReader if Buffer unexpectedly unavailable
    }
  }

  // Browser / fallback path
  return new Promise<string>((resolve, reject) => {
    const blob = new Blob(chunks as unknown as BlobPart[], { type: 'audio/mp4' });
    const r = new FileReader();
    r.onloadend = () => {
      const result = r.result as string;
      const b64 = result.split(',')[1];
      b64 ? resolve(b64) : reject(new Error('FileReader produced empty base64'));
    };
    r.onerror = () => reject(r.error ?? new Error('FileReader failed'));
    r.readAsDataURL(blob);
  });
};

export const downloadAndSaveSong = async (
  id: string,
  title: string,
  artist: string,
  thumbnailUrl: string,
  duration: string,
  onProgress?: (pct: number) => void,
): Promise<DownloadedSong> => {
  const streamUrl = await resolveDownloadUrl(id);
  if (!streamUrl) throw new Error('All download sources failed — no URL available.');

  // 90-second timeout for the actual download fetch
  const response = await fetch(streamUrl, { signal: AbortSignal.timeout(90_000) });
  if (!response.ok) throw new Error(`Server returned ${response.status} for download URL.`);

  const bodyReader = response.body?.getReader();
  if (!bodyReader) throw new Error('Response has no readable body.');

  const contentLength = +(response.headers.get('Content-Length') ?? '0');
  let received = 0;
  const chunks: Uint8Array[] = [];

  while (true) {
    const { done, value } = await bodyReader.read();
    if (done) break;
    if (!value) continue;
    chunks.push(value);
    received += value.byteLength;

    if (onProgress) {
      // Show real progress when Content-Length is known, otherwise fake a
      // slowly-filling bar that caps at 95% until the stream closes.
      const pct = contentLength > 0
        ? (received / contentLength) * 100
        : Math.min(95, (received / 8_000_000) * 95); // assume ~8 MB avg song
      onProgress(pct);
    }
  }

  // Signal 100% before the (potentially slow) encoding step
  onProgress?.(100);

  const audioBase64 = await chunksToBase64(chunks);

  const stored: StoredSong = { id, title, artist, duration, thumbnail: thumbnailUrl };
  const ipc = getIpc();
  if (ipc) {
    await ipc.invoke('save-song', { id, base64: audioBase64 });
  } else {
    stored.audioBase64 = audioBase64;
  }

  const db = await getDB();
  await new Promise<void>((resolve, reject) => {
    const tx  = db.transaction(STORE_NAME, 'readwrite');
    const req = tx.objectStore(STORE_NAME).put(stored);
    req.onsuccess = () => resolve();
    req.onerror   = () => reject(req.error);
  });

  return stored;
};

export const deleteDownloadedSong = async (id: string): Promise<void> => {
  const ipc = getIpc();
  if (ipc) await ipc.invoke('delete-song', id);

  const db = await getDB();
  const tx = db.transaction(STORE_NAME, 'readwrite');
  tx.objectStore(STORE_NAME).delete(id);
};
