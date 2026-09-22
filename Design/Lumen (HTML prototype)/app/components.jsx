// Music player components — Liquid Glass design language

const { useState, useEffect, useRef, useMemo } = React;

/* ---------------------------------------------------------------- */
/* ICONS — minimal, original line glyphs                            */
/* ---------------------------------------------------------------- */
const Icon = ({ name, size = 18, stroke = 1.6, ...rest }) => {
  const props = {
    width: size, height: size, viewBox: "0 0 24 24",
    fill: "none", stroke: "currentColor",
    strokeWidth: stroke, strokeLinecap: "round", strokeLinejoin: "round",
    ...rest,
  };
  const paths = {
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
/* COVER ART — generated, colorful, no real album imagery           */
/* ---------------------------------------------------------------- */
const Cover = ({ seed = "a", size = 56, radius = 10 }) => {
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
/* DATA                                                             */
/* ---------------------------------------------------------------- */
const TRACKS = [
  { id: 1,  title: "Paper Lanterns",        artist: "Mira Okafor",       album: "Slow Rooms",         dur: "3:24", plays: "2.1M", downloaded: true,  liked: true  },
  { id: 2,  title: "Northbound at Dusk",    artist: "The Halcyon Field", album: "Ferry Light",        dur: "4:12", plays: "874K", downloaded: false, liked: false },
  { id: 3,  title: "Velvet Static",         artist: "Aerie Wren",        album: "Vapor / Bloom",      dur: "2:58", plays: "5.4M", downloaded: true,  liked: true  },
  { id: 4,  title: "Half-Light Hymn",       artist: "Sable Court",       album: "Soft Geometry",      dur: "5:01", plays: "412K", downloaded: false, liked: false },
  { id: 5,  title: "Coastal Drift",         artist: "Nori Tanaka",       album: "Sea Glass",          dur: "3:46", plays: "1.8M", downloaded: true,  liked: false },
  { id: 6,  title: "Citrus Sundown",        artist: "Marlow Reed",       album: "Patio Tapes",        dur: "3:11", plays: "623K", downloaded: false, liked: true  },
  { id: 7,  title: "Glass Engines",         artist: "Atrium 04",         album: "Vapor / Bloom",      dur: "4:33", plays: "988K", downloaded: false, liked: false },
  { id: 8,  title: "Telegraph Bloom",       artist: "June Albright",     album: "Pale Signal",        dur: "3:55", plays: "302K", downloaded: true,  liked: false },
  { id: 9,  title: "Soft Industry",         artist: "Kestrel & Lune",    album: "Soft Geometry",      dur: "4:08", plays: "1.2M", downloaded: false, liked: true  },
  { id: 10, title: "Postcards from Linden", artist: "The Halcyon Field", album: "Ferry Light",        dur: "3:29", plays: "545K", downloaded: false, liked: false },
  { id: 11, title: "Marigold Static",       artist: "Aerie Wren",        album: "Vapor / Bloom",      dur: "2:47", plays: "3.0M", downloaded: true,  liked: true  },
  { id: 12, title: "Quiet Architecture",    artist: "Sable Court",       album: "Soft Geometry",      dur: "5:32", plays: "188K", downloaded: false, liked: false },
];

const PLAYLISTS = [
  { id: "p1", name: "Late Night Drive",    count: 38, hue: "p1", pinned: true  },
  { id: "p2", name: "Soft Focus",          count: 64, hue: "p2", pinned: true  },
  { id: "p3", name: "Sunday Coffee",       count: 22, hue: "p3", pinned: false },
  { id: "p4", name: "On Repeat — May",     count: 17, hue: "p4", pinned: false },
  { id: "p5", name: "Run Club / 165 BPM",  count: 41, hue: "p5", pinned: false },
  { id: "p6", name: "Tape Hiss & Reverb",  count: 29, hue: "p6", pinned: false },
];

/* ---------------------------------------------------------------- */
/* WINDOW CHROME                                                    */
/* ---------------------------------------------------------------- */
const WindowChrome = ({ platform, title, onNav, view, onOpenSettings }) => {
  const tabs = [
    { id: "library",  label: "Library",   icon: "library"  },
    { id: "search",   label: "Search",    icon: "search"   },
    { id: "playlists",label: "Playlists", icon: "playlist" },
    { id: "downloads",label: "Downloads", icon: "download" },
  ];
  return (
    <div className={`chrome chrome-${platform}`}>
      <div className="chrome-l">
        {platform === "mac" ? (
          <div className="traffic">
            <span className="t-close"/><span className="t-min"/><span className="t-max"/>
          </div>
        ) : null}
        <div className="chrome-nav">
          <button className="chrome-arrow"><Icon name="chevron" size={14} stroke={2} style={{ transform: "rotate(180deg)" }}/></button>
          <button className="chrome-arrow"><Icon name="chevron" size={14} stroke={2}/></button>
        </div>
      </div>

      <div className="chrome-tabs">
        {tabs.map(t => (
          <button key={t.id}
            className={`chrome-tab ${view === t.id ? "active" : ""}`}
            onClick={() => onNav(t.id)}>
            <Icon name={t.icon} size={14} stroke={1.8}/>
            <span>{t.label}</span>
          </button>
        ))}
      </div>

      <div className="chrome-r">
        <button className="chrome-arrow" title="Now playing"><Icon name="eq" size={14} stroke={1.8}/></button>
        <button className="chrome-arrow" title="Settings" onClick={onOpenSettings}><Icon name="sliders" size={14} stroke={1.8}/></button>
        {platform === "win" ? (
          <div className="win-controls">
            <span className="w-min">—</span>
            <span className="w-max">▢</span>
            <span className="w-close">✕</span>
          </div>
        ) : null}
      </div>
    </div>
  );
};

/* ---------------------------------------------------------------- */
/* SIDEBAR                                                          */
/* ---------------------------------------------------------------- */
const Sidebar = ({ view, onNav, online }) => {
  const items = [
    { id: "library",   label: "Library",   icon: "library",  count: 412 },
    { id: "search",    label: "Search",    icon: "search"   },
    { id: "playlists", label: "Playlists", icon: "playlist", count: 14  },
    { id: "downloads", label: "Downloads", icon: "download", count: 38, badge: "2" },
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

      <div className="side-section">
        <div className="side-title">
          <span>Pinned</span>
          <button className="side-add" title="New playlist"><Icon name="plus" size={12} stroke={2.4}/></button>
        </div>
        {PLAYLISTS.filter(p => p.pinned).map(p => (
          <button key={p.id} className="side-item side-playlist" onClick={() => onNav("playlists", p.id)}>
            <Cover seed={p.hue} size={20} radius={5}/>
            <span className="side-label">{p.name}</span>
            <span className="side-count">{p.count}</span>
          </button>
        ))}
        {PLAYLISTS.filter(p => !p.pinned).slice(0, 3).map(p => (
          <button key={p.id} className="side-item side-playlist" onClick={() => onNav("playlists", p.id)}>
            <Cover seed={p.hue} size={20} radius={5}/>
            <span className="side-label">{p.name}</span>
            <span className="side-count">{p.count}</span>
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
            <span>2.4 / 8 GB</span>
          </div>
          <div className="storage-bar"><div className="storage-fill" style={{ width: "30%" }}/></div>
        </div>
      </div>
    </aside>
  );
};

/* ---------------------------------------------------------------- */
/* LIBRARY                                                          */
/* ---------------------------------------------------------------- */
const Library = ({ onPlay, currentId, playing, onToggleDl }) => {
  const [tab, setTab] = useState("songs");
  const [layout, setLayout] = useState("list");

  return (
    <div className="view">
      <header className="hero glass-hero">
        <div className="hero-text">
          <div className="eyebrow">Your Library</div>
          <h1>Good afternoon, Casey.</h1>
          <p>412 songs · 28 albums · 14 playlists · 2.4 GB available offline</p>
        </div>
        <div className="hero-stack">
          {[7,5,3,1].map(s => <Cover key={s} seed={`H${s}`} size={68} radius={14}/>)}
        </div>
      </header>

      <div className="row-bar">
        <div className="seg">
          {["songs","albums","artists","liked"].map(t =>
            <button key={t} className={tab === t ? "on" : ""} onClick={() => setTab(t)}>
              {t[0].toUpperCase() + t.slice(1)}
            </button>
          )}
        </div>
        <div className="row-bar-r">
          <button className="ico-btn" onClick={() => setLayout(layout === "list" ? "grid" : "list")} title="Toggle layout">
            <Icon name={layout === "list" ? "grid" : "list"} size={16}/>
          </button>
          <button className="ico-btn" title="Sort"><Icon name="sliders" size={16}/></button>
        </div>
      </div>

      {tab === "songs" && (
        layout === "list"
          ? <TrackTable tracks={TRACKS} onPlay={onPlay} currentId={currentId} playing={playing} onToggleDl={onToggleDl}/>
          : <AlbumGrid tracks={TRACKS} onPlay={onPlay}/>
      )}

      {tab === "albums" && <AlbumGrid tracks={TRACKS} onPlay={onPlay}/>}
      {tab === "artists" && <ArtistGrid/>}
      {tab === "liked" && <TrackTable tracks={TRACKS.filter(t => t.liked)} onPlay={onPlay} currentId={currentId} playing={playing} onToggleDl={onToggleDl}/>}
    </div>
  );
};

const TrackTable = ({ tracks, onPlay, currentId, playing, onToggleDl, showAlbum = true }) => (
  <div className="track-table glass-inner">
    <div className="tt-head">
      <div className="tt-i">#</div>
      <div className="tt-t">Title</div>
      {showAlbum && <div className="tt-a">Album</div>}
      <div className="tt-p">Plays</div>
      <div className="tt-d"><Icon name="download" size={14}/></div>
      <div className="tt-dur">Time</div>
      <div className="tt-x"></div>
    </div>
    {tracks.map((t, i) => {
      const isCur = t.id === currentId;
      return (
        <div key={t.id} className={`tt-row ${isCur ? "current" : ""}`} onDoubleClick={() => onPlay(t.id)}>
          <div className="tt-i">
            {isCur && playing
              ? <span className="bars" aria-label="playing"><span/><span/><span/></span>
              : <><span className="num">{i + 1}</span>
                  <button className="play-mini" onClick={() => onPlay(t.id)}>
                    <Icon name={isCur ? "pause" : "play"} size={12}/>
                  </button></>}
          </div>
          <div className="tt-t">
            <Cover seed={t.title[0] + t.id} size={34} radius={6}/>
            <div className="tt-meta">
              <div className="tt-title">{t.title} {t.liked && <Icon name="heartF" size={11} className="heart-inline"/>}</div>
              <div className="tt-artist">{t.artist}</div>
            </div>
          </div>
          {showAlbum && <div className="tt-a">{t.album}</div>}
          <div className="tt-p">{t.plays}</div>
          <div className="tt-d">
            <button
              className={`dl-mini ${t.downloaded ? "done" : ""}`}
              onClick={() => onToggleDl(t.id)}
              title={t.downloaded ? "Downloaded — click to remove" : "Download for offline"}>
              <Icon name={t.downloaded ? "downloaded" : "download"} size={14}/>
            </button>
          </div>
          <div className="tt-dur">{t.dur}</div>
          <div className="tt-x">
            <button className="dots-btn"><Icon name="dots" size={14}/></button>
          </div>
        </div>
      );
    })}
  </div>
);

const AlbumGrid = ({ tracks, onPlay }) => {
  const albums = useMemo(() => {
    const m = {};
    tracks.forEach(t => {
      if (!m[t.album]) m[t.album] = { name: t.album, artist: t.artist, tracks: [], firstId: t.id };
      m[t.album].tracks.push(t);
    });
    return Object.values(m);
  }, [tracks]);
  return (
    <div className="album-grid">
      {albums.map(a => (
        <div key={a.name} className="album-card glass-soft" onDoubleClick={() => onPlay(a.firstId)}>
          <div className="album-art">
            <Cover seed={a.name} size={180} radius={12}/>
            <button className="album-play" onClick={() => onPlay(a.firstId)}><Icon name="play" size={18}/></button>
          </div>
          <div className="album-name">{a.name}</div>
          <div className="album-sub">{a.artist} · {a.tracks.length} tracks</div>
        </div>
      ))}
    </div>
  );
};

const ArtistGrid = () => {
  const artists = ["Mira Okafor","The Halcyon Field","Aerie Wren","Sable Court","Nori Tanaka","Marlow Reed","Atrium 04","June Albright"];
  return (
    <div className="artist-grid">
      {artists.map(a => (
        <div key={a} className="artist-card glass-soft">
          <Cover seed={a} size={120} radius={999}/>
          <div className="artist-name">{a}</div>
          <div className="artist-sub">Artist</div>
        </div>
      ))}
    </div>
  );
};

Object.assign(window, { Icon, Cover, WindowChrome, Sidebar, Library, TrackTable, AlbumGrid, ArtistGrid, TRACKS, PLAYLISTS });
