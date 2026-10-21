#pragma once

class Library;

// --drive-test: the Google Drive backup (DriveAuth, DriveSync) against
// stand-ins on this computer for Google's token and revoke endpoints and for
// Drive's files and upload endpoints, with an invented client and account;
// never Google itself. The whole sign-in as a browser would make it (the
// page's address checked: client, loopback redirect, PKCE S256, state, the
// three scopes and nothing else; then the browser's return played to the
// listener), a wrong state or path ignored, "You didn't allow it", the Drive
// box unticked refused and revoked, the code exchanged with the verifier that
// matches its challenge; the access token renewed when it runs out and once
// on a 401, an ended sign-in (invalid_grant) forgotten; a first sync making
// the file (multipart, in appDataFolder), another computer's file merged both
// ways and uploaded once, a second sync uploading nothing, a file that cannot
// be read never overwritten, two copies made one, a file that vanished
// looked for again, backoff on 429, 5xx and Drive's rate limit; and signing
// out withdrawing the sign-in and deleting only the backup's own secret and
// settings. Throughout: no request carries a cookie, the token goes to the
// Drive stand-in only, a redirect elsewhere is not followed, and no token,
// code, verifier, state, secret or email ever reaches the log. Reports on
// stderr, one line per check, and returns how many failed. Refuses to run
// without MONOLIST_DATA_DIR, since it empties the library there.
int runDriveSelfTest(Library *library);
