// Music player views — Search, Playlists, Downloads, NowPlaying bar

const { useState: useStateV, useEffect: useEffectV, useRef: useRefV } = React;

/* ---------------------------------------------------------------- */
/* SEARCH                                                           */
/* ---------------------------------------------------------------- */
const Search = ({ onPlay, currentId, playing, onToggleDl }) => {
  const [q, setQ] = useStateV("paper");
  const [filter, setFilter] = useStateV("all");

  const lower = q.toLowerCase();
  const matched = q
    ? TRACKS.filter(t =>
        t.title.toLowerCase().includes(lower) ||
        t.artist.toLowerCase().includes(lower) ||
        t.album.toLowerCase().includes(lower))
    : [];

  const top = matched[0];

  const recent = ["mira okafor", "soft focus", "ferry light", "165 bpm", "marigold static"];
  const genres = [
    { name: "Ambient",     hue: "g1" },
    { name: "Indie Folk",  hue: "g2" },
    { name: "Jazz & Bossa",hue: "g3" },
    { name: "Electronic",  hue: "g4" },
    { name: "Lo-fi",       hue: "g5" },
    { name: "Soundtracks", hue: "g6" },
    { name: "Classical",   hue: "g7" },
    { name: "City Pop",    hue: "g8" },
  ];

  return (
    <div className="view">
      <div className="search-bar glass-inner">
        <Icon name="search" size={18}/>
        <input
          autoFocus
          value={q}
          onChange={e => setQ(e.target.value)}
          placeholder="Search songs, artists, albums, lyrics…"/>
        {q && <button className="search-clear" onClick={() => setQ("")}><Icon name="x" size={14}/></button>}
        <span className="kbd">⌘ K</span>
      </div>

      {q && (
        <div className="seg seg-pad">
          {["all","songs","artists","albums","playlists","lyrics"].map(f =>
            <button key={f} className={filter === f ? "on" : ""} onClick={() => setFilter(f)}>
              {f[0].toUpperCase() + f.slice(1)}
            </button>
          )}
        </div>
      )}

      {!q && (
        <>
          <section className="search-section">
            <h3>Recent searches</h3>
            <div className="chip-row">
              {recent.map(r => (
                <button key={r} className="chip glass-soft" onClick={() => setQ(r)}>
                  <Icon name="search" size={12}/>
                  <span>{r}</span>
                  <Icon name="x" size={12}/>
                </button>
              ))}
            </div>
          </section>

          <section className="search-section">
            <h3>Browse by mood & genre</h3>
            <div className="genre-grid">
              {genres.map(g => (
                <div key={g.name} className="genre-tile">
                  <Cover seed={g.hue} size={130} radius={14}/>
                  <div className="genre-name">{g.name}</div>
                </div>
              ))}
            </div>
          </section>
        </>
      )}

      {q && top && (
        <div className="search-results">
          <div className="search-grid">
            <section className="top-result glass-soft">
              <div className="section-head"><h3>Top result</h3></div>
              <div className="top-card" onDoubleClick={() => onPlay(top.id)}>
                <Cover seed={top.title[0] + top.id} size={120} radius={14}/>
                <div className="top-meta">
                  <div className="top-tag">Song</div>
                  <div className="top-title">{top.title}</div>
                  <div className="top-sub">{top.artist} · {top.album}</div>
                </div>
                <button className="big-play" onClick={() => onPlay(top.id)}>
                  <Icon name={currentId === top.id && playing ? "pause" : "play"} size={20}/>
                </button>
              </div>
            </section>

            <section className="search-songs">
              <div className="section-head"><h3>Songs</h3><a className="link">Show all</a></div>
              <div className="mini-list glass-soft">
                {matched.slice(0, 4).map(t => (
                  <div key={t.id} className="mini-row" onDoubleClick={() => onPlay(t.id)}>
                    <Cover seed={t.title[0] + t.id} size={36} radius={6}/>
                    <div className="mini-meta">
                      <div className="mini-title">{t.title}</div>
                      <div className="mini-sub">{t.artist}</div>
                    </div>
                    <button
                      className={`dl-mini ${t.downloaded ? "done" : ""}`}
                      onClick={() => onToggleDl(t.id)}>
                      <Icon name={t.downloaded ? "downloaded" : "download"} size={14}/>
                    </button>
                    <div className="mini-dur">{t.dur}</div>
                  </div>
                ))}
              </div>
            </section>
          </div>

          <section className="search-section">
            <div className="section-head"><h3>Albums</h3><a className="link">Show all</a></div>
            <AlbumGrid tracks={matched} onPlay={onPlay}/>
          </section>
        </div>
      )}

      {q && !top && (
        <div className="empty glass-soft">
          <Icon name="search" size={24}/>
          <h3>No matches for “{q}”</h3>
          <p>Try a different artist, song, or check your spelling.</p>
        </div>
      )}
    </div>
  );
};

/* ---------------------------------------------------------------- */
/* PLAYLISTS                                                        */
/* ---------------------------------------------------------------- */
const Playlists = ({ onPlay, currentId, playing, onToggleDl }) => {
  const [openId, setOpenId] = useStateV("p1");
  const playlist = PLAYLISTS.find(p => p.id === openId);
  const tracks = TRACKS.slice(0, 8); // mock contents

  return (
    <div className="view view-split">
      <aside className="playlists-list glass-inner">
        <div className="pl-head">
          <h3>Your playlists</h3>
          <button className="ghost-btn"><Icon name="plus" size={14} stroke={2.2}/><span>New</span></button>
        </div>
        <div className="pl-rows">
          {PLAYLISTS.map(p => (
            <button key={p.id} className={`pl-row ${openId === p.id ? "on" : ""}`} onClick={() => setOpenId(p.id)}>
              <Cover seed={p.hue} size={42} radius={8}/>
              <div className="pl-meta">
                <div className="pl-name">{p.name} {p.pinned && <Icon name="pin" size={11} className="pin-inline"/>}</div>
                <div className="pl-sub">{p.count} songs · You</div>
              </div>
            </button>
          ))}
        </div>
      </aside>

      <section className="playlist-detail">
        <div className="pl-hero glass-hero">
          <Cover seed={playlist.hue} size={160} radius={18}/>
          <div className="pl-hero-text">
            <div className="eyebrow">Playlist · You</div>
            <h1>{playlist.name}</h1>
            <p>A rotating set for the long quiet drives. Updated weekly.</p>
            <div className="pl-stats">
              <span>{playlist.count} songs</span>
              <span className="dot">·</span>
              <span>2 hr 14 min</span>
              <span className="dot">·</span>
              <span>Last updated 3 days ago</span>
            </div>
            <div className="pl-actions">
              <button className="primary-btn" onClick={() => onPlay(tracks[0].id)}>
                <Icon name="play" size={16}/><span>Play</span>
              </button>
              <button className="ghost-btn"><Icon name="shuffle" size={16} stroke={2}/><span>Shuffle</span></button>
              <button className="ghost-btn"><Icon name="download" size={16} stroke={2}/><span>Download all</span></button>
              <button className="ghost-btn icon-only"><Icon name="dots" size={16}/></button>
            </div>
          </div>
        </div>

        <TrackTable tracks={tracks} onPlay={onPlay} currentId={currentId} playing={playing} onToggleDl={onToggleDl}/>
      </section>
    </div>
  );
};

/* ---------------------------------------------------------------- */
/* DOWNLOADS                                                        */
/* ---------------------------------------------------------------- */
const Downloads = ({ onPlay, currentId, playing, onToggleDl }) => {
  const downloaded = TRACKS.filter(t => t.downloaded);
  const queue = [
    { id: 101, title: "Quiet Architecture", artist: "Sable Court", size: "8.2 MB", progress: 0.62, eta: "12s" },
    { id: 102, title: "Citrus Sundown",     artist: "Marlow Reed", size: "6.4 MB", progress: 0.18, eta: "48s" },
  ];

  const used = 2.4, total = 8.0;
  const pct = (used / total) * 100;

  return (
    <div className="view">
      <header className="hero glass-hero dl-hero">
        <div className="hero-text">
          <div className="eyebrow"><Icon name="download" size={12} stroke={2.2}/><span>Offline</span></div>
          <h1>Downloads</h1>
          <p>Saved on this {/* device-agnostic */} computer · plays without an internet connection.</p>
          <div className="dl-stats">
            <div className="dl-stat">
              <div className="dl-num">{downloaded.length}</div>
              <div className="dl-lbl">Songs</div>
            </div>
            <div className="dl-stat">
              <div className="dl-num">{used.toFixed(1)} <span className="unit">GB</span></div>
              <div className="dl-lbl">Used</div>
            </div>
            <div className="dl-stat">
              <div className="dl-num">{(total - used).toFixed(1)} <span className="unit">GB</span></div>
              <div className="dl-lbl">Free</div>
            </div>
          </div>
          <div className="storage-bar wide">
            <div className="storage-fill" style={{ width: `${pct}%` }}/>
          </div>
        </div>
        <div className="dl-hero-right">
          <button className="ghost-btn"><Icon name="sliders" size={14} stroke={2}/><span>Quality · HQ 320</span></button>
          <button className="ghost-btn"><Icon name="cloud" size={14} stroke={2}/><span>Auto-download</span></button>
          <button className="ghost-btn danger"><Icon name="x" size={14} stroke={2}/><span>Clear all</span></button>
        </div>
      </header>

      {queue.length > 0 && (
        <section className="dl-section">
          <div className="section-head">
            <h3>Downloading <span className="count-pill">{queue.length}</span></h3>
            <a className="link">Pause all</a>
          </div>
          <div className="dl-queue glass-soft">
            {queue.map(q => (
              <div key={q.id} className="dl-row downloading">
                <Cover seed={q.title[0] + q.id} size={40} radius={7}/>
                <div className="dl-meta">
                  <div className="dl-title">{q.title}</div>
                  <div className="dl-sub">{q.artist}</div>
                  <div className="dl-progress-line">
                    <div className="dl-progress"><div className="dl-progress-fill" style={{ width: `${q.progress * 100}%` }}/></div>
                    <span className="dl-eta">{Math.round(q.progress * 100)}% · {q.eta}</span>
                  </div>
                </div>
                <div className="dl-size">{q.size}</div>
                <button className="ico-btn small"><Icon name="pause2" size={14}/></button>
                <button className="ico-btn small"><Icon name="x" size={14}/></button>
              </div>
            ))}
          </div>
        </section>
      )}

      <section className="dl-section">
        <div className="section-head">
          <h3>Available offline <span className="count-pill">{downloaded.length}</span></h3>
          <div className="seg seg-sm">
            <button className="on">Recent</button>
            <button>A–Z</button>
            <button>By artist</button>
          </div>
        </div>
        <TrackTable tracks={downloaded} onPlay={onPlay} currentId={currentId} playing={playing} onToggleDl={onToggleDl}/>
      </section>
    </div>
  );
};

/* ---------------------------------------------------------------- */
/* NOW PLAYING BAR                                                  */
/* ---------------------------------------------------------------- */
const NowPlaying = ({ track, playing, onPlayPause, onPrev, onNext, onToggleDl, progress, setProgress, volume, setVolume, onOpenQueue }) => {
  if (!track) return null;
  const total = (() => {
    const [m, s] = track.dur.split(":").map(Number);
    return m * 60 + s;
  })();
  const cur = Math.floor(progress * total);
  const fmt = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;

  return (
    <footer className="np glass-strong">
      <div className="np-left">
        <Cover seed={track.title[0] + track.id} size={52} radius={10}/>
        <div className="np-meta">
          <div className="np-title">{track.title}</div>
          <div className="np-artist">{track.artist}</div>
        </div>
        <button className={`np-heart ${track.liked ? "on" : ""}`}>
          <Icon name={track.liked ? "heartF" : "heart"} size={16}/>
        </button>
        <button
          className={`np-dl ${track.downloaded ? "done" : ""}`}
          onClick={() => onToggleDl(track.id)}
          title={track.downloaded ? "Saved offline" : "Download for offline"}>
          <Icon name={track.downloaded ? "downloaded" : "download"} size={16} stroke={1.9}/>
          <span>{track.downloaded ? "Saved" : "Download"}</span>
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
          <span className="np-time">{fmt(cur)}</span>
          <div className="np-bar" onClick={e => {
            const r = e.currentTarget.getBoundingClientRect();
            setProgress(Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)));
          }}>
            <div className="np-bar-fill" style={{ width: `${progress * 100}%` }}/>
            <div className="np-bar-thumb" style={{ left: `${progress * 100}%` }}/>
          </div>
          <span className="np-time">{track.dur}</span>
        </div>
      </div>

      <div className="np-right">
        <button className="np-ctl"><Icon name="sparkle" size={16} stroke={1.8}/></button>
        <button className="np-ctl" onClick={onOpenQueue}><Icon name="queue" size={16} stroke={1.8}/></button>
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

Object.assign(window, { Search, Playlists, Downloads, NowPlaying });
