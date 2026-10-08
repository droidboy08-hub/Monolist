#pragma once

// Search with no cap: a search's next pages (MediaExtractor::loadMore) on a
// stand-in YouTube Music on this computer — songs a page at a time with the
// ones already shown dropped, the end where a page brings nothing new or
// fails, a page for an older search dropped, card sections continued each
// on its own. Prints a line per check; returns how many failed.
int runSearchSelfTest();
