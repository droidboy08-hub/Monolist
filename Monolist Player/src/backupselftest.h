#pragma once

class Library;

// --backup-test: the library's backup file and its merge (LibraryBackup), on
// invented songs, with no network and no window. What the library keeps for
// it (playlists' uids, the tombstones, updated_at, Library::edited); a
// library exported, emptied and brought back exactly; every merge rule —
// likes, saves and playlists against tombstones both ways, times kept, plays
// once each with the most heard, turn-downs taken back, History from the
// plays after a clear — and that two libraries merged either way round end
// the same; the same file merged twice changing nothing; a newer version and
// a file that is not a library refused; nothing of this computer's in the
// file (downloads, paths, settings, the YouTube Music mirror); and Export
// and Import as Settings calls them (DriveSync, its dialogs answered by the
// test). Reports on stderr, one line per check, and returns how many
// failed. Refuses to run without MONOLIST_DATA_DIR, since it empties the
// library there.
int runBackupSelfTest(Library *library);
