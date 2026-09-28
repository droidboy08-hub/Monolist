#pragma once

class Library;

// --recovery-test: how a song that has begun is kept playing, on the real mpv
// against a stand-in server on this computer, with no network and no yt-dlp.
// StreamResolver's rungs are answered by the test (setTestAnswer) with links
// to the stand-in, which serves a tone as a WAV file and refuses, or cuts
// short, the way googlevideo can:
//  - a refused InnerTube link: InnerTube is asked once more, before the muxed
//    stream, and the song plays; with playback.refused=muxed, the muxed
//    stream first, as before;
//  - that fresh link refused too: the muxed stream rescues the play, the
//    same play can go back to it, and the song's next play starts from
//    InnerTube again; with playback.rescue_link=keep, from the rescue link,
//    as before;
//  - a stream whose first request is cut at 60% and whose reconnect is
//    refused: the song carries on from where it stopped, on a fresh link, and
//    moves on only at its end; the same for a row's own link, loaded again;
//    with playback.early_end=next, it moves on at the cut, as before;
//  - a downloaded file mpv will not open: the song streams instead; a file
//    with nothing to stream it from is passed over;
//  - three songs in a row that will not play stop the queue, and the count
//    starts again only once a song's sound has started, not when its link
//    arrives.
// One line per check; returns how many failed. Refuses to run without
// MONOLIST_DATA_DIR: it writes a downloads row and a junk file there, and
// takes both away again.
int runRecoverySelfTest(Library *library);
