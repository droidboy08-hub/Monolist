// Settings modal — in-app appearance + playback controls
// Reads/writes tweak state so Tweaks panel & Settings stay in sync.

const Settings = ({ open, onClose, t, setTweak }) => {
  const [section, setSection] = React.useState("appearance");

  if (!open) return null;

  const sections = [
    { id: "appearance", label: "Appearance", icon: "sparkle" },
    { id: "playback",   label: "Playback",   icon: "play"    },
    { id: "downloads",  label: "Downloads",  icon: "download"},
    { id: "account",    label: "Account",    icon: "library" },
    { id: "about",      label: "About",      icon: "sliders" },
  ];

  /* Curated swatches — match ACCENT_MAP keys */
  const accents = [
    { id: "violet", color: "oklch(64% 0.20 295)", label: "Violet" },
    { id: "coral",  color: "oklch(67% 0.20 25)",  label: "Coral"  },
    { id: "ocean",  color: "oklch(60% 0.18 230)", label: "Ocean"  },
    { id: "forest", color: "oklch(58% 0.16 155)", label: "Forest" },
  ];

  /* Curated warmth palettes — match WARMTH_MAP keys */
  const warmths = [
    { id: "warm",     label: "Warm",     swatches: ["#FFD7B0","#FFB7C8","#FFE2AE","#F2C3FF"] },
    { id: "balanced", label: "Balanced", swatches: ["#FFC7E6","#FFD89A","#93D9FF","#C7B9FF"] },
    { id: "cool",     label: "Cool",     swatches: ["#C7E2FF","#B6F0E2","#D4C7FF","#A8C9FF"] },
  ];

  return (
    <div className="settings-overlay" onClick={onClose}>
      <div className="settings-modal glass-strong" onClick={e => e.stopPropagation()} role="dialog" aria-label="Settings">
        <header className="settings-head">
          <h2>Settings</h2>
          <button className="settings-x" onClick={onClose} aria-label="Close">
            <Icon name="x" size={16}/>
          </button>
        </header>

        <div className="settings-body">
          <nav className="settings-nav">
            {sections.map(s => (
              <button key={s.id}
                className={`settings-nav-item ${section === s.id ? "on" : ""}`}
                onClick={() => setSection(s.id)}>
                <Icon name={s.icon} size={14}/>
                <span>{s.label}</span>
              </button>
            ))}
          </nav>

          <div className="settings-content">
            {section === "appearance" && (
              <>
                <div className="s-group">
                  <div className="s-label">
                    <h3>Glass tone</h3>
                    <p>Sets the ambient light tint behind the interface.</p>
                  </div>
                  <div className="s-warmth-row">
                    {warmths.map(w => (
                      <button key={w.id}
                        className={`s-warmth ${t.warmth === w.id ? "on" : ""}`}
                        onClick={() => setTweak("warmth", w.id)}>
                        <div className="s-warmth-prev">
                          {w.swatches.map((c, i) =>
                            <span key={i} style={{ background: c }}/>
                          )}
                        </div>
                        <div className="s-warmth-label">
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
                    <p>Controls primary buttons, progress fills, and the now-playing highlight.</p>
                  </div>
                  <div className="s-accent-row">
                    {accents.map(a => (
                      <button key={a.id}
                        className={`s-accent ${t.accent === a.id ? "on" : ""}`}
                        onClick={() => setTweak("accent", a.id)}
                        title={a.label}>
                        <span className="s-accent-dot" style={{ background: a.color }}>
                          {t.accent === a.id && <Icon name="check" size={14} stroke={3}/>}
                        </span>
                        <span className="s-accent-name">{a.label}</span>
                      </button>
                    ))}
                  </div>
                </div>

                <div className="s-group">
                  <div className="s-label">
                    <h3>Window style</h3>
                    <p>Match the chrome to your operating system.</p>
                  </div>
                  <div className="s-platform-row">
                    {[
                      { id: "mac", label: "macOS", desc: "Traffic-light controls, centered tabs" },
                      { id: "win", label: "Windows", desc: "Minimize / Maximize / Close controls" },
                    ].map(p => (
                      <button key={p.id}
                        className={`s-platform ${t.platform === p.id ? "on" : ""}`}
                        onClick={() => setTweak("platform", p.id)}>
                        <div className="s-platform-prev">
                          {p.id === "mac" ? (
                            <div className="prev-traffic"><span/><span/><span/></div>
                          ) : (
                            <div className="prev-wincontrols"><span>—</span><span>▢</span><span>✕</span></div>
                          )}
                        </div>
                        <div className="s-platform-meta">
                          <div className="s-platform-title">
                            {p.label}
                            {t.platform === p.id && <Icon name="check" size={12} stroke={2.5}/>}
                          </div>
                          <div className="s-platform-desc">{p.desc}</div>
                        </div>
                      </button>
                    ))}
                  </div>
                </div>
              </>
            )}

            {section === "playback" && (
              <>
                <div className="s-group">
                  <div className="s-label">
                    <h3>Streaming quality</h3>
                    <p>Higher quality uses more data when streaming online.</p>
                  </div>
                  <div className="s-radio-stack">
                    {[
                      { id: "auto", label: "Automatic", desc: "Adjusts to your connection" },
                      { id: "normal", label: "Normal", desc: "96 kbps · AAC" },
                      { id: "high", label: "High", desc: "160 kbps · AAC" },
                      { id: "very-high", label: "Very high", desc: "320 kbps · AAC" },
                    ].map(o => (
                      <label key={o.id} className={`s-radio ${(t.quality||"high") === o.id ? "on" : ""}`}>
                        <input type="radio" name="quality" checked={(t.quality||"high") === o.id} onChange={() => setTweak("quality", o.id)}/>
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
                    <input
                      type="range" min="0" max="12" step="1"
                      value={t.crossfade ?? 4}
                      onChange={e => setTweak("crossfade", Number(e.target.value))}/>
                    <div className="s-slider-val">{t.crossfade ?? 4}s</div>
                  </div>
                </div>

                <div className="s-group">
                  <ToggleRow label="Gapless playback" desc="Eliminate silence between tracks." value={t.gapless ?? true} onChange={v => setTweak("gapless", v)}/>
                  <ToggleRow label="Normalize volume" desc="Even out loudness across tracks." value={t.normalize ?? true} onChange={v => setTweak("normalize", v)}/>
                  <ToggleRow label="Show lyrics in mini-player" desc="Display synchronized lyrics when available." value={t.lyrics ?? false} onChange={v => setTweak("lyrics", v)}/>
                </div>
              </>
            )}

            {section === "downloads" && (
              <>
                <div className="s-group">
                  <div className="s-label">
                    <h3>Download quality</h3>
                    <p>Higher quality downloads use more storage.</p>
                  </div>
                  <div className="s-radio-stack">
                    {[
                      { id: "normal", label: "Normal", desc: "96 kbps · ~2 MB per track" },
                      { id: "high", label: "High", desc: "160 kbps · ~4 MB per track" },
                      { id: "very-high", label: "Very high", desc: "320 kbps · ~8 MB per track" },
                      { id: "lossless", label: "Lossless", desc: "FLAC · ~30 MB per track" },
                    ].map(o => (
                      <label key={o.id} className={`s-radio ${(t.dlQuality||"very-high") === o.id ? "on" : ""}`}>
                        <input type="radio" name="dlquality" checked={(t.dlQuality||"very-high") === o.id} onChange={() => setTweak("dlQuality", o.id)}/>
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
                    <p>Used: 2.4 GB of {t.storage ?? 8} GB</p>
                  </div>
                  <div className="s-slider-row">
                    <input
                      type="range" min="2" max="32" step="2"
                      value={t.storage ?? 8}
                      onChange={e => setTweak("storage", Number(e.target.value))}/>
                    <div className="s-slider-val">{t.storage ?? 8} GB</div>
                  </div>
                </div>

                <div className="s-group">
                  <ToggleRow label="Auto-download liked songs" desc="Save liked tracks for offline automatically." value={t.autoDl ?? true} onChange={v => setTweak("autoDl", v)}/>
                  <ToggleRow label="Download over Wi-Fi only" desc="Pause downloads on mobile networks." value={t.wifiOnly ?? true} onChange={v => setTweak("wifiOnly", v)}/>
                </div>
              </>
            )}

            {section === "account" && (
              <div className="s-empty">
                <Icon name="library" size={28}/>
                <h3>Signed in as casey@lumen.fm</h3>
                <p>Lumen Plus · renews May 28, 2026</p>
                <button className="ghost-btn" style={{ marginTop: 12 }}>Manage subscription</button>
              </div>
            )}

            {section === "about" && (
              <div className="s-empty">
                <Icon name="sparkle" size={28}/>
                <h3>Lumen 4.2.1 (build 8910)</h3>
                <p>An online + offline music player.</p>
                <p style={{ fontSize: 11.5, marginTop: 8, color: "var(--fg-mute)" }}>Made with care · © 2026</p>
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
};

const ToggleRow = ({ label, desc, value, onChange }) => (
  <label className="s-toggle-row">
    <div className="s-toggle-text">
      <div className="s-toggle-label">{label}</div>
      <div className="s-toggle-desc">{desc}</div>
    </div>
    <button
      type="button"
      role="switch"
      aria-checked={value}
      className={`s-switch ${value ? "on" : ""}`}
      onClick={() => onChange(!value)}>
      <span className="s-switch-thumb"/>
    </button>
  </label>
);

Object.assign(window, { Settings, ToggleRow });
