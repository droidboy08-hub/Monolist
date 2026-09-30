#pragma once

class QString;

// --download-cleanup-test: what a failed or cancelled download may delete,
// on invented files in a scratch folder inside MONOLIST_DATA_DIR, and where
// downloads go when MONOLIST_DOWNLOAD_DIR or MONOLIST_DATA_DIR is set. No
// network, no yt-dlp, and nothing outside that folder is written or deleted.
// Reports on stderr, one line per check, and returns how many failed. Refuses
// to run without MONOLIST_DATA_DIR, since it opens a DownloadManager on the
// database there.
int runDownloadCleanupSelfTest();

// --saavn-download-test: a download taken from JioSaavn's copy of a song
// (saavndownload.*), against a stand-in CDN on this computer serving a tone
// FFmpeg makes for the test: the file's name, tags and cover as a yt-dlp
// download's; a refused link handing the song to yt-dlp (on an invented id
// YouTube does not have, so yt-dlp fails without downloading anything) with
// what was there before kept; a missing cover; a cancel part-way; MP3; and
// JioSaavn never asked on Standard. Downloads only into a scratch folder in
// MONOLIST_DATA_DIR, which it insists on.
int runSaavnDownloadSelfTest();

// --download-queue-test: the downloads not finished are kept across a quit
// (DownloadManager::saveQueue): queued ones queued again at the next launch
// in their order, a cancel written a moment later, failed ones back as
// failed with why and not tried again unasked, and one finished meanwhile
// not queued again. On invented songs that are never started; in
// MONOLIST_DATA_DIR and MONOLIST_DOWNLOAD_DIR only, which it insists on.
int runDownloadQueueSelfTest();

// --download-resume-test <videoId>: a real song, from YouTube through yt-dlp,
// stopped part-way as a quit stops it, then finished by the next launch from
// the partial file kept; nothing left over. Needs the network; in
// MONOLIST_DATA_DIR and MONOLIST_DOWNLOAD_DIR only, which it insists on.
int runDownloadResumeSelfTest(const QString &videoId);
