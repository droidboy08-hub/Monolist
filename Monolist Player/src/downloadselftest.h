#pragma once

// --download-cleanup-test: what a failed or cancelled download may delete,
// on invented files in a scratch folder inside MONOLIST_DATA_DIR, and where
// downloads go when MONOLIST_DOWNLOAD_DIR or MONOLIST_DATA_DIR is set. No
// network, no yt-dlp, and nothing outside that folder is written or deleted.
// Reports on stderr, one line per check, and returns how many failed. Refuses
// to run without MONOLIST_DATA_DIR, since it opens a DownloadManager on the
// database there.
int runDownloadCleanupSelfTest();
