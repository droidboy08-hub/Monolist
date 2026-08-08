import React from 'react';

/* ---------------------------------------------------------------- */
/* ICONS — minimal, original line glyphs                            */
/* ---------------------------------------------------------------- */
export const Icon = ({ name, size = 18, stroke = 1.6, ...rest }: { name: string, size?: number, stroke?: number, [key: string]: any }) => {
  const props = {
    width: size, height: size, viewBox: "0 0 24 24",
    fill: "none", stroke: "currentColor",
    strokeWidth: stroke, strokeLinecap: "round" as const, strokeLinejoin: "round" as const,
    ...rest,
  };
  const paths: Record<string, React.ReactNode> = {
    library: <><path d="M4 5h2v14H4z"/><path d="M9 5h2v14H9z"/><path d="m15 6 5 13-2 .8L13 7z"/></>,
    search:  <><circle cx="11" cy="11" r="6.5"/><path d="m20 20-4.2-4.2"/></>,
    playlist:<><path d="M4 7h12"/><path d="M4 12h12"/><path d="M4 17h8"/><circle cx="18" cy="17" r="2.5"/><path d="M20.5 17V9l-3 1"/></>,
    download:<><path d="M12 4v11"/><path d="m7.5 10.5 4.5 4.5 4.5-4.5"/><path d="M5 19h14"/></>,
    downloaded:<><circle cx="12" cy="12" r="8.5"/><path d="m8.5 12.2 2.6 2.6 4.4-5"/></>,
    play:    <><path d="M7 5.5v13l11-6.5z" fill="currentColor"/></>,
    pause:   <><rect x="7" y="5.5" width="3.2" height="13" rx="1" fill="currentColor" stroke="none"/><rect x="13.8" y="5.5" width="3.2" height="13" rx="1" fill="currentColor" stroke="none"/></>,
    prev:    <><path d="M7 6v12"/><path d="M19 6 9 12l10 6z" fill="currentColor"/></>,
    next:    <><path d="M17 6v12"/><path d="M5 6l10 6L5 18z" fill="currentColor"/></>,
    shuffle: <><path d="M4 7h3l9 10h4"/><path d="m18 19 3-2-3-2"/><path d="M4 17h3l3-3.3"/><path d="m14 10.3 2-2.3h4"/><path d="m18 5 3 2-3 2"/></>,
    repeat:  <><path d="M5 9V8a2 2 0 0 1 2-2h11"/><path d="m15 3 3 3-3 3"/><path d="M19 15v1a2 2 0 0 1-2 2H6"/><path d="m9 21-3-3 3-3"/></>,
    heart:   <><path d="M12 20s-7-4.3-7-10.2A4 4 0 0 1 12 6.5 4 4 0 0 1 19 9.8c0 5.9-7 10.2-7 10.2z"/></>,
    heartF:  <><path d="M12 20s-7-4.3-7-10.2A4 4 0 0 1 12 6.5 4 4 0 0 1 19 9.8c0 5.9-7 10.2-7 10.2z" fill="currentColor"/></>,
    volume:  <><path d="M4 9.5v5h3.5L13 19V5L7.5 9.5z" fill="currentColor" stroke="none"/><path d="M16 9a4 4 0 0 1 0 6"/><path d="M19 6.5a8 8 0 0 1 0 11"/></>,
    queue:   <><path d="M4 6h11"/><path d="M4 12h11"/><path d="M4 18h7"/><path d="m18 13 4 2.5L18 18z" fill="currentColor"/></>,
    dots:    <><circle cx="5" cy="12" r="1.2" fill="currentColor"/><circle cx="12" cy="12" r="1.2" fill="currentColor"/><circle cx="19" cy="12" r="1.2" fill="currentColor"/></>,
    plus:    <><path d="M12 5v14"/><path d="M5 12h14"/></>,
    sliders: <><path d="M4 6h10"/><path d="M18 6h2"/><circle cx="16" cy="6" r="2"/><path d="M4 12h2"/><path d="M10 12h10"/><circle cx="8" cy="12" r="2"/><path d="M4 18h12"/><path d="M20 18h0"/><circle cx="18" cy="18" r="2"/></>,
    sparkle: <><path d="M12 4v5"/><path d="M12 15v5"/><path d="M4 12h5"/><path d="M15 12h5"/><path d="m6.5 6.5 3 3"/><path d="m14.5 14.5 3 3"/><path d="m17.5 6.5-3 3"/><path d="m9.5 14.5-3 3"/></>,
    chevron: <><path d="m9 6 6 6-6 6"/></>,
    pin:     <><path d="M12 17v5"/><path d="M8 6h8l-1 6 3 3H6l3-3z"/></>,
    pause2:  <><rect x="6" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none"/><rect x="14" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none"/></>,
    wifi:    <><path d="M2 9a16 16 0 0 1 20 0"/><path d="M5 12.5a11 11 0 0 1 14 0"/><path d="M8.5 16a6 6 0 0 1 7 0"/><circle cx="12" cy="19.5" r=".8" fill="currentColor"/></>,
    cloud:   <><path d="M7 17h10a4 4 0 0 0 .5-7.95 6 6 0 0 0-11.7 1.45A3.5 3.5 0 0 0 7 17z"/></>,
    check:   <><path d="m5 12.5 4.5 4.5L19 7"/></>,
    x:       <><path d="m6 6 12 12"/><path d="m18 6-12 12"/></>,
    arrow:   <><path d="M5 12h14"/><path d="m13 6 6 6-6 6"/></>,
    grid:    <><rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="1.5"/></>,
    list:    <><path d="M4 7h16"/><path d="M4 12h16"/><path d="M4 17h16"/></>,
    eq:      <><path d="M5 19v-7"/><path d="M5 9V5"/><path d="M12 19v-4"/><path d="M12 12V5"/><path d="M19 19v-9"/><path d="M19 7V5"/><circle cx="5" cy="10.5" r="1.5" fill="currentColor"/><circle cx="12" cy="13.5" r="1.5" fill="currentColor"/><circle cx="19" cy="8.5" r="1.5" fill="currentColor"/></>,
  };
  return <svg {...props}>{paths[name]}</svg>;
};

/* ---------------------------------------------------------------- */
/* COVER ART — generated or image-based                             */
/* ---------------------------------------------------------------- */
export const Cover = ({ seed = "a", size = 56, radius = 10, url }: { seed?: string, size?: number, radius?: number, url?: string }) => {
  if (url) {
    return (
        <div className="cover" style={{ width: size, height: size, borderRadius: radius }}>
            <img src={url} style={{ width: '100%', height: '100%', objectFit: 'cover' }} alt="cover" />
            <div className="cover-gloss"/>
        </div>
    );
  }

  // deterministic palette based on seed
  const palettes = [
    ["#FF8FB1","#FF5C8A","#3D1C5E"],
    ["#F5C16C","#E07A3B","#3B1D2A"],
    ["#7CE4D6","#3FA9F5","#0B2B5C"],
    ["#C3A0FF","#7B5BFF","#1E0C44"],
    ["#9CE89C","#3FBF87","#0E2A2F"],
    ["#FFD86E","#FF7A3B","#3A0F2C"],
    ["#7AD7FF","#3F6BFF","#11144F"],
    ["#FF9DCB","#A75BFF","#2A0E4F"],
    ["#FFEAA8","#FF6B6B","#2C1438"],
    ["#9DFFE0","#3FBFD8","#0A2E3F"],
    ["#FFCFA3","#E08AAE","#2B1546"],
    ["#B6B0FF","#5C7BFF","#0D1B4A"],
  ];
  const i = (seed.charCodeAt(0) + seed.length * 7) % palettes.length;
  const [a, b, c] = palettes[i];
  const angle = (seed.charCodeAt(seed.length - 1) % 12) * 30;
  const blob = (seed.charCodeAt(1 || 0) || 0) % 3;
  return (
    <div className="cover" style={{ width: size, height: size, borderRadius: radius, background: c }}>
      <div className="cover-grad" style={{
        background: `linear-gradient(${angle}deg, ${a} 0%, ${b} 60%, ${c} 100%)`
      }}/>
      <svg viewBox="0 0 100 100" className="cover-svg" preserveAspectRatio="xMidYMid slice">
        {blob === 0 && <circle cx="70" cy="30" r="32" fill={a} opacity=".55"/>}
        {blob === 1 && <rect x="-10" y="55" width="120" height="60" fill={a} opacity=".45" transform="rotate(-12 50 75)"/>}
        {blob === 2 && <><circle cx="20" cy="80" r="26" fill={a} opacity=".55"/><circle cx="78" cy="22" r="14" fill={b} opacity=".7"/></>}
      </svg>
      <div className="cover-gloss"/>
    </div>
  );
};

/* ---------------------------------------------------------------- */
/* WINDOW CHROME                                                    */
/* ---------------------------------------------------------------- */
export const WindowChrome = ({ platform, onNav, view, onOpenSettings }: { platform: 'mac' | 'win', onNav: (id: string) => void, view: string, onOpenSettings: () => void }) => {
  const tabs = [
    { id: "library",  label: "Library",   icon: "library"  },
    { id: "search",   label: "Search",    icon: "search"   },
    { id: "playlists",label: "Playlists", icon: "playlist" },
    { id: "downloads",label: "Downloads", icon: "download" },
  ];

  const handleControl = (cmd: string) => {
      try {
          // @ts-ignore
          const ipc = window.ipcRenderer || (window as any).require?.('electron').ipcRenderer;
          if (ipc) {
              ipc.send(`window-${cmd}`);
          }
      } catch (e) {
          console.error('Failed to send window control:', e);
      }
  };

  return (
    <div className={`chrome chrome-${platform}`} style={{ WebkitAppRegion: 'drag' } as any}>
      <div className="chrome-l" style={{ WebkitAppRegion: 'no-drag' } as any}>
        {platform === "mac" ? (
          <div className="traffic">
            <span className="t-close" onClick={() => handleControl('close')}/><span className="t-min" onClick={() => handleControl('min')}/><span className="t-max" onClick={() => handleControl('max')}/>
          </div>
        ) : null}
        <div className="chrome-nav">
          <button className="chrome-arrow"><Icon name="chevron" size={14} stroke={2} style={{ transform: "rotate(180deg)" }}/></button>
          <button className="chrome-arrow"><Icon name="chevron" size={14} stroke={2}/></button>
        </div>
      </div>

      <div className="chrome-tabs" style={{ WebkitAppRegion: 'no-drag' } as any}>
        {tabs.map(t => (
          <button key={t.id}
            className={`chrome-tab ${view === t.id ? "active" : ""}`}
            onClick={() => onNav(t.id)}>
            <Icon name={t.icon} size={14} stroke={1.8}/>
            <span>{t.label}</span>
          </button>
        ))}
      </div>

      <div className="chrome-r" style={{ WebkitAppRegion: 'no-drag' } as any}>
        <button className="chrome-arrow" title="Now playing"><Icon name="eq" size={14} stroke={1.8}/></button>
        <button className="chrome-arrow" title="Settings" onClick={onOpenSettings}><Icon name="sliders" size={14} stroke={1.8}/></button>
        {platform === "win" ? (
          <div className="win-controls">
            <span className="w-min" onClick={() => handleControl('min')}>—</span>
            <span className="w-max" onClick={() => handleControl('max')}>▢</span>
            <span className="w-close" onClick={() => handleControl('close')}>✕</span>
          </div>
        ) : null}
      </div>
    </div>
  );
};

/* ---------------------------------------------------------------- */
/* SIDEBAR                                                          */
/* ---------------------------------------------------------------- */
export const Sidebar = ({ view, onNav, online, downloadedCount }: { view: string, onNav: (id: string) => void, online: boolean, downloadedCount: number }) => {
  const items = [
    { id: "library",   label: "Library",   icon: "library",  count: 412 }, // Mock library count
    { id: "search",    label: "Search",    icon: "search"   },
    { id: "playlists", label: "Playlists", icon: "playlist", count: 0  },
    { id: "downloads", label: "Downloads", icon: "download", count: downloadedCount, badge: downloadedCount > 0 ? String(downloadedCount) : undefined },
  ];
  return (
    <aside className="sidebar glass">
      <div className="side-section">
        <div className="side-title">Browse</div>
        {items.map(it => (
          <button key={it.id}
            className={`side-item ${view === it.id ? "active" : ""}`}
            onClick={() => onNav(it.id)}>
            <Icon name={it.icon} size={16}/>
            <span className="side-label">{it.label}</span>
            {it.badge ? <span className="side-badge">{it.badge}</span> : null}
            {it.count != null ? <span className="side-count">{it.count}</span> : null}
          </button>
        ))}
      </div>

      <div className="side-footer">
        <div className={`net-pill ${online ? "ok" : "off"}`}>
          <Icon name={online ? "wifi" : "cloud"} size={12} stroke={2}/>
          <span>{online ? "Streaming · 320 kbps" : "Offline · Library only"}</span>
        </div>
        <div className="storage">
          <div className="storage-label">
            <span>Offline storage</span>
            <span>{(downloadedCount * 0.008).toFixed(1)} / 8 GB</span>
          </div>
          <div className="storage-bar"><div className="storage-fill" style={{ width: `${(downloadedCount * 0.1)}%` }}/></div>
        </div>
      </div>
    </aside>
  );
};

/* ---------------------------------------------------------------- */
/* TRACK TABLE                                                      */
/* ---------------------------------------------------------------- */
export interface GenericSong {
    id: string;
    title: string;
    artist: string;
    thumbnail: string;
    duration: string;
}

export const TrackTable = ({ 
    tracks, 
    onPlay, 
    currentId, 
    playing, 
    onToggleDl, 
    downloadedSongs,
    downloadProgress,
    showAlbum = true 
}: { 
    tracks: GenericSong[], 
    onPlay: (song: GenericSong) => void, 
    currentId?: string, 
    playing: boolean, 
    onToggleDl: (song: GenericSong) => void,
    downloadedSongs: string[],
    downloadProgress: Record<string, number>,
    showAlbum?: boolean 
}) => (
  <div className="track-table glass-inner">
    <div className="tt-head">
      <div className="tt-i">#</div>
      <div className="tt-t">Title</div>
      {showAlbum && <div className="tt-a">Album</div>}
      <div className="tt-p">Status</div>
      <div className="tt-d"><Icon name="download" size={14}/></div>
      <div className="tt-dur">Time</div>
      <div className="tt-x"></div>
    </div>
    {tracks.map((t, i) => {
      const isCur = t.id === currentId;
      const isDownloaded = downloadedSongs.includes(t.id);
      const progress = downloadProgress[t.id];

      return (
        <div key={t.id} className={`tt-row ${isCur ? "current" : ""}`} onDoubleClick={() => onPlay(t)}>
          <div className="tt-i">
            {isCur && playing
              ? <span className="bars" aria-label="playing"><span/><span/><span/></span>
              : <><span className="num">{i + 1}</span>
                  <button className="play-mini" onClick={() => onPlay(t)}>
                    <Icon name={isCur ? "pause" : "play"} size={12}/>
                  </button></>}
          </div>
          <div className="tt-t">
            <Cover url={t.thumbnail} size={34} radius={6}/>
            <div className="tt-meta">
              <div className="tt-title">{t.title}</div>
              <div className="tt-artist">{t.artist}</div>
            </div>
          </div>
          {showAlbum && <div className="tt-a">YouTube</div>}
          <div className="tt-p">{isDownloaded ? 'Offline' : 'Online'}</div>
          <div className="tt-d">
            <button
              className={`dl-mini ${isDownloaded ? "done" : ""}`}
              onClick={(e) => { e.stopPropagation(); onToggleDl(t); }}
              title={isDownloaded ? "Downloaded" : "Download for offline"}>
              {progress ? (
                <span className="text-[9px] font-bold">{Math.round(progress)}%</span>
              ) : (
                <Icon name={isDownloaded ? "downloaded" : "download"} size={14}/>
              )}
            </button>
          </div>
          <div className="tt-dur">{t.duration}</div>
          <div className="tt-x">
            <button className="dots-btn"><Icon name="dots" size={14}/></button>
          </div>
        </div>
      );
    })}
  </div>
);

/* ---------------------------------------------------------------- */
/* NOW PLAYING BAR                                                  */
/* ---------------------------------------------------------------- */
export const NowPlaying = ({ 
    track, 
    playing, 
    onPlayPause, 
    onPrev, 
    onNext, 
    onToggleDl, 
    progress, 
    setProgress, 
    volume, 
    setVolume,
    isDownloaded,
    currentTime,
    duration
}: { 
    track: GenericSong, 
    playing: boolean, 
    onPlayPause: () => void, 
    onPrev: () => void, 
    onNext: () => void, 
    onToggleDl: (song: GenericSong) => void, 
    progress: number, 
    setProgress: (p: number) => void, 
    volume: number, 
    setVolume: (v: number) => void,
    isDownloaded: boolean,
    currentTime: string,
    duration: string
}) => {
  return (
    <footer className="np glass-strong">
      <div className="np-left">
        <Cover url={track.thumbnail} size={52} radius={10}/>
        <div className="np-meta">
          <div className="np-title">{track.title}</div>
          <div className="np-artist">{track.artist}</div>
        </div>
        <button className={`np-heart`}>
          <Icon name={"heart"} size={16}/>
        </button>
        <button
          className={`np-dl ${isDownloaded ? "done" : ""}`}
          onClick={() => onToggleDl(track)}
          title={isDownloaded ? "Saved offline" : "Download for offline"}>
          <Icon name={isDownloaded ? "downloaded" : "download"} size={16} stroke={1.9}/>
          <span>{isDownloaded ? "Saved" : "Download"}</span>
        </button>
      </div>

      <div className="np-center">
        <div className="np-controls">
          <button className="np-ctl"><Icon name="shuffle" size={16} stroke={1.8}/></button>
          <button className="np-ctl" onClick={onPrev}><Icon name="prev" size={18}/></button>
          <button className="np-play" onClick={onPlayPause}>
            <Icon name={playing ? "pause" : "play"} size={20}/>
          </button>
          <button className="np-ctl" onClick={onNext}><Icon name="next" size={18}/></button>
          <button className="np-ctl"><Icon name="repeat" size={16} stroke={1.8}/></button>
        </div>
        <div className="np-scrub">
          <span className="np-time">{currentTime}</span>
          <div className="np-bar" onClick={e => {
            const r = e.currentTarget.getBoundingClientRect();
            setProgress(Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * 100);
          }}>
            <div className="np-bar-fill" style={{ width: `${progress}%` }}/>
            <div className="np-bar-thumb" style={{ left: `${progress}%` }}/>
          </div>
          <span className="np-time">{duration}</span>
        </div>
      </div>

      <div className="np-right">
        <button className="np-ctl"><Icon name="sparkle" size={16} stroke={1.8}/></button>
        <button className="np-ctl"><Icon name="queue" size={16} stroke={1.8}/></button>
        <div className="np-vol">
          <Icon name="volume" size={16}/>
          <div className="np-bar small" onClick={e => {
            const r = e.currentTarget.getBoundingClientRect();
            setVolume(Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)));
          }}>
            <div className="np-bar-fill" style={{ width: `${volume * 100}%` }}/>
            <div className="np-bar-thumb" style={{ left: `${volume * 100}%` }}/>
          </div>
        </div>
      </div>
    </footer>
  );
};

/* ---------------------------------------------------------------- */
/* SETTINGS MODAL                                                   */
/* ---------------------------------------------------------------- */
export const Settings = ({ open, onClose, t, setTweak }: { open: boolean, onClose: () => void, t: any, setTweak: (k: string, v: any) => void }) => {
  const [section, setSection] = React.useState("appearance");

  if (!open) return null;

  const sections = [
    { id: "appearance", label: "Appearance", icon: "sparkle" },
    { id: "playback",   label: "Playback",   icon: "play"    },
    { id: "downloads",  label: "Downloads",  icon: "download"},
    { id: "about",      label: "About",      icon: "sliders" },
  ];

  const accents = [
    { id: "violet", color: "oklch(64% 0.20 295)", label: "Violet" },
    { id: "coral",  color: "oklch(67% 0.20 25)",  label: "Coral"  },
    { id: "ocean",  color: "oklch(60% 0.18 230)", label: "Ocean"  },
    { id: "forest", color: "oklch(58% 0.16 155)", label: "Forest" },
  ];

  const warmths = [
    { id: "warm",     label: "Warm",     swatches: ["#FFD7B0","#FFB7C8","#FFE2AE","#F2C3FF"] },
    { id: "balanced", label: "Balanced", swatches: ["#FFC7E6","#FFD89A","#93D9FF","#C7B9FF"] },
    { id: "cool",     label: "Cool",     swatches: ["#C7E2FF","#B6F0E2","#D4C7FF","#A8C9FF"] },
  ];

  return (
    <div className="settings-overlay" onClick={onClose} style={{ position: 'fixed', inset: 0, zIndex: 100, background: 'rgba(20, 12, 50, .28)', backdropFilter: 'blur(8px)', display: 'grid', placeItems: 'center' }}>
      <div className="settings-modal glass-strong" onClick={e => e.stopPropagation()} role="dialog" style={{ width: 'min(860px, calc(100vw - 64px))', height: 'min(620px, calc(100vh - 64px))', borderRadius: '22px', display: 'grid', gridTemplateRows: '56px 1fr', overflow: 'hidden', boxShadow: '0 1px 0 rgba(255,255,255,.7) inset, 0 60px 120px -20px rgba(20,12,50,.55), 0 30px 60px -20px rgba(20,12,50,.4)' }}>
        <header className="settings-head" style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '0 16px 0 22px', borderBottom: '.5px solid rgba(255,255,255,.4)' }}>
          <h2 style={{ margin: 0, fontSize: '15px', fontWeight: 600 }}>Settings</h2>
          <button className="settings-x" onClick={onClose} style={{ width: '28px', height: '28px', borderRadius: '8px', border: 0, background: 'rgba(255,255,255,.4)', color: 'var(--fg-soft)', display: 'grid', placeItems: 'center' }}>
            <Icon name="x" size={16}/>
          </button>
        </header>

        <div className="settings-body" style={{ display: 'grid', gridTemplateColumns: '180px 1fr', minHeight: 0, overflow: 'hidden' }}>
          <nav className="settings-nav" style={{ padding: '14px 10px', borderRight: '.5px solid rgba(255,255,255,.4)', display: 'flex', flexDirection: 'column', gap: '2px', background: 'rgba(255,255,255,.18)' }}>
            {sections.map(s => (
              <button key={s.id}
                className={`settings-nav-item ${section === s.id ? "on" : ""}`}
                onClick={() => setSection(s.id)}
                style={{ display: 'flex', alignItems: 'center', gap: '9px', height: '32px', padding: '0 10px', border: 0, borderRadius: '9px', background: section === s.id ? 'rgba(255,255,255,.78)' : 'transparent', color: section === s.id ? 'var(--fg)' : 'var(--fg-soft)', fontSize: '12.5px', fontWeight: 500, textAlign: 'left' }}>
                <Icon name={s.icon} size={14}/>
                <span>{s.label}</span>
              </button>
            ))}
          </nav>

          <div className="settings-content" style={{ overflowY: 'auto', padding: '22px 28px', display: 'flex', flexDirection: 'column', gap: '24px' }}>
            {section === "appearance" && (
              <>
                <div className="s-group">
                  <div className="s-label">
                    <h3>Glass tone</h3>
                    <p style={{ fontSize: '12px', color: 'var(--fg-soft)' }}>Sets the ambient light tint behind the interface.</p>
                  </div>
                  <div className="s-warmth-row" style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: '10px' }}>
                    {warmths.map(w => (
                      <button key={w.id}
                        className={`s-warmth ${t.warmth === w.id ? "on" : ""}`}
                        onClick={() => setTweak("warmth", w.id)}
                        style={{ padding: '10px', borderRadius: '12px', background: 'rgba(255,255,255,.4)', border: t.warmth === w.id ? '1px solid var(--accent)' : '1px solid transparent', display: 'flex', flexDirection: 'column', gap: '8px', textAlign: 'left' }}>
                        <div className="s-warmth-prev" style={{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: '4px', height: '36px' }}>
                          {w.swatches.map((c, i) =>
                            <span key={i} style={{ background: c, borderRadius: '6px' }}/>
                          )}
                        </div>
                        <div className="s-warmth-label" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', fontSize: '12px', fontWeight: 500 }}>
                          <span>{w.label}</span>
                          {t.warmth === w.id && <Icon name="check" size={12} stroke={2.5}/>}
                        </div>
                      </button>
                    ))}
                  </div>
                </div>

                <div className="s-group">
                  <div className="s-label">
                    <h3>Accent color</h3>
                    <p style={{ fontSize: '12px', color: 'var(--fg-soft)' }}>Controls primary buttons and highlights.</p>
                  </div>
                  <div className="s-accent-row" style={{ display: 'flex', gap: '10px', flexWrap: 'wrap' }}>
                    {accents.map(a => (
                      <button key={a.id}
                        className={`s-accent ${t.accent === a.id ? "on" : ""}`}
                        onClick={() => setTweak("accent", a.id)}
                        style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '6px', padding: '6px', border: t.accent === a.id ? '1px solid var(--accent)' : '1px solid transparent', borderRadius: '12px', background: 'transparent' }}>
                        <span className="s-accent-dot" style={{ width: '36px', height: '36px', borderRadius: '50%', background: a.color, display: 'grid', placeItems: 'center', color: '#fff' }}>
                          {t.accent === a.id && <Icon name="check" size={14} stroke={3}/>}
                        </span>
                        <span className="s-accent-name" style={{ fontSize: '11.5px', color: t.accent === a.id ? 'var(--fg)' : 'var(--fg-soft)', fontWeight: 500 }}>{a.label}</span>
                      </button>
                    ))}
                  </div>
                </div>
              </>
            )}

            {section === "about" && (
              <div className="s-empty" style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', textAlign: 'center', padding: '40px 20px', color: 'var(--fg-soft)' }}>
                <Icon name="sparkle" size={28}/>
                <h3 style={{ margin: '12px 0 4px', fontSize: '14px', color: 'var(--fg)' }}>Melody 1.0.0</h3>
                <p style={{ margin: 0, fontSize: '12.5px' }}>An online + offline music player.</p>
                <p style={{ fontSize: '11.5px', marginTop: '8px', color: "var(--fg-mute)" }}>Made with care · © 2026</p>
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
};
