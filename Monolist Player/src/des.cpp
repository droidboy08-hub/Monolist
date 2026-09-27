#include "des.h"

#include <array>

namespace {

// Every table is FIPS 46-3's, and numbers bits the way it does: from 1, most
// significant first. permute() reads them as written, so each can be checked
// against the standard by eye.

// Initial permutation IP, and its inverse applied at the end.
constexpr quint8 kIp[64] = {
    58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17,  9, 1,
    59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,
    63, 55, 47, 39, 31, 23, 15, 7,
};
constexpr quint8 kFp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32,
    39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,
    35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41,  9, 49, 17, 57, 25,
};

// E, the expansion of a 32-bit half to 48 bits.
constexpr quint8 kE[48] = {
    32,  1,  2,  3,  4,  5,
     4,  5,  6,  7,  8,  9,
     8,  9, 10, 11, 12, 13,
    12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,
    20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,
    28, 29, 30, 31, 32,  1,
};

// P, applied to the S-boxes' output.
constexpr quint8 kP[32] = {
    16,  7, 20, 21, 29, 12, 28, 17,
     1, 15, 23, 26,  5, 18, 31, 10,
     2,  8, 24, 14, 32, 27,  3,  9,
    19, 13, 30,  6, 22, 11,  4, 25,
};

// Permuted choices 1 and 2 of the key schedule. PC-1 drops the eight parity
// bits (8, 16, ..., 64), which is why they appear nowhere below.
constexpr quint8 kPc1[56] = {
    57, 49, 41, 33, 25, 17,  9,
     1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27,
    19, 11,  3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,
     7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29,
    21, 13,  5, 28, 20, 12,  4,
};
constexpr quint8 kPc2[48] = {
    14, 17, 11, 24,  1,  5,
     3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8,
    16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32,
};

// Left shifts of C and D before each round's subkey.
constexpr quint8 kShifts[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };

// S1..S8, each four rows of sixteen, as printed in the standard.
constexpr quint8 kSBoxes[8][64] = {
    { 14,  4, 13,  1,  2, 15, 11,  8,  3, 10,  6, 12,  5,  9,  0,  7,
       0, 15,  7,  4, 14,  2, 13,  1, 10,  6, 12, 11,  9,  5,  3,  8,
       4,  1, 14,  8, 13,  6,  2, 11, 15, 12,  9,  7,  3, 10,  5,  0,
      15, 12,  8,  2,  4,  9,  1,  7,  5, 11,  3, 14, 10,  0,  6, 13 },
    { 15,  1,  8, 14,  6, 11,  3,  4,  9,  7,  2, 13, 12,  0,  5, 10,
       3, 13,  4,  7, 15,  2,  8, 14, 12,  0,  1, 10,  6,  9, 11,  5,
       0, 14,  7, 11, 10,  4, 13,  1,  5,  8, 12,  6,  9,  3,  2, 15,
      13,  8, 10,  1,  3, 15,  4,  2, 11,  6,  7, 12,  0,  5, 14,  9 },
    { 10,  0,  9, 14,  6,  3, 15,  5,  1, 13, 12,  7, 11,  4,  2,  8,
      13,  7,  0,  9,  3,  4,  6, 10,  2,  8,  5, 14, 12, 11, 15,  1,
      13,  6,  4,  9,  8, 15,  3,  0, 11,  1,  2, 12,  5, 10, 14,  7,
       1, 10, 13,  0,  6,  9,  8,  7,  4, 15, 14,  3, 11,  5,  2, 12 },
    {  7, 13, 14,  3,  0,  6,  9, 10,  1,  2,  8,  5, 11, 12,  4, 15,
      13,  8, 11,  5,  6, 15,  0,  3,  4,  7,  2, 12,  1, 10, 14,  9,
      10,  6,  9,  0, 12, 11,  7, 13, 15,  1,  3, 14,  5,  2,  8,  4,
       3, 15,  0,  6, 10,  1, 13,  8,  9,  4,  5, 11, 12,  7,  2, 14 },
    {  2, 12,  4,  1,  7, 10, 11,  6,  8,  5,  3, 15, 13,  0, 14,  9,
      14, 11,  2, 12,  4,  7, 13,  1,  5,  0, 15, 10,  3,  9,  8,  6,
       4,  2,  1, 11, 10, 13,  7,  8, 15,  9, 12,  5,  6,  3,  0, 14,
      11,  8, 12,  7,  1, 14,  2, 13,  6, 15,  0,  9, 10,  4,  5,  3 },
    { 12,  1, 10, 15,  9,  2,  6,  8,  0, 13,  3,  4, 14,  7,  5, 11,
      10, 15,  4,  2,  7, 12,  9,  5,  6,  1, 13, 14,  0, 11,  3,  8,
       9, 14, 15,  5,  2,  8, 12,  3,  7,  0,  4, 10,  1, 13, 11,  6,
       4,  3,  2, 12,  9,  5, 15, 10, 11, 14,  1,  7,  6,  0,  8, 13 },
    {  4, 11,  2, 14, 15,  0,  8, 13,  3, 12,  9,  7,  5, 10,  6,  1,
      13,  0, 11,  7,  4,  9,  1, 10, 14,  3,  5, 12,  2, 15,  8,  6,
       1,  4, 11, 13, 12,  3,  7, 14, 10, 15,  6,  8,  0,  5,  9,  2,
       6, 11, 13,  8,  1,  4, 10,  7,  9,  5,  0, 15, 14,  2,  3, 12 },
    { 13,  2,  8,  4,  6, 15, 11,  1, 10,  9,  3, 14,  5,  0, 12,  7,
       1, 15, 13,  8, 10,  3,  7,  4, 12,  5,  6, 11,  0, 14,  9,  2,
       7, 11,  4,  1,  9, 12, 14,  2,  0,  6, 10, 13, 15,  3,  5,  8,
       2,  1, 14,  7,  4, 10,  8, 13, 15, 12,  9,  0,  3,  5,  6, 11 },
};

// Output bit i (from the most significant) is input bit table[i], counted
// from 1 at the most significant of `inBits`.
quint64 permute(quint64 in, int inBits, const quint8 *table, int outBits)
{
    quint64 out = 0;
    for (int i = 0; i < outBits; ++i)
        out = (out << 1) | ((in >> (inBits - table[i])) & 1u);
    return out;
}

quint32 rotateLeft28(quint32 value, int count)
{
    return ((value << count) | (value >> (28 - count))) & 0x0FFFFFFFu;
}

std::array<quint64, 16> subkeys(quint64 key)
{
    std::array<quint64, 16> keys{};
    const quint64 cd = permute(key, 64, kPc1, 56);
    quint32 c = quint32(cd >> 28) & 0x0FFFFFFFu;
    quint32 d = quint32(cd) & 0x0FFFFFFFu;
    for (int round = 0; round < 16; ++round) {
        c = rotateLeft28(c, kShifts[round]);
        d = rotateLeft28(d, kShifts[round]);
        keys[round] = permute((quint64(c) << 28) | d, 56, kPc2, 48);
    }
    return keys;
}

// The cipher function f(R, K).
quint32 feistel(quint32 right, quint64 subkey)
{
    const quint64 mixed = permute(right, 32, kE, 48) ^ subkey;
    quint32 out = 0;
    for (int box = 0; box < 8; ++box) {
        const int six = int((mixed >> (42 - 6 * box)) & 0x3F);
        // The outer two bits pick the row, the inner four the column.
        const int row = ((six >> 4) & 0x2) | (six & 0x1);
        const int column = (six >> 1) & 0xF;
        out = (out << 4) | kSBoxes[box][row * 16 + column];
    }
    return quint32(permute(out, 32, kP, 32));
}

quint64 crypt(quint64 key, quint64 block, bool decrypt)
{
    const std::array<quint64, 16> keys = subkeys(key);
    const quint64 permuted = permute(block, 64, kIp, 64);
    quint32 left = quint32(permuted >> 32);
    quint32 right = quint32(permuted);
    for (int round = 0; round < 16; ++round) {
        const quint32 previous = right;
        right = left ^ feistel(right, keys[decrypt ? 15 - round : round]);
        left = previous;
    }
    // The halves go into the final permutation swapped: R16 L16.
    return permute((quint64(right) << 32) | left, 64, kFp, 64);
}

quint64 readBlock(const char *bytes)
{
    quint64 block = 0;
    for (int i = 0; i < 8; ++i)
        block = (block << 8) | quint8(bytes[i]);
    return block;
}

void writeBlock(quint64 block, char *bytes)
{
    for (int i = 7; i >= 0; --i) {
        bytes[i] = char(block & 0xFF);
        block >>= 8;
    }
}

} // namespace

namespace Des {

quint64 encryptBlock(quint64 key, quint64 block)
{
    return crypt(key, block, /*decrypt=*/false);
}

quint64 decryptBlock(quint64 key, quint64 block)
{
    return crypt(key, block, /*decrypt=*/true);
}

QByteArray encryptEcb(const QByteArray &key, const QByteArray &plain)
{
    if (key.size() != 8)
        return {};
    const quint64 k = readBlock(key.constData());
    // PKCS#5: always at least one byte of padding, so a whole last block
    // gains a block of eights and the padding can always be read back.
    const int pad = 8 - int(plain.size() % 8);
    QByteArray padded = plain;
    padded.append(QByteArray(pad, char(pad)));
    QByteArray out(padded.size(), Qt::Uninitialized);
    for (qsizetype at = 0; at < padded.size(); at += 8)
        writeBlock(encryptBlock(k, readBlock(padded.constData() + at)), out.data() + at);
    return out;
}

QByteArray decryptEcb(const QByteArray &key, const QByteArray &cipher, bool *ok)
{
    if (ok)
        *ok = false;
    if (key.size() != 8 || cipher.isEmpty() || cipher.size() % 8 != 0)
        return {};
    const quint64 k = readBlock(key.constData());
    QByteArray out(cipher.size(), Qt::Uninitialized);
    for (qsizetype at = 0; at < cipher.size(); at += 8)
        writeBlock(decryptBlock(k, readBlock(cipher.constData() + at)), out.data() + at);

    // Wrong key or damaged data shows up here, as padding that does not read.
    const int pad = quint8(out.at(out.size() - 1));
    if (pad < 1 || pad > 8)
        return {};
    for (qsizetype i = out.size() - pad; i < out.size(); ++i) {
        if (quint8(out.at(i)) != pad)
            return {};
    }
    out.chop(pad);
    if (ok)
        *ok = true;
    return out;
}

} // namespace Des
