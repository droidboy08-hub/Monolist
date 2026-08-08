# DESIGN SYSTEM: ACOUSTIC NOIR

This document tracks the visual language, design tokens, and orientation-specific prototypes for the Melody Music Player.

---

## 🎨 Visual Identity
- **Concept:** Minimalist-Modern / Immersive High-Fidelity.
- **Tone:** Sophisticated Calm / Harmonious Energy.
- **Primary Font:** Inter (Sans-serif).

### Design Tokens
- **Background:** `#121212` (Deep Charcoal)
- **Surface:** `#1E1E1E` (Tonal Elevation)
- **Accent:** `#BB86FC` (Vibrant Purple)
- **Rounding (Large):** 32px - 48px (Artwork, Hero Banners)
- **Rounding (Small):** 8px (Buttons, Inputs)

---

## 📱 Mobile Architecture (Portrait)
Focused on one-handed use and content immersion.

| Screen | ID | Status | Key Features |
| :--- | :--- | :--- | :--- |
| **Now Playing** | `4feaabbf2c6841b68dfa788c3aa02f86` | ✅ Generated | 1:1 Art, 8px Progress Bar |
| **Home** | `8375279471f0491eb92c7e5328451103` | ✅ Generated | Discovery Grids, Mini-Player |

---

## 🖥️ Desktop Architecture (Landscape)
Focused on multi-column layouts and expansive white space.

| Screen | ID | Status | Key Features |
| :--- | :--- | :--- | :--- |
| **Now Playing** | `93d0bbd2f47c46ccbeeae15e86bedec6` | ✅ Generated | 40/60 Split, 48px Radius |
| **Home** | `80534fac4b08472489183fd6101830c1` | ✅ Generated | 3-Column Dashboard, Sidebar |

---

## 🛠️ Design-to-Code Rules
1. **Separation:** UI components must remain decoupled from the `StreamService` and `DownloadService`.
2. **Adaptive Colors:** Future implementation should prioritize K-means color extraction from album art to dynamically tint the `#BB86FC` accent.
3. **Orientation Locking:** Desktop build is locked to Landscape; Mobile builds support Portrait by default.
