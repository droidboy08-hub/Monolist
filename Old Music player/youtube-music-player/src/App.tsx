import React, { useState, useRef, useEffect } from 'react';
import { downloadAndSaveSong, getAllDownloadedSongs, deleteDownloadedSong, getAudioBase64 } from './services/downloadService';
import { resolveStreamUrl, isElectron } from './services/streamService';
import { WindowChrome, Sidebar, NowPlaying, Settings } from './components/LumenUI';
import type { GenericSong } from './components/LumenUI';
import { LibraryView, SearchView, DownloadsView } from './components/LumenViews';

declare global {
  interface Window {
    onYouTubeIframeAPIReady: () => void;
    YT: any;
  }
}

const ACCENT_MAP: any = {
  violet: { a: "oklch(64% 0.20 295)", b: "oklch(72% 0.18 200)", warm: "oklch(78% 0.18 55)" },
  coral:  { a: "oklch(67% 0.20 25)",  b: "oklch(72% 0.18 60)",  warm: "oklch(74% 0.16 340)" },
  ocean:  { a: "oklch(60% 0.18 230)", b: "oklch(68% 0.16 195)", warm: "oklch(74% 0.16 160)" },
  forest: { a: "oklch(58% 0.16 155)", b: "oklch(66% 0.16 110)", warm: "oklch(74% 0.16 80)"  },
};

const WARMTH_MAP: any = {
  warm: ["#FFD7B0", "#FFB7C8", "#FFE2AE", "#F2C3FF", "#FFEAC4"],
  balanced: ["#FFC7E6", "#FFD89A", "#93D9FF", "#C7B9FF", "#FFE08C"],
  cool: ["#C7E2FF", "#B6F0E2", "#D4C7FF", "#A8C9FF", "#E5F0FF"],
};

function App() {
  // --- States ---
  const [query, setQuery] = useState('');
  const [songs, setSongs] = useState<GenericSong[]>([]);
  const [searching, setSearching] = useState(false);
  const [currentSong, setCurrentSong] = useState<GenericSong | null>(null);
  const [streamUrl, setStreamUrl] = useState<string | null>(null);
  const [, setStatus] = useState('');
  const [protocol, setProtocol] = useState<'NONE' | 'STEALTH' | 'HYBRID' | 'LOCAL'>('NONE');
  const [isPlaying, setIsPlaying] = useState(false);
  
  const [downloadedSongs, setDownloadedSongs] = useState<GenericSong[]>([]);
  const [downloadProgress, setDownloadProgress] = useState<Record<string, number>>({});
  const [view, setView] = useState('library');
  const [settingsOpen, setSettingsOpen] = useState(false);

  const [progress, setProgress] = useState(0);
  const [durationSec, setDurationSec] = useState(0);
  const [currentPosSec, setCurrentPosSec] = useState(0);
  const [repeatMode] = useState<0 | 1 | 2>(0);
  const [volume, setVolume] = useState(0.8);

  // --- Theme State ---
  const [theme, setTheme] = useState({
    accent: 'violet',
    warmth: 'balanced',
    platform: 'win' as 'win' | 'mac'
  });

  const audioRef = useRef<HTMLAudioElement>(null);
  const ytPlayerRef = useRef<any>(null);
  const progressInterval = useRef<any>(null);

  // --- Theme Effect ---
  useEffect(() => {
    const accents = ACCENT_MAP[theme.accent];
    const colors = WARMTH_MAP[theme.warmth];
    
    document.documentElement.style.setProperty('--accent', accents.a);
    document.documentElement.style.setProperty('--accent-2', accents.b);
    document.documentElement.style.setProperty('--accent-warm', accents.warm);

    const [a, b, c, d, e] = colors;
    document.body.style.background = `
      radial-gradient(120% 80% at 0% 0%,   ${a} 0%, transparent 55%),
      radial-gradient(90% 70% at 100% 0%,  ${b} 0%, transparent 50%),
      radial-gradient(120% 90% at 100% 100%, ${c} 0%, transparent 55%),
      radial-gradient(110% 90% at 0% 100%, ${d} 0%, transparent 55%),
      linear-gradient(180deg, ${e}, ${d})
    `;
  }, [theme]);

  // --- Initial Setup ---
  useEffect(() => {
    if (!window.YT) {
        const tag = document.createElement('script');
        tag.src = "https://www.youtube.com/iframe_api";
        const firstScriptTag = document.getElementsByTagName('script')[0];
        if (firstScriptTag && firstScriptTag.parentNode) {
            firstScriptTag.parentNode.insertBefore(tag, firstScriptTag);
        } else {
            document.head.appendChild(tag);
        }
    }
    loadDownloadedSongs();
  }, []);

  const loadDownloadedSongs = async () => {
    try {
        const offline = await getAllDownloadedSongs();
        setDownloadedSongs(offline || []);
    } catch(e) {}
  };

  // --- Simplified Progress Tracker ---
  useEffect(() => {
    if (isPlaying) {
      progressInterval.current = setInterval(() => {
        if ((protocol === 'STEALTH' || protocol === 'LOCAL') && audioRef.current) {
          const cur = audioRef.current.currentTime;
          const dur = audioRef.current.duration;
          if (dur) {
            setCurrentPosSec(cur);
            setDurationSec(dur);
            setProgress((cur / dur) * 100);
          }
        } else if (protocol === 'HYBRID' && ytPlayerRef.current && ytPlayerRef.current.getCurrentTime) {
          try {
            const cur = ytPlayerRef.current.getCurrentTime();
            const dur = ytPlayerRef.current.getDuration();
            if (dur) {
              setCurrentPosSec(cur);
              setDurationSec(dur);
              setProgress((cur / dur) * 100);
            }
          } catch(e) {}
        }
      }, 1000);
    } else {
      clearInterval(progressInterval.current);
    }
    return () => clearInterval(progressInterval.current);
  }, [isPlaying, protocol]);

  // --- Logic ---
  const handleSearch = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!query) return;
    setSearching(true);
    setSongs([]);
    setStatus('Syncing Queue...');
    setView('search');

    try {
      const ytUrl = `https://www.youtube.com/results?search_query=${encodeURIComponent(query)}&sp=EgIQAQ%253D%253D`;
      const fetchUrl = isElectron()
        ? ytUrl
        : `https://api.codetabs.com/v1/proxy?quest=${encodeURIComponent(ytUrl)}`;
      const res = await fetch(fetchUrl);
      const html = await res.text();
      const startTag = 'var ytInitialData = ';
      const startPos = html.indexOf(startTag);
      if (startPos === -1) throw new Error('fail');
      const jsonStr = html.substring(startPos + startTag.length, html.indexOf(';</script>', startPos));
      const data = JSON.parse(jsonStr);
      const contents = data.contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer.contents[0].itemSectionRenderer.contents;
      
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
      setStatus('');
    } catch (err) {
      setStatus('Network Delay.');
    } finally {
      setSearching(false);
    }
  };

  const initHybridPlayer = (videoId: string) => {
      if (!window.YT || !window.YT.Player) {
          setStatus('Player Engine Loading...');
          setTimeout(() => initHybridPlayer(videoId), 1000);
          return;
      }
      if (ytPlayerRef.current) { try { ytPlayerRef.current.destroy(); } catch(e) {} }
      ytPlayerRef.current = new window.YT.Player('ghost-player', {
          height: '1', width: '1', videoId: videoId,
          host: 'https://www.youtube.com',
          playerVars: { 
            'autoplay': 1, 
            'controls': 0, 
            'modestbranding': 1, 
            'rel': 0,
            'origin': window.location.origin || 'http://localhost:5173'
          },
          events: {
              'onReady': (event: any) => { 
                  event.target.playVideo(); 
                  setIsPlaying(true);
                  setStatus('');
              },
              'onStateChange': (event: any) => {
                  if (event.data === window.YT.PlayerState.PLAYING) {
                      setIsPlaying(true);
                      setStatus('');
                  }
                  if (event.data === window.YT.PlayerState.PAUSED) setIsPlaying(false);
                  if (event.data === window.YT.PlayerState.ENDED) handleNext();
              },
              'onError': (event: any) => {
                  console.error('YouTube Player Error:', event.data);
                  setStatus('Playback Restricted.');
                  setIsPlaying(false);
              }
          }
      });
  };

  const playSong = async (song: GenericSong) => {
    // --- Clean Up Previous State ---
    if (ytPlayerRef.current) {
        try { ytPlayerRef.current.stopVideo(); ytPlayerRef.current.destroy(); ytPlayerRef.current = null; } catch(e) {}
    }
    if (audioRef.current) {
        audioRef.current.pause();
        audioRef.current.src = "";
    }
    
    setCurrentSong(song);
    setProtocol('NONE');
    setIsPlaying(false);
    setProgress(0);
    setStreamUrl(null);
    setStatus('Syncing...');

    const local = downloadedSongs.find(s => s.id === song.id);
    if (local) {
        setStatus('Offline Playback');
        try {
            const audioBase64 = await getAudioBase64(song.id);
            if (!audioBase64) {
                console.warn('Audio data not found in storage, trying stream...');
            } else {
                const byteCharacters = atob(audioBase64);
                const byteNumbers = new Uint8Array(byteCharacters.length);
                for (let i = 0; i < byteCharacters.length; i++) {
                    byteNumbers[i] = byteCharacters.charCodeAt(i);
                }
                const blob = new Blob([byteNumbers], { type: 'audio/mp4' });
                const blobUrl = URL.createObjectURL(blob);
                setStreamUrl(blobUrl);
                setProtocol('LOCAL');
                setIsPlaying(true);
                setStatus('');
                return;
            }
        } catch (e) {
            console.error('Local Playback Error:', e);
        }
    }

    setStatus('Optimizing Link...');
    const resolvedUrl = await resolveStreamUrl(song.id);

    if (resolvedUrl) {
        setStreamUrl(resolvedUrl);
        setProtocol('STEALTH');
        setIsPlaying(true);
        setStatus('');
        console.log('Playing via STEALTH:', resolvedUrl);
    } else {
      setProtocol('HYBRID');
      setStatus('Fallback Engine...');
      console.log('Stealth resolution failed, falling back to HYBRID');
      setTimeout(() => initHybridPlayer(song.id), 500);
    }
  };

  const handleToggleDownload = async (song: GenericSong) => {
    if (downloadedSongs.some(s => s.id === song.id)) {
        await deleteDownloadedSong(song.id);
        await loadDownloadedSongs();
        return;
    }
    setStatus(`Saving...`);
    try {
      await downloadAndSaveSong(song.id, song.title, song.artist, song.thumbnail, song.duration, (p) => {
          setDownloadProgress(prev => ({...prev, [song.id]: p}));
      });
      await loadDownloadedSongs();
      setStatus('');
      setDownloadProgress(prev => {
          const next = {...prev};
          delete next[song.id];
          return next;
      });
    } catch (e) {
      setStatus('Save Restricted.');
    }
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

  const handleNext = () => {
      const list = view === 'search' ? songs : downloadedSongs;
      const index = list.findIndex(s => s.id === currentSong?.id);
      if (index !== -1 && index < list.length - 1) playSong(list[index + 1]);
      else if (index !== -1 && repeatMode === 1) playSong(list[0]);
  };

  const handlePrev = () => {
      const list = view === 'search' ? songs : downloadedSongs;
      const index = list.findIndex(s => s.id === currentSong?.id);
      if (index > 0) playSong(list[index - 1]);
  };

  const handleSeek = (val: number) => {
      setProgress(val);
      const pos = (val / 100) * durationSec;
      if (protocol === 'STEALTH' || protocol === 'LOCAL') {
          if (audioRef.current) audioRef.current.currentTime = pos;
      } else if (protocol === 'HYBRID' && ytPlayerRef.current) {
          ytPlayerRef.current.seekTo(pos, true);
      }
  };

  const formatTime = (sec: number) => {
      if (isNaN(sec)) return "0:00";
      const m = Math.floor(sec / 60);
      const s = Math.floor(sec % 60);
      return `${m}:${s < 10 ? '0' : ''}${s}`;
  };

  useEffect(() => {
    if (audioRef.current) audioRef.current.volume = volume;
  }, [volume]);

  return (
    <div className="app-shell">
      <div className="bg-orbs" aria-hidden="true">
        <div className="orb o1"></div>
        <div className="orb o2"></div>
        <div className="orb o3"></div>
        <div className="orb o4"></div>
        <div className="orb o5"></div>
      </div>

      <div className="window">
        <WindowChrome 
            platform="win" 
            view={view} 
            onNav={setView} 
            onOpenSettings={() => {}} 
        />

        <div className="body">
          <Sidebar 
            view={view} 
            onNav={setView} 
            online={protocol !== 'LOCAL'} 
            downloadedCount={downloadedSongs.length} 
          />

          <main className="main-pane">
            {view === 'library' && (
                <LibraryView 
                    onPlay={playSong}
                    currentId={currentSong?.id}
                    playing={isPlaying}
                    onToggleDl={handleToggleDownload}
                    downloadedSongs={downloadedSongs}
                    downloadProgress={downloadProgress}
                />
            )}
            {view === 'search' && (
                <SearchView 
                    query={query}
                    setQuery={setQuery}
                    onSearch={handleSearch}
                    results={songs}
                    searching={searching}
                    onPlay={playSong}
                    currentId={currentSong?.id}
                    playing={isPlaying}
                    onToggleDl={handleToggleDownload}
                    downloadedSongs={downloadedSongs.map(s => s.id)}
                    downloadProgress={downloadProgress}
                />
            )}
            {view === 'downloads' && (
                <DownloadsView 
                    downloadedSongs={downloadedSongs}
                    downloadProgress={downloadProgress}
                    onPlay={playSong}
                    currentId={currentSong?.id}
                    playing={isPlaying}
                    onToggleDl={handleToggleDownload}
                />
            )}
            {view === 'playlists' && (
                <div className="view"><h2 className="text-center mt-20 opacity-50">Playlists coming soon...</h2></div>
            )}
          </main>
        </div>

        {currentSong && (
            <NowPlaying 
                track={currentSong}
                playing={isPlaying}
                onPlayPause={togglePlay}
                onPrev={handlePrev}
                onNext={handleNext}
                onToggleDl={handleToggleDownload}
                progress={progress}
                setProgress={handleSeek}
                volume={volume}
                setVolume={setVolume}
                isDownloaded={downloadedSongs.some(s => s.id === currentSong.id)}
                currentTime={formatTime(currentPosSec)}
                duration={formatTime(durationSec) || currentSong.duration}
            />
        )}
      </div>

      <Settings 
        open={settingsOpen} 
        onClose={() => setSettingsOpen(false)} 
        t={theme} 
        setTweak={(k, v) => setTheme(prev => ({...prev, [k]: v}))} 
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
                setStatus('Engine Fallback...');
                setTimeout(() => initHybridPlayer(currentSong!.id), 100);
            }
        }}
        className="hidden" 
      />
      <div id="ghost-player" style={{ position: 'absolute', left: '-9999px', opacity: 0 }}></div>
    </div>
  );
}

export default App;
