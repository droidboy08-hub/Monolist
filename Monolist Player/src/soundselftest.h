#pragma once

class Library;

// --sound-test: the sound effects without a sound card or libmpv
// (SoundChain, SoundEffects); in the release gate:
//  - the chain's text against the golden copies the research proved inside
//    the shipped libmpv (with and without the 8D branch), mpv's af value,
//    when a chain is needed at all, what each setting puts in it (the 8D
//    room rule too), and the commands between two sets of values;
//  - the ramps: every step within its limit (a band 1 dB, the pre-gain
//    0.5 dB, a weight 0.07, the limiter's ceiling 0.01), all values arriving
//    together, the graph's shape kept;
//  - the presets, and the clipping maths: the headroom table, the curve
//    against the real filters' measured response (lowshelf, equalizer), the
//    pre-gain never positive and never more than +6 dB left for the limiter;
//  - what is kept: defaults, switches written at once, a band drag 400 ms
//    after it settles and at once as Monolist closes, the preset winning
//    unless it is custom, values out of range read as the defaults,
//    Slowed and Nightcore one at a time, and the summary line.
// Refuses to run without MONOLIST_DATA_DIR: it writes settings there.
int runSoundSelfTest(Library *library);

// --sound-live-test: the same effects on the real libmpv, with
// MONOLIST_MPV_AO=null (no sound device), against files and a stand-in
// server on this computer; not in the release gate (it is timed):
//  - nothing on, then each effect switched on mid-song: the chain answers,
//    its values arrive within a fraction of a second, the clock runs at
//    0.85x under Slowed and 1.25x under Nightcore, the song's length stays;
//  - a band changed then a seek, the next file, 8D switched on (a rebuild)
//    and off mid-song, everything off mid-song (the chain stays until the
//    next file), the 8 kHz mono file with everything on, a chain mpv refuses;
//  - every preset with High bass +12 and heavy reverb, with and without the
//    8D branch, answering;
//  - the levelling preamp moving only at a load or seek;
//  - a mono song keeping its values when its output reopens mid-song;
//  - a chain broken while idle: the song still plays, once, the effects off
//    with one notice, no load failure, the saved JioSaavn match untouched;
//    a file that fails without them too is a load failure, effects not blamed;
//  - a slow seek through a throttled stand-in: no false alarm;
//  - the JioSaavn takeover carrying the chain and the speed over;
//  - a launch with Slowed + reverb kept plays its first song with them.
// Refuses to run without MONOLIST_DATA_DIR.
int runSoundLiveSelfTest(Library *library);
