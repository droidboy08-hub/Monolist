#pragma once

#include <QByteArray>
#include <QtGlobal>

// DES, as FIPS 46-3 specifies it, for one purpose: JioSaavn sends its stream
// links encrypted with single DES in ECB mode under a key every client knows,
// and Qt has no DES (QCryptographicHash is hashes only, and DES is too old for
// any of Qt's backends to expose). Pulling in OpenSSL for eight bytes of key
// would be a dependency on every platform for one call, so the cipher is
// written out here from the standard: the permutation tables and S-boxes are
// FIPS 46-3's, bit for bit.
//
// This is NOT a cipher for protecting anything. DES has a 56-bit key and ECB
// leaks patterns; it is here only because it is what the other end speaks.
// The self-test checks it against the standard's known answer.
namespace Des {

// One 64-bit block, bits numbered as the standard numbers them: bit 1 is the
// most significant bit of the first byte. `key` is the 64-bit key with its
// parity bits, which DES ignores.
quint64 encryptBlock(quint64 key, quint64 block);
quint64 decryptBlock(quint64 key, quint64 block);

// ECB over whole blocks with PKCS#5 padding (PKCS#7 at DES's 8-byte block).
// `key` must be exactly 8 bytes. Decryption returns an empty array and sets
// *ok to false when the input is not whole blocks or the padding does not
// read back.
QByteArray encryptEcb(const QByteArray &key, const QByteArray &plain);
QByteArray decryptEcb(const QByteArray &key, const QByteArray &cipher, bool *ok = nullptr);

} // namespace Des
