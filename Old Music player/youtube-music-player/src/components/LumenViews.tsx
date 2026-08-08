import React from 'react';
import { Icon, Cover, TrackTable } from './LumenUI';
import type { GenericSong } from './LumenUI';

/* ---------------------------------------------------------------- */
/* LIBRARY                                                          */
/* ---------------------------------------------------------------- */
export const LibraryView = ({ 
    onPlay, 
    currentId, 
    playing, 
    onToggleDl, 
    downloadedSongs,
    downloadProgress 
}: { 
    onPlay: (song: GenericSong) => void, 
    currentId?: string, 
    playing: boolean, 
    onToggleDl: (song: GenericSong) => void,
    downloadedSongs: GenericSong[],
    downloadProgress: Record<string, number>
}) => {
  return (
    <div className="view">
      <header className="hero glass-hero">
        <div className="hero-text">
          <div className="eyebrow">Your Library</div>
          <h1>Good afternoon.</h1>
          <p>{downloadedSongs.length} songs · {(downloadedSongs.length * 0.008).toFixed(1)} GB available offline</p>
        </div>
        <div className="hero-stack">
          {downloadedSongs.slice(0, 4).map(s => <Cover key={s.id} url={s.thumbnail} size={68} radius={14}/>)}
        </div>
      </header>

      <div className="row-bar">
        <div className="seg">
            <button className="on">Songs</button>
            <button>Albums</button>
            <button>Artists</button>
        </div>
      </div>

      <TrackTable 
        tracks={downloadedSongs} 
        onPlay={onPlay} 
        currentId={currentId} 
        playing={playing} 
        onToggleDl={onToggleDl}
        downloadedSongs={downloadedSongs.map(s => s.id)}
        downloadProgress={downloadProgress}
      />
    </div>
  );
};

/* ---------------------------------------------------------------- */
/* SEARCH                                                           */
/* ---------------------------------------------------------------- */
export const SearchView = ({ 
    query, 
    setQuery, 
    onSearch, 
    results, 
    searching,
    onPlay, 
    currentId, 
    playing, 
    onToggleDl,
    downloadedSongs,
    downloadProgress
}: { 
    query: string, 
    setQuery: (q: string) => void, 
    onSearch: (e: React.FormEvent) => void,
    results: GenericSong[],
    searching: boolean,
    onPlay: (song: GenericSong) => void, 
    currentId?: string, 
    playing: boolean, 
    onToggleDl: (song: GenericSong) => void,
    downloadedSongs: string[],
    downloadProgress: Record<string, number>
}) => {
  return (
    <div className="view">
      <form onSubmit={onSearch} className="search-bar glass-inner">
        <Icon name="search" size={18}/>
        <input
          autoFocus
          value={query}
          onChange={e => setQuery(e.target.value)}
          placeholder="Search for music on YouTube..."/>
        {query && <button type="button" className="search-clear" onClick={() => setQuery("")}><Icon name="x" size={14}/></button>}
        <span className="kbd">{searching ? '...' : 'ENTER'}</span>
      </form>

      {results.length > 0 && (
        <div className="search-results">
            <div className="section-head"><h3>Search Results</h3></div>
            <TrackTable 
                tracks={results} 
                onPlay={onPlay} 
                currentId={currentId} 
                playing={playing} 
                onToggleDl={onToggleDl}
                downloadedSongs={downloadedSongs}
                downloadProgress={downloadProgress}
            />
        </div>
      )}

      {results.length === 0 && !searching && (
        <div className="empty glass-soft">
          <Icon name="search" size={24}/>
          <h3>Ready to discover</h3>
          <p>Search for your favorite tracks, artists, or albums.</p>
        </div>
      )}
    </div>
  );
};

/* ---------------------------------------------------------------- */
/* DOWNLOADS                                                        */
/* ---------------------------------------------------------------- */
export const DownloadsView = ({ 
    downloadedSongs,
    downloadProgress,
    onPlay,
    currentId,
    playing,
    onToggleDl
}: { 
    downloadedSongs: GenericSong[],
    downloadProgress: Record<string, number>,
    onPlay: (song: GenericSong) => void,
    currentId?: string,
    playing: boolean,
    onToggleDl: (song: GenericSong) => void
}) => {
  const downloading = Object.entries(downloadProgress).map(([id, progress]) => {
      // Find title from somewhere? Maybe we need to pass a list of active downloads
      return { id, progress };
  });

  return (
    <div className="view">
      <header className="hero glass-hero">
        <div className="hero-text">
          <div className="eyebrow"><Icon name="download" size={12} stroke={2.2}/><span>Offline</span></div>
          <h1>Downloads</h1>
          <p>Saved on this computer · plays without an internet connection.</p>
        </div>
      </header>

      {downloading.length > 0 && (
          <section className="dl-section">
              <div className="section-head"><h3>Downloading</h3></div>
              <div className="dl-queue glass-soft">
                  {downloading.map(dl => (
                      <div key={dl.id} className="dl-row">
                          <div className="dl-meta">
                              <div className="dl-title">Song ID: {dl.id}</div>
                              <div className="dl-progress-line">
                                  <div className="dl-progress"><div className="dl-progress-fill" style={{ width: `${dl.progress}%` }}/></div>
                                  <span className="dl-eta">{Math.round(dl.progress)}%</span>
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
        <TrackTable 
            tracks={downloadedSongs} 
            onPlay={onPlay} 
            currentId={currentId} 
            playing={playing} 
            onToggleDl={onToggleDl}
            downloadedSongs={downloadedSongs.map(s => s.id)}
            downloadProgress={downloadProgress}
        />
      </section>
    </div>
  );
};
