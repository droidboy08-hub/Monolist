import React, { useState, useRef, useEffect, useMemo } from 'react';
import {
  downloadAndSaveSong,
  getAllDownloadedSongs,
  deleteDownloadedSong as deleteStoredSong,
  getAudioBase64,
} from './services/downloadService';
import type { DownloadedSong } from './services/downloadService';
import { resolveStreamUrl, isElectron } from './services/streamService';

// IPC accessor for OS window controls (Electron only)
const winIpc = (() => {
  try { return isElectron() ? (window as any).require('electron').ipcRenderer : null; }
  catch { return null; }
})();

// ─── Types ────────────────────────────────────────────────────────────────────

interface Song {
  id: string;
  title: string;
  artist: string;
  thumbnail: string;
  duration: string;
}

type ViewType   = 'library' | 'search' | 'playlists' | 'downloads';
type Protocol   = 'NONE' | 'STEALTH' | 'HYBRID' | 'LOCAL';
type RepeatMode = 0 | 1 | 2;

declare global {
  interface Window { onYouTubeIframeAPIReady: () => void; YT: any; }
}

// ─── Cover Art ────────────────────────────────────────────────────────────────

const PALETTES: [string, string, string][] = [
  ['#FF8FB1','#FF5C8A','#3D1C5E'],
  ['#F5C16C','#E07A3B','#3B1D2A'],
  ['#7CE4D6','#3FA9F5','#0B2B5C'],
  ['#C3A0FF','#7B5BFF','#1E0C44'],
  ['#9CE89C','#3FBF87','#0E2A2F'],
  ['#FFD86E','#FF7A3B','#3A0F2C'],
  ['#7AD7FF','#3F6BFF','#11144F'],
  ['#FF9DCB','#A75BFF','#2A0E4F'],
  ['#FFEAA8','#FF6B6B','#2C1438'],
  ['#9DFFE0','#3FBFD8','#0A2E3F'],
  ['#FFCFA3','#E08AAE','#2B1546'],
  ['#B6B0FF','#5C7BFF','#0D1B4A'],
];

interface CoverProps { seed?: string; size?: number; radius?: number }

const Cover: React.FC<CoverProps> = ({ seed = 'a', size = 56, radius = 10 }) => {
  const i = (seed.charCodeAt(0) + seed.length * 7) % PALETTES.length;
  const [a, b, c] = PALETTES[i];
  const angle = (seed.charCodeAt(seed.length - 1) % 12) * 30;
  const blob  = (seed.charCodeAt(1) || 0) % 3;
  return (
    <div className="cover" style={{ width: size, height: size, borderRadius: radius, background: c }}>
      <div className="cover-grad" style={{ background: `linear-gradient(${angle}deg,${a} 0%,${b} 60%,${c} 100%)` }}/>
      <svg viewBox="0 0 100 100" className="cover-svg" preserveAspectRatio="xMidYMid slice">
        {blob === 0 && <circle cx="70" cy="30" r="32" fill={a} opacity={0.55}/>}
        {blob === 1 && <rect x="-10" y="55" width="120" height="60" fill={a} opacity={0.45} transform="rotate(-12 50 75)"/>}
        {blob === 2 && <>
          <circle cx="20" cy="80" r="26" fill={a} opacity={0.55}/>
          <circle cx="78" cy="22" r="14" fill={b} opacity={0.7}/>
        </>}
      </svg>
      <div className="cover-gloss"/>
    </div>
  );
};

interface SongCoverProps extends CoverProps { thumbnail?: string }

const SongCover: React.FC<SongCoverProps> = ({ thumbnail, seed = 'a', size = 56, radius = 10 }) => {
  if (thumbnail) {
    return (
      <div className="cover" style={{ width: size, height: size, borderRadius: radius, overflow: 'hidden', background: '#111' }}>
        <img src={thumbnail} alt="" style={{ width: '100%', height: '100%', objectFit: 'cover', display: 'block' }}/>
        <div className="cover-gloss"/>
      </div>
    );
  }
  return <Cover seed={seed} size={size} radius={radius}/>;
};

// ─── Icons ────────────────────────────────────────────────────────────────────

interface IconProps extends Omit<React.SVGAttributes<SVGSVGElement>, 'stroke'> {
  name: string; size?: number; stroke?: number;
}

const Icon: React.FC<IconProps> = ({ name, size = 18, stroke = 1.6, ...rest }) => {
  const p: Record<string, React.ReactNode> = {
    library:    <><path d="M4 5h2v14H4z"/><path d="M9 5h2v14H9z"/><path d="m15 6 5 13-2 .8L13 7z"/></>,
    search:     <><circle cx="11" cy="11" r="6.5"/><path d="m20 20-4.2-4.2"/></>,
    playlist:   <><path d="M4 7h12"/><path d="M4 12h12"/><path d="M4 17h8"/><circle cx="18" cy="17" r="2.5"/><path d="M20.5 17V9l-3 1"/></>,
    download:   <><path d="M12 4v11"/><path d="m7.5 10.5 4.5 4.5 4.5-4.5"/><path d="M5 19h14"/></>,
    downloaded: <><circle cx="12" cy="12" r="8.5"/><path d="m8.5 12.2 2.6 2.6 4.4-5"/></>,
    play:       <><path d="M7 5.5v13l11-6.5z" fill="currentColor"/></>,
    pause:      <><rect x="7" y="5.5" width="3.2" height="13" rx="1" fill="currentColor" stroke="none"/><rect x="13.8" y="5.5" width="3.2" height="13" rx="1" fill="currentColor" stroke="none"/></>,
    pause2:     <><rect x="6" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none"/><rect x="14" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none"/></>,
    prev:       <><path d="M7 6v12"/><path d="M19 6 9 12l10 6z" fill="currentColor"/></>,
    next:       <><path d="M17 6v12"/><path d="M5 6l10 6L5 18z" fill="currentColor"/></>,
    shuffle:    <><path d="M4 7h3l9 10h4"/><path d="m18 19 3-2-3-2"/><path d="M4 17h3l3-3.3"/><path d="m14 10.3 2-2.3h4"/><path d="m18 5 3 2-3 2"/></>,
    repeat:     <><path d="M5 9V8a2 2 0 0 1 2-2h11"/><path d="m15 3 3 3-3 3"/><path d="M19 15v1a2 2 0 0 1-2 2H6"/><path d="m9 21-3-3 3-3"/></>,
    repeat1:    <><path d="M5 9V8a2 2 0 0 1 2-2h11"/><path d="m15 3 3 3-3 3"/><path d="M19 15v1a2 2 0 0 1-2 2H6"/><path d="m9 21-3-3 3-3"/><path d="M12 10v4h1.5"/></>,
    heart:      <><path d="M12 20s-7-4.3-7-10.2A4 4 0 0 1 12 6.5 4 4 0 0 1 19 9.8c0 5.9-7 10.2-7 10.2z"/></>,
    heartF:     <><path d="M12 20s-7-4.3-7-10.2A4 4 0 0 1 12 6.5 4 4 0 0 1 19 9.8c0 5.9-7 10.2-7 10.2z" fill="currentColor"/></>,
    volume:     <><path d="M4 9.5v5h3.5L13 19V5L7.5 9.5z" fill="currentColor" stroke="none"/><path d="M16 9a4 4 0 0 1 0 6"/><path d="M19 6.5a8 8 0 0 1 0 11"/></>,
    queue:      <><path d="M4 6h11"/><path d="M4 12h11"/><path d="M4 18h7"/><path d="m18 13 4 2.5L18 18z" fill="currentColor"/></>,
    dots:       <><circle cx="5" cy="12" r="1.2" fill="currentColor"/><circle cx="12" cy="12" r="1.2" fill="currentColor"/><circle cx="19" cy="12" r="1.2" fill="currentColor"/></>,
    plus:       <><path d="M12 5v14"/><path d="M5 12h14"/></>,
    sliders:    <><path d="M4 6h10"/><path d="M18 6h2"/><circle cx="16" cy="6" r="2"/><path d="M4 12h2"/><path d="M10 12h10"/><circle cx="8" cy="12" r="2"/><path d="M4 18h12"/><path d="M20 18h0"/><circle cx="18" cy="18" r="2"/></>,
    sparkle:    <><path d="M12 4v5"/><path d="M12 15v5"/><path d="M4 12h5"/><path d="M15 12h5"/><path d="m6.5 6.5 3 3"/><path d="m14.5 14.5 3 3"/><path d="m17.5 6.5-3 3"/><path d="m9.5 14.5-3 3"/></>,
    chevron:    <><path d="m9 6 6 6-6 6"/></>,
    pin:        <><path d="M12 17v5"/><path d="M8 6h8l-1 6 3 3H6l3-3z"/></>,
    wifi:       <><path d="M2 9a16 16 0 0 1 20 0"/><path d="M5 12.5a11 11 0 0 1 14 0"/><path d="M8.5 16a6 6 0 0 1 7 0"/><circle cx="12" cy="19.5" r=".8" fill="currentColor"/></>,
    cloud:      <><path d="M7 17h10a4 4 0 0 0 .5-7.95 6 6 0 0 0-11.7 1.45A3.5 3.5 0 0 0 7 17z"/></>,
    check:      <><path d="m5 12.5 4.5 4.5L19 7"/></>,
    x:          <><path d="m6 6 12 12"/><path d="m18 6-12 12"/></>,
    grid:       <><rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="1.5"/></>,
    list:       <><path d="M4 7h16"/><path d="M4 12h16"/><path d="M4 17h16"/></>,
    eq:         <><path d="M5 19v-7"/><path d="M5 9V5"/><path d="M12 19v-4"/><path d="M12 12V5"/><path d="M19 19v-9"/><path d="M19 7V5"/><circle cx="5" cy="10.5" r="1.5" fill="currentColor"/><circle cx="12" cy="13.5" r="1.5" fill="currentColor"/><circle cx="19" cy="8.5" r="1.5" fill="currentColor"/></>,
    trash:      <><path d="M3 6h18"/><path d="M8 6V4h8v2"/><path d="M19 6l-1 14H6L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/></>,
    arrow:      <><path d="M5 12h14"/><path d="m13 6 6 6-6 6"/></>,
  };
  return (
    <svg
      width={size} height={size} viewBox="0 0 24 24"
      fill="none" stroke="currentColor"
      strokeWidth={stroke} strokeLinecap="round" strokeLinejoin="round"
      {...rest}
    >
      {p[name] ?? null}
    </svg>
  );
};

// ─── Window Chrome ────────────────────────────────────────────────────────────

const WindowChrome: React.FC<{ view: ViewType; onNav: (v: ViewType) => void; onOpenSettings: () => void }> = ({ view, onNav, onOpenSettings }) => {
  const tabs: { id: ViewType; label: string; icon: string }[] = [
    { id: 'library',   label: 'Library',   icon: 'library'  },
    { id: 'search',    label: 'Search',    icon: 'search'   },
    { id: 'playlists', label: 'Playlists', icon: 'playlist' },
    { id: 'downloads', label: 'Downloads', icon: 'download' },
  ];
  return (
    <div className="chrome chrome-win">
      <div className="chrome-l">
        <div className="chrome-nav">
          <button className="chrome-arrow">
            <Icon name="chevron" size={14} stroke={2} style={{ transform: 'rotate(180deg)' }}/>
          </button>
          <button className="chrome-arrow"><Icon name="chevron" size={14} stroke={2}/></button>
        </div>
      </div>

      <div className="chrome-tabs">
        {tabs.map(t => (
          <button key={t.id} className={`chrome-tab ${view === t.id ? 'active' : ''}`} onClick={() => onNav(t.id)}>
            <Icon name={t.icon} size={14} stroke={1.8}/>
            <span>{t.label}</span>
          </button>
        ))}
      </div>

      <div className="chrome-r">
        <button className="chrome-arrow" title="Settings" onClick={onOpenSettings}><Icon name="sliders" size={14} stroke={1.8}/></button>
        {winIpc && (
          <div className="win-controls">
            <button className="w-min"   onClick={() => winIpc.send('window-minimize')} title="Minimize">—</button>
            <button className="w-max"   onClick={() => winIpc.send('window-maximize')} title="Maximize">▢</button>
            <button className="w-close" onClick={() => winIpc.send('window-close')}    title="Close">✕</button>
          </div>
        )}
      </div>
    </div>
  );
};

// ─── Sidebar ──────────────────────────────────────────────────────────────────

const PLAYLISTS_MOCK = [
  { id: 'p1', name: 'Late Night Drive', count: 38, hue: 'p1' },
  { id: 'p2', name: 'Soft Focus',       count: 64, hue: 'p2' },
  { id: 'p3', name: 'Sunday Coffee',    count: 22, hue: 'p3' },
];

interface SidebarProps {
  view: ViewType;
  onNav: (v: ViewType) => void;
  downloadedCount: number;
  queueCount: number;
  online: boolean;
}

const Sidebar: React.FC<SidebarProps> = ({ view, onNav, downloadedCount, queueCount, online }) => {
  const items: { id: ViewType; label: string; icon: string; count?: number; badge?: string }[] = [
    { id: 'library',   label: 'Library',   icon: 'library',  count: downloadedCount },
    { id: 'search',    label: 'Search',    icon: 'search' },
    { id: 'playlists', label: 'Playlists', icon: 'playlist' },
    { id: 'downloads', label: 'Downloads', icon: 'download',
      count: downloadedCount, badge: queueCount > 0 ? String(queueCount) : undefined },
  ];
  return (
    <aside className="sidebar glass">
      <div className="side-section">
        <div className="side-title">Browse</div>
        {items.map(it => (
          <button key={it.id} className={`side-item ${view === it.id ? 'active' : ''}`} onClick={() => onNav(it.id)}>
            <Icon name={it.icon} size={16}/>
            <span className="side-label">{it.label}</span>
            {it.badge && <span className="side-badge">{it.badge}</span>}
            {it.count != null && <span className="side-count">{it.count}</span>}
          </button>
        ))}
      </div>

      <div className="side-section">
        <div className="side-title">
          <span>Playlists</span>
          <button className="side-add" title="New playlist"><Icon name="plus" size={12} stroke={2.4}/></button>
        </div>
        {PLAYLISTS_MOCK.map(p => (
          <button key={p.id} className="side-item side-playlist" onClick={() => onNav('playlists')}>
            <Cover seed={p.hue} size={20} radius={5}/>
            <span className="side-label">{p.name}</span>
            <span className="side-count">{p.count}</span>
          </button>
        ))}
      </div>

      <div className="side-footer">
        <div className={`net-pill ${online ? 'ok' : 'off'}`}>
          <Icon name={online ? 'wifi' : 'cloud'} size={12} stroke={2}/>
          <span>{online ? 'Streaming · 320 kbps' : 'Offline · Library only'}</span>
        </div>
        <div className="storage">
          <div className="storage-label">
            <span>Offline library</span>
            <span>{downloadedCount} songs</span>
          </div>
          <div className="storage-bar">
            <div className="storage-fill" style={{ width: `${Math.min(100, (downloadedCount / 50) * 100)}%` }}/>
          </div>
        </div>
      </div>
    </aside>
  );
};

// ─── Track Table ──────────────────────────────────────────────────────────────

interface TrackRowSong { id: string; title: string; artist: string; thumbnail: string; duration: string }

interface TrackTableProps {
  tracks: TrackRowSong[];
  currentSongId?: string;
  isPlaying: boolean;
  downloadedIds: Set<string>;
  downloadProgress: Record<string, number>;
  onPlay: (song: TrackRowSong) => void;
  onDownload: (song: TrackRowSong) => void;
  onDelete?: (id: string) => void;
  showDelete?: boolean;
}

const EqBars = () => (
  <span className="bars" aria-label="playing"><span/><span/><span/></span>
);

const TrackTable: React.FC<TrackTableProps> = ({
  tracks, currentSongId, isPlaying, downloadedIds, downloadProgress,
  onPlay, onDownload, onDelete, showDelete = false,
}) => (
  <div className="track-table glass-inner">
    <div className="tt-head">
      <div className="tt-i">#</div>
      <div>Title</div>
      <div className="tt-d"><Icon name="download" size={14}/></div>
      <div className="tt-dur">Time</div>
      <div/>
    </div>
    {tracks.map((t, i) => {
      const isCur = t.id === currentSongId;
      const isDl  = downloadedIds.has(t.id);
      const prog  = downloadProgress[t.id];
      return (
        <div key={t.id} className={`tt-row ${isCur ? 'current' : ''}`} onDoubleClick={() => onPlay(t)}>
          <div className="tt-i">
            {isCur && isPlaying
              ? <EqBars/>
              : <>
                  <span className="num">{i + 1}</span>
                  <button className="play-mini" onClick={() => onPlay(t)}>
                    <Icon name={isCur ? 'pause' : 'play'} size={12}/>
                  </button>
                </>}
          </div>
          <div className="tt-t">
            <SongCover thumbnail={t.thumbnail} seed={t.title[0] + t.id} size={34} radius={6}/>
            <div className="tt-meta">
              <div className="tt-title">{t.title}</div>
              <div className="tt-artist">{t.artist}</div>
            </div>
          </div>
          <div className="tt-d">
            {prog != null
              ? <span style={{ fontSize: 10, fontWeight: 700, color: 'var(--accent)' }}>{Math.round(prog)}%</span>
              : <button
                  className={`dl-mini ${isDl ? 'done' : ''}`}
                  onClick={() => showDelete && isDl && onDelete ? onDelete(t.id) : onDownload(t)}
                  title={isDl ? (showDelete ? 'Remove offline copy' : 'Saved offline') : 'Save for offline'}>
                  <Icon name={isDl ? 'downloaded' : 'download'} size={14}/>
                </button>}
          </div>
          <div className="tt-dur">{t.duration}</div>
          <div>
            <button className="dots-btn"><Icon name="dots" size={14}/></button>
          </div>
        </div>
      );
    })}
  </div>
);

// ─── Now Playing Bar ──────────────────────────────────────────────────────────

const fmtTime = (s: number) =>
  `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;

interface NowPlayingBarProps {
  song: Song | null;
  isPlaying: boolean;
  progress: number;       // 0–1
  volume: number;         // 0–1
  currentPosSec: number;
  durationSec: number;
  isDownloaded: boolean;
  dlProgress?: number;
  status: string;
  repeatMode: RepeatMode;
  onPlayPause: () => void;
  onPrev: () => void;
  onNext: () => void;
  onSeek: (p: number) => void;
  onVolume: (v: number) => void;
  onDownload: () => void;
  onRepeat: () => void;
}

const NowPlayingBar: React.FC<NowPlayingBarProps> = ({
  song, isPlaying, progress, volume, currentPosSec, durationSec,
  isDownloaded, dlProgress, status, repeatMode,
  onPlayPause, onPrev, onNext, onSeek, onVolume, onDownload, onRepeat,
}) => {
  const barRef  = useRef<HTMLDivElement>(null);
  const volRef  = useRef<HTMLDivElement>(null);

  const handleBarClick = (e: React.MouseEvent<HTMLDivElement>) => {
    const r = e.currentTarget.getBoundingClientRect();
    onSeek(Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)));
  };
  const handleVolClick = (e: React.MouseEvent<HTMLDivElement>) => {
    const r = e.currentTarget.getBoundingClientRect();
    onVolume(Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)));
  };

  return (
    <footer className="np glass-strong">
      {status && <div className="np-status">{status}</div>}

      <div className="np-left">
        {song ? (
          <>
            <SongCover thumbnail={song.thumbnail} seed={song.title[0] + song.id} size={52} radius={10}/>
            <div className="np-meta">
              <div className="np-title">{song.title}</div>
              <div className="np-artist">{song.artist}</div>
            </div>
            <button className="np-heart"><Icon name="heart" size={16}/></button>
            <button
              className={`np-dl ${isDownloaded ? 'done' : ''}`}
              onClick={onDownload}>
              {dlProgress != null
                ? <span style={{ fontSize: 11, fontWeight: 700 }}>{Math.round(dlProgress)}%</span>
                : <>
                    <Icon name={isDownloaded ? 'downloaded' : 'download'} size={16} stroke={1.9}/>
                    <span>{isDownloaded ? 'Saved' : 'Save'}</span>
                  </>}
            </button>
          </>
        ) : (
          <div className="np-meta" style={{ paddingLeft: 4, color: 'var(--fg-mute)', fontSize: 13 }}>
            Nothing playing
          </div>
        )}
      </div>

      <div className="np-center">
        <div className="np-controls">
          <button className="np-ctl"><Icon name="shuffle" size={16} stroke={1.8}/></button>
          <button className="np-ctl" onClick={onPrev}><Icon name="prev" size={18}/></button>
          <button className="np-play" onClick={onPlayPause} disabled={!song}>
            <Icon name={isPlaying ? 'pause' : 'play'} size={20}/>
          </button>
          <button className="np-ctl" onClick={onNext}><Icon name="next" size={18}/></button>
          <button
            className="np-ctl"
            style={{ color: repeatMode > 0 ? 'var(--accent)' : undefined }}
            onClick={onRepeat}>
            <Icon name={repeatMode === 2 ? 'repeat1' : 'repeat'} size={16} stroke={1.8}/>
          </button>
        </div>
        <div className="np-scrub">
          <span className="np-time">{fmtTime(currentPosSec)}</span>
          <div className="np-bar" ref={barRef} onClick={handleBarClick}>
            <div className="np-bar-fill" style={{ width: `${progress * 100}%` }}/>
            <div className="np-bar-thumb" style={{ left: `${progress * 100}%` }}/>
          </div>
          <span className="np-time">{song?.duration || fmtTime(durationSec)}</span>
        </div>
      </div>

      <div className="np-right">
        <button className="np-ctl"><Icon name="sparkle" size={16} stroke={1.8}/></button>
        <button className="np-ctl"><Icon name="queue" size={16} stroke={1.8}/></button>
        <div className="np-vol">
          <Icon name="volume" size={16}/>
          <div className="np-bar small" ref={volRef} onClick={handleVolClick}>
            <div className="np-bar-fill" style={{ width: `${volume * 100}%` }}/>
            <div className="np-bar-thumb" style={{ left: `${volume * 100}%` }}/>
          </div>
        </div>
      </div>
    </footer>
  );
};

// ─── Library View ─────────────────────────────────────────────────────────────

interface LibraryViewProps {
  downloadedSongs: DownloadedSong[];
  currentSongId?: string;
  isPlaying: boolean;
  downloadedIds: Set<string>;
  downloadProgress: Record<string, number>;
  onPlay: (song: DownloadedSong) => void;
  onDelete: (id: string) => void;
}

const LibraryView: React.FC<LibraryViewProps> = ({
  downloadedSongs, currentSongId, isPlaying, downloadedIds, downloadProgress, onPlay, onDelete,
}) => {
  const tracks = downloadedSongs as TrackRowSong[];
  return (
    <div className="view">
      <header className="hero glass-hero">
        <div className="hero-text">
          <div className="eyebrow"><Icon name="library" size={11} stroke={2}/><span>Your Library</span></div>
          <h1>Offline Library</h1>
          <p>
            {downloadedSongs.length > 0
              ? `${downloadedSongs.length} song${downloadedSongs.length !== 1 ? 's' : ''} saved on this device`
              : 'Download songs from Search to listen offline'}
          </p>
        </div>
        <div className="hero-stack">
          {downloadedSongs.length > 0
            ? downloadedSongs.slice(0, 4).map(s =>
                <SongCover key={s.id} thumbnail={s.thumbnail} seed={s.title[0] + s.id} size={68} radius={14}/>)
            : [3, 1].map(n => <Cover key={n} seed={`H${n}`} size={68} radius={14}/>)}
        </div>
      </header>

      {tracks.length === 0 ? (
        <div className="empty glass-soft">
          <Icon name="download" size={28}/>
          <h3>No offline songs yet</h3>
          <p>Search for music and tap the download icon to save songs.</p>
        </div>
      ) : (
        <TrackTable
          tracks={tracks}
          currentSongId={currentSongId}
          isPlaying={isPlaying}
          downloadedIds={downloadedIds}
          downloadProgress={downloadProgress}
          onPlay={onPlay}
          onDownload={() => {}}
          onDelete={onDelete}
          showDelete
        />
      )}
    </div>
  );
};

// ─── Search cache (module-level, survives re-renders, clears on app restart) ──
const searchCache = new Map<string, { results: Song[]; ts: number }>();
const CACHE_TTL_MS = 5 * 60 * 1000; // 5 minutes

const GENRES = [
  { label: 'Pop',        color: 'oklch(68% 0.20 0)'   },
  { label: 'Hip-Hop',   color: 'oklch(60% 0.18 295)'  },
  { label: 'Rock',       color: 'oklch(50% 0.20 25)'   },
  { label: 'Electronic', color: 'oklch(58% 0.18 230)'  },
  { label: 'R&B',        color: 'oklch(65% 0.18 50)'   },
  { label: 'Jazz',       color: 'oklch(60% 0.16 155)'  },
  { label: 'Classical',  color: 'oklch(58% 0.14 275)'  },
  { label: 'Lo-fi',      color: 'oklch(55% 0.12 55)'   },
  { label: 'K-Pop',      color: 'oklch(70% 0.20 340)'  },
  { label: 'Indie',      color: 'oklch(58% 0.16 140)'  },
  { label: 'Afrobeats',  color: 'oklch(72% 0.18 70)'   },
  { label: 'Latin',      color: 'oklch(68% 0.20 45)'   },
];

// ─── Search View ──────────────────────────────────────────────────────────────

interface SearchViewProps {
  query: string;
  setQuery: (q: string) => void;
  songs: Song[];
  searching: boolean;
  currentSongId?: string;
  isPlaying: boolean;
  downloadedIds: Set<string>;
  downloadProgress: Record<string, number>;
  recentSearches: string[];
  onSearch: (e: React.FormEvent) => Promise<void>;
  onPlay: (song: Song) => void;
  onDownload: (song: Song) => void;
}

const SearchView: React.FC<SearchViewProps> = ({
  query, setQuery, songs, searching, currentSongId, isPlaying,
  downloadedIds, downloadProgress, recentSearches, onSearch, onPlay, onDownload,
}) => {
  const top = songs[0];
  return (
    <div className="view">
      <form onSubmit={onSearch} style={{ display: 'contents' }}>
        <div className="search-bar glass-inner">
          <Icon name="search" size={18}/>
          <input
            value={query}
            onChange={e => setQuery(e.target.value)}
            placeholder="Search YouTube for music…"
            autoFocus
          />
          {query && (
            <button type="button" className="search-clear" onClick={() => setQuery('')}>
              <Icon name="x" size={14}/>
            </button>
          )}
        </div>
      </form>

      {/* ── Recommendations (shown when search is empty) ── */}
      {!query && songs.length === 0 && !searching && (
        <div className="reco-page">
          {recentSearches.length > 0 && (
            <div className="reco-section">
              <p className="reco-title">Recent searches</p>
              <div className="reco-chips">
                {recentSearches.map(r => (
                  <button key={r} className="reco-chip" onClick={() => setQuery(r)}>
                    <Icon name="search" size={11}/>
                    {r}
                  </button>
                ))}
              </div>
            </div>
          )}
          <div className="reco-section">
            <p className="reco-title">Browse by genre</p>
            <div className="reco-chips">
              {GENRES.map(g => (
                <button
                  key={g.label}
                  className="genre-chip"
                  style={{ '--genre-color': g.color } as React.CSSProperties}
                  onClick={() => setQuery(g.label)}>
                  <span className="genre-dot"/>
                  {g.label}
                </button>
              ))}
            </div>
          </div>
        </div>
      )}

      {searching && (
        <div className="empty glass-soft">
          <div style={{ fontSize: 11, fontWeight: 700, letterSpacing: '.1em', textTransform: 'uppercase', color: 'var(--accent)', animation: 'pulse 1.5s ease-in-out infinite' }}>
            Searching…
          </div>
        </div>
      )}

      {!searching && songs.length > 0 && (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 22 }}>
          <div className="search-grid">
            {top && (
              <section className="top-result glass-soft">
                <div className="section-head"><h3>Top result</h3></div>
                <div className="top-card" onDoubleClick={() => onPlay(top)}>
                  <SongCover thumbnail={top.thumbnail} seed={top.title[0] + top.id} size={120} radius={14}/>
                  <div className="top-meta">
                    <div className="top-tag">YouTube · Music</div>
                    <div className="top-title">{top.title}</div>
                    <div className="top-sub">{top.artist} · {top.duration}</div>
                  </div>
                  <button className="big-play" onClick={() => onPlay(top)}>
                    <Icon name={currentSongId === top.id && isPlaying ? 'pause' : 'play'} size={20}/>
                  </button>
                </div>
              </section>
            )}

            <section>
              <div className="section-head">
                <h3>Songs <span className="count-pill">{songs.length}</span></h3>
              </div>
              <div className="mini-list glass-soft">
                {songs.slice(0, 6).map(t => (
                  <div key={t.id} className="mini-row" onDoubleClick={() => onPlay(t)}>
                    <SongCover thumbnail={t.thumbnail} seed={t.title[0] + t.id} size={36} radius={6}/>
                    <div className="mini-meta">
                      <div className="mini-title">{t.title}</div>
                      <div className="mini-sub">{t.artist}</div>
                    </div>
                    <button
                      className={`dl-mini ${downloadedIds.has(t.id) ? 'done' : ''}`}
                      onClick={() => onDownload(t)}>
                      {downloadProgress[t.id] != null
                        ? <span style={{ fontSize: 10, fontWeight: 700, color: 'var(--accent)' }}>{Math.round(downloadProgress[t.id])}%</span>
                        : <Icon name={downloadedIds.has(t.id) ? 'downloaded' : 'download'} size={14}/>}
                    </button>
                    <div className="mini-dur">{t.duration}</div>
                  </div>
                ))}
              </div>
            </section>
          </div>

          {songs.length > 1 && (
            <section>
              <div className="section-head">
                <h3>All results <span className="count-pill">{songs.length}</span></h3>
              </div>
              <TrackTable
                tracks={songs}
                currentSongId={currentSongId}
                isPlaying={isPlaying}
                downloadedIds={downloadedIds}
                downloadProgress={downloadProgress}
                onPlay={onPlay}
                onDownload={onDownload}
              />
            </section>
          )}
        </div>
      )}

      {!searching && query && songs.length === 0 && (
        <div className="empty glass-soft">
          <Icon name="search" size={24}/>
          <h3>No results for "{query}"</h3>
          <p>Try a different title or check your internet connection.</p>
        </div>
      )}
    </div>
  );
};

// ─── Downloads View ───────────────────────────────────────────────────────────

interface DownloadsViewProps {
  downloadedSongs: DownloadedSong[];
  activeDownloads: { song: Song; progress: number }[];
  currentSongId?: string;
  isPlaying: boolean;
  downloadedIds: Set<string>;
  downloadProgress: Record<string, number>;
  onPlay: (song: DownloadedSong) => void;
  onDelete: (id: string) => void;
}

const DownloadsView: React.FC<DownloadsViewProps> = ({
  downloadedSongs, activeDownloads, currentSongId, isPlaying,
  downloadedIds, downloadProgress, onPlay, onDelete,
}) => {
  const tracks = downloadedSongs as TrackRowSong[];
  return (
    <div className="view">
      <header className="hero glass-hero dl-hero">
        <div className="hero-text">
          <div className="eyebrow"><Icon name="download" size={12} stroke={2.2}/><span>Offline</span></div>
          <h1>Downloads</h1>
          <p>Saved on this device · plays without an internet connection.</p>
          <div className="dl-stats">
            <div className="dl-stat">
              <div className="dl-num">{downloadedSongs.length}</div>
              <div className="dl-lbl">Songs</div>
            </div>
            {activeDownloads.length > 0 && (
              <div className="dl-stat">
                <div className="dl-num">{activeDownloads.length}</div>
                <div className="dl-lbl">Downloading</div>
              </div>
            )}
          </div>
        </div>
        <div className="dl-hero-right">
          {downloadedSongs.length > 0 && (
            <button
              className="ghost-btn danger"
              onClick={() => downloadedSongs.forEach(s => onDelete(s.id))}>
              <Icon name="trash" size={14} stroke={2}/><span>Clear all</span>
            </button>
          )}
        </div>
      </header>

      {activeDownloads.length > 0 && (
        <section className="dl-section">
          <div className="section-head">
            <h3>Downloading <span className="count-pill">{activeDownloads.length}</span></h3>
          </div>
          <div className="dl-queue glass-soft">
            {activeDownloads.map(({ song, progress }) => (
              <div key={song.id} className="dl-row">
                <SongCover thumbnail={song.thumbnail} seed={song.title[0] + song.id} size={40} radius={7}/>
                <div>
                  <div className="dl-title">{song.title}</div>
                  <div className="dl-sub">{song.artist}</div>
                  <div className="dl-progress-line">
                    <div className="dl-progress">
                      <div className="dl-progress-fill" style={{ width: `${progress}%` }}/>
                    </div>
                    <span className="dl-eta">{Math.round(progress)}%</span>
                  </div>
                </div>
              </div>
            ))}
          </div>
        </section>
      )}

      <section className="dl-section">
        <div className="section-head">
          <h3>Available offline <span className="count-pill">{downloadedSongs.length}</span></h3>
        </div>
        {tracks.length === 0 ? (
          <div className="empty glass-soft">
            <Icon name="download" size={28}/>
            <h3>No downloads yet</h3>
            <p>Search for music and tap the save icon to add songs here.</p>
          </div>
        ) : (
          <TrackTable
            tracks={tracks}
            currentSongId={currentSongId}
            isPlaying={isPlaying}
            downloadedIds={downloadedIds}
            downloadProgress={downloadProgress}
            onPlay={onPlay}
            onDownload={() => {}}
            onDelete={onDelete}
            showDelete
          />
        )}
      </section>
    </div>
  );
};

// ─── Playlists View ───────────────────────────────────────────────────────────

const PlaylistsView: React.FC = () => (
  <div className="view">
    <div className="empty glass-soft" style={{ margin: '80px auto' }}>
      <Icon name="playlist" size={32}/>
      <h3>Playlists coming soon</h3>
      <p>Create and manage playlists in a future update.</p>
    </div>
  </div>
);

// ─── Settings Modal ───────────────────────────────────────────────────────────

interface Tweaks {
  accent:    string;
  warmth:    string;
  quality:   string;
  crossfade: number;
  gapless:   boolean;
  normalize: boolean;
  lyrics:    boolean;
  dlQuality: string;
  storage:   number;
  autoDl:    boolean;
  wifiOnly:  boolean;
}

const ACCENT_OPTIONS = [
  { id: 'violet', color: 'oklch(64% 0.20 295)', label: 'Violet' },
  { id: 'coral',  color: 'oklch(67% 0.20 25)',  label: 'Coral'  },
  { id: 'ocean',  color: 'oklch(60% 0.18 230)', label: 'Ocean'  },
  { id: 'forest', color: 'oklch(58% 0.16 155)', label: 'Forest' },
];

const ACCENT_MAP: Record<string, { a: string; b: string }> = {
  violet: { a: 'oklch(64% 0.20 295)', b: 'oklch(72% 0.18 200)' },
  coral:  { a: 'oklch(67% 0.20 25)',  b: 'oklch(72% 0.18 60)'  },
  ocean:  { a: 'oklch(60% 0.18 230)', b: 'oklch(68% 0.16 195)' },
  forest: { a: 'oklch(58% 0.16 155)', b: 'oklch(66% 0.16 110)' },
};

const WARMTH_OPTIONS = [
  { id: 'warm',     label: 'Warm',     swatches: ['#FFD7B0','#FFB7C8','#FFE2AE','#F2C3FF'] },
  { id: 'balanced', label: 'Balanced', swatches: ['#FFC7E6','#FFD89A','#93D9FF','#C7B9FF'] },
  { id: 'cool',     label: 'Cool',     swatches: ['#C7E2FF','#B6F0E2','#D4C7FF','#A8C9FF'] },
];

const WARMTH_MAP: Record<string, { tl: string; tr: string; br: string; bl: string; g1: string; g2: string }> = {
  warm:     { tl: '#FFD7B0', tr: '#FFB7C8', br: '#FFE2AE', bl: '#F2C3FF', g1: '#FFF5E4', g2: '#FFE3D5' },
  balanced: { tl: '#FFC7E6', tr: '#FFD89A', br: '#93D9FF', bl: '#C7B9FF', g1: '#FFEFD9', g2: '#E6D7FF' },
  cool:     { tl: '#C7E2FF', tr: '#B6F0E2', br: '#D4C7FF', bl: '#A8C9FF', g1: '#E8F4FF', g2: '#D8EAFF' },
};

interface SettingsProps {
  open:     boolean;
  onClose:  () => void;
  tweaks:   Tweaks;
  setTweak: <K extends keyof Tweaks>(k: K, v: Tweaks[K]) => void;
}

const ToggleRow: React.FC<{ label: string; desc: string; value: boolean; onChange: (v: boolean) => void }> = ({ label, desc, value, onChange }) => (
  <label className="s-toggle-row">
    <div className="s-toggle-text">
      <div className="s-toggle-label">{label}</div>
      <div className="s-toggle-desc">{desc}</div>
    </div>
    <button type="button" role="switch" aria-checked={value}
      className={`s-switch ${value ? 'on' : ''}`}
      onClick={() => onChange(!value)}>
      <span className="s-switch-thumb"/>
    </button>
  </label>
);

const SettingsModal: React.FC<SettingsProps> = ({ open, onClose, tweaks, setTweak }) => {
  const [section, setSection] = useState('appearance');
  if (!open) return null;

  const sections = [
    { id: 'appearance', label: 'Appearance', icon: 'sparkle'  },
    { id: 'playback',   label: 'Playback',   icon: 'play'     },
    { id: 'downloads',  label: 'Downloads',  icon: 'download' },
    { id: 'about',      label: 'About',      icon: 'sliders'  },
  ];

  return (
    <div className="settings-overlay" onClick={onClose}>
      <div className="settings-modal glass-strong" onClick={e => e.stopPropagation()} role="dialog" aria-label="Settings">
        <header className="settings-head">
          <h2>Settings</h2>
          <button className="settings-x" onClick={onClose} aria-label="Close"><Icon name="x" size={16}/></button>
        </header>

        <div className="settings-body">
          <nav className="settings-nav">
            {sections.map(s => (
              <button key={s.id}
                className={`settings-nav-item ${section === s.id ? 'on' : ''}`}
                onClick={() => setSection(s.id)}>
                <Icon name={s.icon} size={14}/>
                <span>{s.label}</span>
              </button>
            ))}
          </nav>

          <div className="settings-content">

            {section === 'appearance' && (<>
              <div className="s-group">
                <div className="s-label">
                  <h3>Glass tone</h3>
                  <p>Sets the ambient light tint behind the interface.</p>
                </div>
                <div className="s-warmth-row">
                  {WARMTH_OPTIONS.map(w => (
                    <button key={w.id}
                      className={`s-warmth ${tweaks.warmth === w.id ? 'on' : ''}`}
                      onClick={() => setTweak('warmth', w.id)}>
                      <div className="s-warmth-prev">
                        {w.swatches.map((c, i) => <span key={i} style={{ background: c }}/>)}
                      </div>
                      <div className="s-warmth-label">
                        <span>{w.label}</span>
                        {tweaks.warmth === w.id && <Icon name="check" size={12} stroke={2.5}/>}
                      </div>
                    </button>
                  ))}
                </div>
              </div>

              <div className="s-group">
                <div className="s-label">
                  <h3>Accent color</h3>
                  <p>Controls primary buttons, progress fills, and the now-playing highlight.</p>
                </div>
                <div className="s-accent-row">
                  {ACCENT_OPTIONS.map(a => (
                    <button key={a.id}
                      className={`s-accent ${tweaks.accent === a.id ? 'on' : ''}`}
                      onClick={() => setTweak('accent', a.id)}
                      title={a.label}>
                      <span className="s-accent-dot" style={{ background: a.color }}>
                        {tweaks.accent === a.id && <Icon name="check" size={14} stroke={3}/>}
                      </span>
                      <span className="s-accent-name">{a.label}</span>
                    </button>
                  ))}
                </div>
              </div>
            </>)}

            {section === 'playback' && (<>
              <div className="s-group">
                <div className="s-label">
                  <h3>Streaming quality</h3>
                  <p>Higher quality uses more data when streaming online.</p>
                </div>
                <div className="s-radio-stack">
                  {([
                    { id: 'auto',      label: 'Automatic', desc: 'Adjusts to your connection' },
                    { id: 'normal',    label: 'Normal',    desc: '96 kbps · AAC'              },
                    { id: 'high',      label: 'High',      desc: '160 kbps · AAC'             },
                    { id: 'very-high', label: 'Very high', desc: '320 kbps · AAC'             },
                  ] as const).map(o => (
                    <label key={o.id} className={`s-radio ${tweaks.quality === o.id ? 'on' : ''}`}>
                      <input type="radio" name="quality" checked={tweaks.quality === o.id} onChange={() => setTweak('quality', o.id)}/>
                      <span className="s-radio-dot"/>
                      <div className="s-radio-text">
                        <div>{o.label}</div>
                        <div className="s-radio-desc">{o.desc}</div>
                      </div>
                    </label>
                  ))}
                </div>
              </div>

              <div className="s-group">
                <div className="s-label">
                  <h3>Crossfade</h3>
                  <p>Blends the end of one track with the start of the next.</p>
                </div>
                <div className="s-slider-row">
                  <input type="range" min="0" max="12" step="1"
                    value={tweaks.crossfade}
                    onChange={e => setTweak('crossfade', Number(e.target.value))}/>
                  <div className="s-slider-val">{tweaks.crossfade}s</div>
                </div>
              </div>

              <div className="s-group">
                <ToggleRow label="Gapless playback"           desc="Eliminate silence between tracks."           value={tweaks.gapless}   onChange={v => setTweak('gapless',   v)}/>
                <ToggleRow label="Normalize volume"           desc="Even out loudness across tracks."            value={tweaks.normalize} onChange={v => setTweak('normalize', v)}/>
                <ToggleRow label="Show lyrics in mini-player" desc="Display synchronized lyrics when available." value={tweaks.lyrics}    onChange={v => setTweak('lyrics',    v)}/>
              </div>
            </>)}

            {section === 'downloads' && (<>
              <div className="s-group">
                <div className="s-label">
                  <h3>Download quality</h3>
                  <p>Higher quality downloads use more storage.</p>
                </div>
                <div className="s-radio-stack">
                  {([
                    { id: 'normal',    label: 'Normal',    desc: '96 kbps · ~2 MB per track'  },
                    { id: 'high',      label: 'High',      desc: '160 kbps · ~4 MB per track' },
                    { id: 'very-high', label: 'Very high', desc: '320 kbps · ~8 MB per track' },
                  ] as const).map(o => (
                    <label key={o.id} className={`s-radio ${tweaks.dlQuality === o.id ? 'on' : ''}`}>
                      <input type="radio" name="dlquality" checked={tweaks.dlQuality === o.id} onChange={() => setTweak('dlQuality', o.id)}/>
                      <span className="s-radio-dot"/>
                      <div className="s-radio-text">
                        <div>{o.label}</div>
                        <div className="s-radio-desc">{o.desc}</div>
                      </div>
                    </label>
                  ))}
                </div>
              </div>

              <div className="s-group">
                <div className="s-label">
                  <h3>Storage limit</h3>
                  <p>Maximum space used for offline downloads.</p>
                </div>
                <div className="s-slider-row">
                  <input type="range" min="2" max="32" step="2"
                    value={tweaks.storage}
                    onChange={e => setTweak('storage', Number(e.target.value))}/>
                  <div className="s-slider-val">{tweaks.storage} GB</div>
                </div>
              </div>

              <div className="s-group">
                <ToggleRow label="Auto-download liked songs" desc="Save liked tracks for offline automatically." value={tweaks.autoDl}   onChange={v => setTweak('autoDl',   v)}/>
                <ToggleRow label="Download over Wi-Fi only"  desc="Pause downloads on mobile networks."         value={tweaks.wifiOnly} onChange={v => setTweak('wifiOnly', v)}/>
              </div>
            </>)}

            {section === 'about' && (
              <div className="s-empty">
                <Icon name="sparkle" size={28}/>
                <h3>Melody</h3>
                <p>YouTube music player · Liquid Glass</p>
                <p style={{ fontSize: 11.5, marginTop: 8, color: 'var(--fg-mute)' }}>Desktop · Electron + React · © 2026</p>
              </div>
            )}

          </div>
        </div>
      </div>
    </div>
  );
};

// ─── Main App ─────────────────────────────────────────────────────────────────

function App() {
  // Audio state
  const [currentSong, setCurrentSong]       = useState<Song | null>(null);
  const [streamUrl,   setStreamUrl]         = useState<string | null>(null);
  const [protocol,    setProtocol]          = useState<Protocol>('NONE');
  const [isPlaying,   setIsPlaying]         = useState(false);
  const [progress,    setProgress]          = useState(0);        // 0–1
  const [durationSec, setDurationSec]       = useState(0);
  const [currentPosSec, setCurrentPosSec]   = useState(0);
  const [volume,      setVolume]            = useState(0.68);
  const [repeatMode,  setRepeatMode]        = useState<RepeatMode>(0);

  // UI state
  const [view,          setView]          = useState<ViewType>('library');
  const [status,        setStatus]        = useState('');
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [tweaks, setTweaksState] = useState<Tweaks>({
    accent: 'violet', warmth: 'balanced', quality: 'high',
    crossfade: 4, gapless: true, normalize: true, lyrics: false,
    dlQuality: 'very-high', storage: 8, autoDl: true, wifiOnly: true,
  });
  const setTweak = <K extends keyof Tweaks>(k: K, v: Tweaks[K]) =>
    setTweaksState(prev => ({ ...prev, [k]: v }));

  // Search state
  const [query,          setQuery]          = useState('');
  const [songs,          setSongs]          = useState<Song[]>([]);
  const [searching,      setSearching]      = useState(false);
  const [recentSearches, setRecentSearches] = useState<string[]>([]);

  // Download state
  const [downloadedSongs,   setDownloadedSongs]   = useState<DownloadedSong[]>([]);
  const [downloadProgress,  setDownloadProgress]  = useState<Record<string, number>>({});

  const audioRef          = useRef<HTMLAudioElement>(null);
  const ytPlayerRef       = useRef<any>(null);
  const progressInterval  = useRef<any>(null);
  const statusTimeout     = useRef<any>(null);

  const downloadedIds = useMemo(() => new Set(downloadedSongs.map(s => s.id)), [downloadedSongs]);

  const activeDownloads = useMemo(() =>
    Object.entries(downloadProgress).flatMap(([id, prog]) => {
      const song = songs.find(s => s.id === id);
      return song ? [{ song, progress: prog }] : [];
    }), [downloadProgress, songs]);

  // Apply accent CSS vars
  useEffect(() => {
    const colors = ACCENT_MAP[tweaks.accent] || ACCENT_MAP.violet;
    document.documentElement.style.setProperty('--accent',   colors.a);
    document.documentElement.style.setProperty('--accent-2', colors.b);
  }, [tweaks.accent]);

  // Apply warmth CSS vars
  useEffect(() => {
    const w = WARMTH_MAP[tweaks.warmth] || WARMTH_MAP.balanced;
    const root = document.documentElement;
    root.style.setProperty('--warmth-tl', w.tl);
    root.style.setProperty('--warmth-tr', w.tr);
    root.style.setProperty('--warmth-br', w.br);
    root.style.setProperty('--warmth-bl', w.bl);
    root.style.setProperty('--warmth-grad-1', w.g1);
    root.style.setProperty('--warmth-grad-2', w.g2);
  }, [tweaks.warmth]);

  // Initial setup
  useEffect(() => {
    if (!window.YT) {
      const tag = document.createElement('script');
      tag.src = 'https://www.youtube.com/iframe_api';
      const first = document.getElementsByTagName('script')[0];
      if (first?.parentNode) first.parentNode.insertBefore(tag, first);
      else document.head.appendChild(tag);
    }
    loadDownloadedSongs();
  }, []);

  // Sync volume
  useEffect(() => {
    if (audioRef.current) audioRef.current.volume = volume;
  }, [volume]);

  const showStatus = (msg: string, autoClear = 0) => {
    clearTimeout(statusTimeout.current);
    setStatus(msg);
    if (autoClear > 0) statusTimeout.current = setTimeout(() => setStatus(''), autoClear);
  };

  const loadDownloadedSongs = async () => {
    try {
      const offline = await getAllDownloadedSongs();
      setDownloadedSongs(offline || []);
    } catch {}
  };

  // Progress tracker
  useEffect(() => {
    if (isPlaying) {
      progressInterval.current = setInterval(() => {
        if ((protocol === 'STEALTH' || protocol === 'LOCAL') && audioRef.current) {
          const cur = audioRef.current.currentTime;
          const dur = audioRef.current.duration;
          if (dur) { setCurrentPosSec(cur); setDurationSec(dur); setProgress(cur / dur); }
        } else if (protocol === 'HYBRID' && ytPlayerRef.current?.getCurrentTime) {
          try {
            const cur = ytPlayerRef.current.getCurrentTime();
            const dur = ytPlayerRef.current.getDuration();
            if (dur) { setCurrentPosSec(cur); setDurationSec(dur); setProgress(cur / dur); }
          } catch {}
        }
      }, 1000);
    } else {
      clearInterval(progressInterval.current);
    }
    return () => clearInterval(progressInterval.current);
  }, [isPlaying, protocol]);

  const runSearch = async (q: string) => {
    const key = q.trim().toLowerCase();
    if (!key) return;

    // Serve from cache if fresh
    const cached = searchCache.get(key);
    if (cached && Date.now() - cached.ts < CACHE_TTL_MS) {
      setSongs(cached.results);
      setView('search');
      return;
    }

    setSearching(true);
    setSongs([]);
    setView('search');
    try {
      const ytUrl = `https://www.youtube.com/results?search_query=${encodeURIComponent(q)}&sp=EgIQAQ%253D%253D`;
      const fetchUrl = isElectron()
        ? ytUrl
        : `https://api.codetabs.com/v1/proxy?quest=${encodeURIComponent(ytUrl)}`;
      const res = await fetch(fetchUrl);
      const html = await res.text();
      const startTag = 'var ytInitialData = ';
      const startPos = html.indexOf(startTag);
      if (startPos === -1) throw new Error('no ytInitialData');
      const jsonStr = html.substring(startPos + startTag.length, html.indexOf(';</script>', startPos));
      const data = JSON.parse(jsonStr);
      const contents = data.contents.twoColumnSearchResultsRenderer
        .primaryContents.sectionListRenderer.contents[0].itemSectionRenderer.contents;
      const results = contents.filter((i: any) => i.videoRenderer).map((i: any) => {
        const v = i.videoRenderer;
        const thumb = v.thumbnail.thumbnails[v.thumbnail.thumbnails.length - 1].url;
        return {
          id: v.videoId,
          title: v.title.runs[0].text,
          artist: v.ownerText.runs[0].text,
          thumbnail: thumb.startsWith('//') ? `https:${thumb}` : thumb,
          duration: v.lengthText?.simpleText || '0:00',
        };
      });
      setSongs(results);
      searchCache.set(key, { results, ts: Date.now() });
      setRecentSearches(prev =>
        [q, ...prev.filter(r => r.toLowerCase() !== key)].slice(0, 5)
      );
    } catch {
      showStatus('Network error — try again.', 3000);
    } finally {
      setSearching(false);
    }
  };

  // Auto-search: debounce 500 ms after the user stops typing
  useEffect(() => {
    if (!query.trim()) { setSongs([]); return; }
    const timer = setTimeout(() => runSearch(query), 500);
    return () => clearTimeout(timer);
  }, [query]); // eslint-disable-line react-hooks/exhaustive-deps

  const handleSearch = async (e: React.FormEvent) => {
    e.preventDefault();
    runSearch(query);
  };

  const initHybridPlayer = (videoId: string) => {
    if (!window.YT?.Player) {
      showStatus('Player loading…');
      setTimeout(() => initHybridPlayer(videoId), 1000);
      return;
    }
    if (ytPlayerRef.current) { try { ytPlayerRef.current.destroy(); } catch {} }
    ytPlayerRef.current = new window.YT.Player('ghost-player', {
      height: '1', width: '1', videoId,
      host: 'https://www.youtube.com',
      playerVars: {
        autoplay: 1, controls: 0, modestbranding: 1, rel: 0,
        origin: window.location.origin || 'http://localhost:5173',
      },
      events: {
        onReady: (ev: any) => { ev.target.playVideo(); setIsPlaying(true); showStatus(''); },
        onStateChange: (ev: any) => {
          if (ev.data === window.YT.PlayerState.PLAYING) { setIsPlaying(true); showStatus(''); }
          if (ev.data === window.YT.PlayerState.PAUSED)  setIsPlaying(false);
          if (ev.data === window.YT.PlayerState.ENDED)   handleNext();
        },
        onError: () => { showStatus('Playback restricted.', 3000); setIsPlaying(false); },
      },
    });
  };

  const playSong = async (song: Song | DownloadedSong) => {
    setCurrentSong(song as Song);
    setProtocol('NONE');
    setIsPlaying(false);
    setProgress(0);
    setCurrentPosSec(0);
    setStreamUrl(null);
    showStatus('Syncing…');

    const local = downloadedSongs.find(s => s.id === song.id);
    if (local) {
      showStatus('Offline playback');
      try {
        const audioBase64 = await getAudioBase64(song.id);
        if (!audioBase64) throw new Error('no data');
        const bytes = atob(audioBase64);
        const arr   = new Uint8Array(bytes.length);
        for (let i = 0; i < bytes.length; i++) arr[i] = bytes.charCodeAt(i);
        setStreamUrl(URL.createObjectURL(new Blob([arr], { type: 'audio/mp4' })));
        setProtocol('LOCAL');
        setIsPlaying(true);
        showStatus('');
        return;
      } catch {}
    }

    showStatus('Optimizing link…');
    const resolvedUrl = await resolveStreamUrl(song.id);
    if (resolvedUrl) {
      setStreamUrl(resolvedUrl);
      setProtocol('STEALTH');
      setIsPlaying(true);
      showStatus('');
    } else {
      setProtocol('HYBRID');
      showStatus('Fallback engine…');
      setTimeout(() => initHybridPlayer(song.id), 500);
    }
  };

  const handleDownload = async (song: Song) => {
    if (downloadedIds.has(song.id)) return;
    showStatus('Saving to library…');
    try {
      await downloadAndSaveSong(song.id, song.title, song.artist, song.thumbnail, song.duration, (p) => {
        setDownloadProgress(prev => ({ ...prev, [song.id]: p }));
      });
      await loadDownloadedSongs();
      showStatus('Saved to library', 2500);
      setDownloadProgress(prev => {
        const next = { ...prev };
        delete next[song.id];
        return next;
      });
    } catch {
      showStatus('Save failed.', 3000);
    }
  };

  const handleDelete = async (id: string) => {
    await deleteStoredSong(id);
    await loadDownloadedSongs();
  };

  const togglePlay = () => {
    if ((protocol === 'STEALTH' || protocol === 'LOCAL') && audioRef.current) {
      isPlaying ? audioRef.current.pause() : audioRef.current.play();
      setIsPlaying(!isPlaying);
    } else if (protocol === 'HYBRID' && ytPlayerRef.current) {
      isPlaying ? ytPlayerRef.current.pauseVideo() : ytPlayerRef.current.playVideo();
      setIsPlaying(!isPlaying);
    }
  };

  const currentList: (Song | DownloadedSong)[] = view === 'search' ? songs : downloadedSongs;

  const handleNext = () => {
    const idx = currentList.findIndex(s => s.id === currentSong?.id);
    if (idx !== -1 && idx < currentList.length - 1) playSong(currentList[idx + 1]);
    else if (idx !== -1 && repeatMode === 1) playSong(currentList[0]);
  };

  const handlePrev = () => {
    const idx = currentList.findIndex(s => s.id === currentSong?.id);
    if (idx > 0) playSong(currentList[idx - 1]);
  };

  const handleSeek = (p: number) => {
    setProgress(p);
    const pos = p * durationSec;
    if ((protocol === 'STEALTH' || protocol === 'LOCAL') && audioRef.current)
      audioRef.current.currentTime = pos;
    else if (protocol === 'HYBRID' && ytPlayerRef.current)
      ytPlayerRef.current.seekTo(pos, true);
  };

  return (
    <div className="app-shell">
      <div className="window">
        <WindowChrome view={view} onNav={setView} onOpenSettings={() => setSettingsOpen(true)}/>

        <div className="body">
          <Sidebar
            view={view}
            onNav={setView}
            downloadedCount={downloadedSongs.length}
            queueCount={Object.keys(downloadProgress).length}
            online
          />

          <div className="main-pane">
            {view === 'library' && (
              <LibraryView
                downloadedSongs={downloadedSongs}
                currentSongId={currentSong?.id}
                isPlaying={isPlaying}
                downloadedIds={downloadedIds}
                downloadProgress={downloadProgress}
                onPlay={playSong}
                onDelete={handleDelete}
              />
            )}
            {view === 'search' && (
              <SearchView
                query={query}
                setQuery={setQuery}
                songs={songs}
                searching={searching}
                currentSongId={currentSong?.id}
                isPlaying={isPlaying}
                downloadedIds={downloadedIds}
                downloadProgress={downloadProgress}
                recentSearches={recentSearches}
                onSearch={handleSearch}
                onPlay={playSong}
                onDownload={handleDownload}
              />
            )}
            {view === 'downloads' && (
              <DownloadsView
                downloadedSongs={downloadedSongs}
                activeDownloads={activeDownloads}
                currentSongId={currentSong?.id}
                isPlaying={isPlaying}
                downloadedIds={downloadedIds}
                downloadProgress={downloadProgress}
                onPlay={playSong}
                onDelete={handleDelete}
              />
            )}
            {view === 'playlists' && <PlaylistsView/>}
          </div>
        </div>

        <NowPlayingBar
          song={currentSong}
          isPlaying={isPlaying}
          progress={progress}
          volume={volume}
          currentPosSec={currentPosSec}
          durationSec={durationSec}
          isDownloaded={currentSong ? downloadedIds.has(currentSong.id) : false}
          dlProgress={currentSong ? downloadProgress[currentSong.id] : undefined}
          status={status}
          repeatMode={repeatMode}
          onPlayPause={togglePlay}
          onPrev={handlePrev}
          onNext={handleNext}
          onSeek={handleSeek}
          onVolume={setVolume}
          onDownload={() => currentSong && handleDownload(currentSong)}
          onRepeat={() => setRepeatMode(((repeatMode + 1) % 3) as RepeatMode)}
        />
      </div>

      <SettingsModal
        open={settingsOpen}
        onClose={() => setSettingsOpen(false)}
        tweaks={tweaks}
        setTweak={setTweak}
      />

      <audio
        ref={audioRef}
        src={streamUrl ?? undefined}
        autoPlay
        onPlay={() => setIsPlaying(true)}
        onPause={() => setIsPlaying(false)}
        onEnded={handleNext}
        onError={() => {
          if (protocol === 'STEALTH') {
            setProtocol('HYBRID');
            showStatus('Engine fallback…');
            setTimeout(() => currentSong && initHybridPlayer(currentSong.id), 100);
          } else if (protocol === 'LOCAL') {
            showStatus('Local file error', 3000);
          }
        }}
        style={{ display: 'none' }}
      />
      <div id="ghost-player" style={{ position: 'absolute', left: '-9999px', opacity: 0 }}/>
    </div>
  );
}

export default App;
