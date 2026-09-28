# Engine study against the BitChord research

The owner's benchmark is the BitChord engine research
(github.com/droidboy08-hub/BitChord, docs/research/BITCHORD_ENGINE_RESEARCH.md).
These three documents compare Monolist's engine with it, in the order they were
written (2026-09-26 to 27):

1. [1-engine-map.md](1-engine-map.md) — Monolist's pipeline, stage by stage, in the paper's terms.
2. [2-scorecard-and-measurements.md](2-scorecard-and-measurements.md) — the 48-row checklist (§15.2) and measured timings on the old VM.
3. [3-plan.md](3-plan.md) — the approved, ranked plan (items PP-, QT-, LY-). B0-B9 are done; see CLAUDE.md for what is left.

Numbers were measured on a Windows ARM64 VM under x64 emulation, so absolute
times on a native PC will be lower; compare before/after on the same machine.