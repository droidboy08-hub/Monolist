import { resolveDownloadUrl, isElectron } from './streamService';

// Electron IPC helper
const getIpc = () => (isElectron() ? (window as any).require('electron').ipcRenderer : null);

// Metadata only — no audio blob in memory (loaded on demand via getAudioBase64)
export interface DownloadedSong {
  id: string;
  title: string;
  artist: string;
  thumbnail: string;
  duration: string;
}

// Full record stored in IndexedDB (audio included for non-electron)
interface StoredSong extends DownloadedSong {
  audioBase64?: string; // Optional for Electron (stored on disk)
}

const DB_NAME = 'MelodyOfflineDB';
const DB_VERSION = 2;
const STORE_NAME = 'songs';

const getDB = (): Promise<IDBDatabase> =>
  new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = () => {
      const db = request.result;
      if (db.objectStoreNames.contains(STORE_NAME)) {
        db.deleteObjectStore(STORE_NAME);
      }
      db.createObjectStore(STORE_NAME, { keyPath: 'id' });
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });

export const getAllDownloadedSongs = async (): Promise<DownloadedSong[]> => {
  const db = await getDB();
  return new Promise((resolve, reject) => {
    const transaction = db.transaction(STORE_NAME, 'readonly');
    const request = transaction.objectStore(STORE_NAME).getAll();
    request.onsuccess = () => {
      const records: StoredSong[] = (request as IDBRequest).result;
      resolve(records.map(({ audioBase64: _audio, ...meta }) => meta));
    };
    request.onerror = () => reject(request.error);
  });
};

export const getAudioBase64 = async (id: string): Promise<string | null> => {
  if (isElectron()) {
    return await getIpc().invoke('read-song', id);
  }

  const db = await getDB();
  return new Promise((resolve, reject) => {
    const transaction = db.transaction(STORE_NAME, 'readonly');
    const request = transaction.objectStore(STORE_NAME).get(id);
    request.onsuccess = () => resolve((request as IDBRequest).result?.audioBase64 ?? null);
    request.onerror = () => reject(request.error);
  });
};

export const downloadAndSaveSong = async (
  id: string,
  title: string,
  artist: string,
  thumbnailUrl: string,
  duration: string,
  onProgress?: (progress: number) => void
): Promise<DownloadedSong> => {
  const streamUrl = await resolveDownloadUrl(id);
  if (!streamUrl) throw new Error('All download gateways failed.');

  const response = await fetch(streamUrl);
  if (!response.ok) throw new Error('Download fetch failed.');

  const reader = response.body?.getReader();
  if (!reader) throw new Error('No response body reader.');

  const contentLength = +(response.headers.get('Content-Length') || '0');
  let receivedLength = 0;
  const chunks: Uint8Array<ArrayBuffer>[] = [];

  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    receivedLength += value.length;
    if (contentLength && onProgress) onProgress((receivedLength / contentLength) * 100);
  }

  const audioBase64 = await new Promise<string>((resolve) => {
    const r = new FileReader();
    r.onloadend = () => resolve((r.result as string).split(',')[1]);
    r.readAsDataURL(new Blob(chunks));
  });

  const stored: StoredSong = { id, title, artist, duration, thumbnail: thumbnailUrl };

  if (isElectron()) {
    await getIpc().invoke('save-song', { id, base64: audioBase64 });
  } else {
    stored.audioBase64 = audioBase64;
  }

  const db = await getDB();
  await new Promise<void>((resolve, reject) => {
    const transaction = db.transaction(STORE_NAME, 'readwrite');
    const req = transaction.objectStore(STORE_NAME).put(stored);
    req.onsuccess = () => resolve();
    req.onerror = () => reject(req.error);
  });

  return stored;
};

export const deleteDownloadedSong = async (id: string): Promise<void> => {
  if (isElectron()) {
    await getIpc().invoke('delete-song', id);
  }
  const db = await getDB();
  const transaction = db.transaction(STORE_NAME, 'readwrite');
  transaction.objectStore(STORE_NAME).delete(id);
};
