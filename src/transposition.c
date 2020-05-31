/*
  Ethereal is a UCI chess playing engine authored by Andrew Grant.
  <https://github.com/AndyGrant/Ethereal>     <andrew@grantnet.us>

  Ethereal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Ethereal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#if defined(__linux__)
    #include <sys/mman.h>
#endif

#include "bitboards.h"
#include "board.h"
#include "transposition.h"
#include "types.h"
#include "zobrist.h"

#define XXH_NO_PREFETCH 1
#define XXH_INLINE_ALL 1
#define XXH_FORCE_ALIGN_CHECK 0
#include "xxHash/xxhash.h"
#include "xxHash/xxh3.h"

void printBoardHashSrc(const BoardHashSrc *hashSrc);

TTable Table; // Global Transposition Table
static const uint64_t MB = 1ull << 20;

BoardHashSrc *verificationHashes = NULL;
uint64_t passedLookups = 0;
uint64_t verificationFailures = 0;

void initTT(uint64_t megabytes) {

    // Cleanup memory when resizing the table
    if (Table.hashMask) free(Table.buckets);
    free(verificationHashes);

    // Use a default keysize of 16 bits, which should be equal to
    // the smallest possible hash table size, which is 2 megabytes
    assert((1ull << 16ull) * sizeof(TTBucket) == 2 * MB);
    uint64_t keySize = 16ull;

    // Find the largest keysize that is still within our given megabytes
    while ((1ull << keySize) * sizeof(TTBucket) <= megabytes * MB / 2) keySize++;
    assert((1ull << keySize) * sizeof(TTBucket) <= megabytes * MB);

#if defined(__linux__) && !defined(__ANDROID__)
    // On Linux systems we align on 2MB boundaries and request Huge Pages
    Table.buckets = aligned_alloc(2 * MB, (1ull << keySize) * sizeof(TTBucket));
    madvise(Table.buckets, (1ull << keySize) * sizeof(TTBucket), MADV_HUGEPAGE);
#else
    // Otherwise, we simply allocate as usual and make no requests
    Table.buckets = malloc((1ull << keySize) * sizeof(TTBucket));
#endif

    verificationHashes = calloc(sizeof(BoardHashSrc), 3 * (1ull << keySize));

    // Save the lookup mask
    Table.hashMask = (1ull << keySize) - 1u;

    clearTT(); // Clear the table and load everything into the cache
}

int hashSizeMBTT() {
    return ((Table.hashMask + 1) * sizeof(TTBucket)) / MB;
}

void updateTT() {

    // The two LSBs are used for storing the entry bound
    // types, and the six MSBs are for storing the entry
    // age. Therefore add TT_MASK_BOUND + 1 to increment

    Table.generation += TT_MASK_BOUND + 1;
    assert(!(Table.generation & TT_MASK_BOUND));

}

void clearTT() {

    // Wipe the Table in preperation for a new game. The
    // Hash Mask is known to be one less than the size

    memset(Table.buckets, 0, sizeof(TTBucket) * (Table.hashMask + 1u));
}

int hashfullTT() {

    // Take a sample of the first thousand buckets in the table
    // in order to estimate the permill of the table that is in
    // use for the most recent search. We do this, instead of
    // tracking this while probing in order to avoid sharing
    // memory between the search threads.

    int used = 0;

    for (int i = 0; i < 1000; i++)
        for (int j = 0; j < TT_BUCKET_NB; j++)
            used += (Table.buckets[i].slots[j].generation & TT_MASK_BOUND) != BOUND_NONE
                 && (Table.buckets[i].slots[j].generation & TT_MASK_AGE) == Table.generation;

    return used / TT_BUCKET_NB;
}

int valueFromTT(int value, int height) {

    // When probing MATE scores into the table
    // we must factor in the search height

    return value >=  TBWIN_IN_MAX ? value - height
         : value <= -TBWIN_IN_MAX ? value + height : value;
}

int valueToTT(int value, int height) {

    // When storing MATE scores into the table
    // we must factor in the search height

    return value >=  TBWIN_IN_MAX ? value + height
         : value <= -TBWIN_IN_MAX ? value - height : value;
}

void prefetchTTEntry(uint64_t hash) {

    TTBucket *bucket = &Table.buckets[hash & Table.hashMask];
    __builtin_prefetch(bucket);
}

int verifyTTEntry(uint64_t hash, uint32_t slot, const Board *board) {

    int ret = 0;

    // verify retrieval
    BoardHashSrc hashSrc;
    boardToBoardHashSrc(board, &hashSrc);
    const BoardHashSrc *verificationBoard = &verificationHashes[(hash & Table.hashMask) * 3 + slot];

    if (memcmp(&hashSrc, verificationBoard, sizeof(BoardHashSrc)) != 0) {

        ret = 1;

        // skip increase if all zeroes
        if (!(verificationBoard->packedSquares[0] == 0 &&
              verificationBoard->packedSquares[1] == 0 &&
              verificationBoard->packedSquares[2] == 0 &&
              verificationBoard->packedSquares[3] == 0))
        {
            verificationFailures++;
/*
            printf("=================================\n");
            printBoardHashSrc(&hashSrc);
            printf("\n");
            printBoardHashSrc(&verificationHashes[(hash & Table.hashMask) * 3 + slot]);
*/
        }
    }

    passedLookups++;

    return ret;
}

int getTTEntry(uint64_t hash, uint16_t *move, int *value, int *eval, int *depth, int *bound, uint32_t *slot, const Board *board) {

    const uint16_t hash16 = hash >> 48;
    TTEntry *slots = Table.buckets[hash & Table.hashMask].slots;

    // Search for a matching hash signature
    for (int i = 0; i < TT_BUCKET_NB; i++) {
        if (slots[i].hash16 == hash16) {

            if (verifyTTEntry(hash, i, board))
                return 0;

            // Update age but retain bound type
            slots[i].generation = Table.generation | (slots[i].generation & TT_MASK_BOUND);

            // Copy over the TTEntry and signal success
            *move  = slots[i].move;
            *value = slots[i].value;
            *eval  = slots[i].eval;
            *depth = slots[i].depth;
            *bound = slots[i].generation & TT_MASK_BOUND;

            *slot = i;
            return 1;
        }
    }

    return 0;
}

void storeTTEntry(uint64_t hash, uint16_t move, int value, int eval, int depth, int bound, const Board *board) {

    int i;
    const uint16_t hash16 = hash >> 48;
    TTEntry *const slots = Table.buckets[hash & Table.hashMask].slots;
    TTEntry *replace = slots; // &slots[0]

    // Find a matching hash, or replace using MAX(x1, x2, x3),
    // where xN equals the depth minus 4 times the age difference
    for (i = 0; i < TT_BUCKET_NB && slots[i].hash16 != hash16; i++)
        if (   replace->depth - ((259 + Table.generation - replace->generation) & TT_MASK_AGE)
            >= slots[i].depth - ((259 + Table.generation - slots[i].generation) & TT_MASK_AGE))
            replace = &slots[i];

    // Prefer a matching hash, otherwise score a replacement
    replace = (i != TT_BUCKET_NB) ? &slots[i] : replace;

    // Don't overwrite an entry from the same position, unless we have
    // an exact bound or depth that is nearly as good as the old one
    if (   bound != BOUND_EXACT
        && hash16 == replace->hash16
        && depth < replace->depth - 3)
        return;

    // Finally, copy the new data into the replaced slot
    replace->depth      = (int8_t)depth;
    replace->generation = (uint8_t)bound | Table.generation;
    replace->value      = (int16_t)value;
    replace->eval       = (int16_t)eval;
    replace->move       = (uint16_t)move;
    replace->hash16     = (uint16_t)hash16;

    // store verification
    BoardHashSrc hashSrc;
    boardToBoardHashSrc(board, &hashSrc);

    size_t slotIndex = replace - slots;
    memcpy(&verificationHashes[(hash & Table.hashMask) * 3 + slotIndex], &hashSrc, sizeof(hashSrc));

/*
    uint32_t ttt;
    int hit = getTTEntry(hash, &move, &value, &eval, &depth, &bound, &ttt);
    if (!hit)
        printf("####\n");
    else {
        verifyTTEntry(hash, ttt, board);
    }
*/
}

PKEntry* getPKEntry(PKTable *pktable, uint64_t pkhash) {
    PKEntry *pkentry = &pktable->entries[pkhash >> PKT_HASH_SHIFT];
    return pkentry->pkhash == pkhash ? pkentry : NULL;
}

void storePKEntry(PKTable *pktable, uint64_t pkhash, uint64_t passed, int eval) {
    PKEntry *pkentry = &pktable->entries[pkhash >> PKT_HASH_SHIFT];
    pkentry->pkhash = pkhash;
    pkentry->passed = passed;
    pkentry->eval   = eval;
}

void boardToBoardHashSrc(const Board *board, BoardHashSrc *boardHashSrc)
{
    const uint64_t *rawBoard = (const uint64_t *)board;
    unsigned i;

    // pack the board
    for (i = 0; i < 4; ++i)
        boardHashSrc->packedSquares[i] = rawBoard[i * 2] | (rawBoard[i * 2 + 1] << 4);

    // copy the extras
    boardHashSrc->castleRooks[WHITE] = board->castleRooks         & 0xFFu;
    boardHashSrc->castleRooks[BLACK] = (board->castleRooks >> 56) & 0xFFu;
    boardHashSrc->epSquare = board->epSquare;
    boardHashSrc->turn = board->turn;

    // make sure we didn't lose information in packing
    for (i = 0; i < 4; ++i) {
        assert((boardHashSrc->packedSquares[i]        & 0x0F0F0F0F0F0F0F0Full) == rawBoard[i * 2]);
        assert(((boardHashSrc->packedSquares[i] >> 4) & 0x0F0F0F0F0F0F0F0Full) == rawBoard[i * 2 + 1]);
    }

    boardHashSrc->padding = 0;

    assert((boardHashSrc->castleRooks[WHITE] | (boardHashSrc->castleRooks[BLACK] * 0x0100000000000000ull)) ==
           board->castleRooks);
    assert(boardHashSrc->epSquare == board->epSquare);
    assert(boardHashSrc->turn == board->turn);

}

uint64_t boardHashSrcToZobrist(const BoardHashSrc *hashSrc)
{
    unsigned int sq;
    unsigned int i;
    uint64_t hash = hashSrc->turn == BLACK ? ZobristTurnKey : 0;
    uint64_t rooks = hashSrc->castleRooks[WHITE] | (hashSrc->castleRooks[BLACK] * 0x0100000000000000ull);

    union {
        uint8_t  board[SQUARE_NB];
        uint64_t rawBoard[SQUARE_NB / 8];
    } x;

    for (i = 0; i < 4; ++i) {
        x.rawBoard[i * 2]     =  hashSrc->packedSquares[i]       & 0x0F0F0F0F0F0F0F0Full;
        x.rawBoard[i * 2 + 1] = (hashSrc->packedSquares[i] >> 4) & 0x0F0F0F0F0F0F0F0Full;
    }

    for (sq = 0; sq < SQUARE_NB; ++sq) {
        const uint8_t piece = x.board[sq];

        hash ^= ZobristKeys[piece][sq];
    }

    if (hashSrc->epSquare != -1)
        hash ^= ZobristEnpassKeys[fileOf(hashSrc->epSquare)];

    while (rooks) hash ^= ZobristCastleKeys[poplsb(&rooks)];

    return hash;
}

void printBoardHashSrc(const BoardHashSrc *hashSrc)
{
    union {
        uint8_t  board[SQUARE_NB];
        uint64_t rawBoard[SQUARE_NB / 8];
    } x;

    for (unsigned i = 0; i < 4; ++i) {
        x.rawBoard[i * 2]     =  hashSrc->packedSquares[i]       & 0x0F0F0F0F0F0F0F0Full;
        x.rawBoard[i * 2 + 1] = (hashSrc->packedSquares[i] >> 4) & 0x0F0F0F0F0F0F0F0Full;
    }

    for (int rank=7; rank >= 0; rank--)
    {
        for (unsigned file = 0; file < 8; ++file)
        {
            char c = ' ';
            switch (x.board[rank * 8 + file])
            {
                case WHITE_PAWN:   c = 'P';  break;
                case WHITE_KNIGHT: c = 'N';  break;
                case WHITE_BISHOP: c = 'B';  break;
                case WHITE_ROOK:   c = 'R';  break;
                case WHITE_QUEEN:  c = 'Q';  break;
                case WHITE_KING:   c = 'K';  break;
                case BLACK_PAWN:   c = 'p';  break;
                case BLACK_KNIGHT: c = 'n';  break;
                case BLACK_BISHOP: c = 'b';  break;
                case BLACK_ROOK:   c = 'r';  break;
                case BLACK_QUEEN:  c = 'q';  break;
                case BLACK_KING:   c = 'k';  break;
                default:
                    if ((int)(rank * 8 + file) == hashSrc->epSquare)
                        c = '*';
                    break;
            }

            printf("%c", c);
        }
        if (!hashSrc->turn && rank == 0) printf(" O");
        if (hashSrc->turn && rank == 7) printf(" O");
        printf("\n");
    }
}

uint64_t boardToHash(const Board *board)
{
    _Alignas(64) BoardHashSrc hashSrc;
    boardToBoardHashSrc(board, &hashSrc);

    if (1)
        return boardHashSrcToZobrist(&hashSrc);
    else if (0) {
        union {
            uint32_t hash32[5];
            unsigned char sha1bytes[20];
        } x;

        SHA1((const unsigned char *)&hashSrc, 40, x.sha1bytes);

        return (x.hash32[0] ^ x.hash32[1] ^ x.hash32[2]) |
            (((uint64_t)(x.hash32[1] ^ x.hash32[3])) << 32);

    } else if (0) {
        XXH64_hash_t hash = XXH64(&hashSrc, 32, *(const uint64_t *)hashSrc.castleRooks);
        return hash;
    } else if (0) {
        XXH128_hash_t hash = XXH128(&hashSrc, 32, *(const uint64_t *)hashSrc.castleRooks);
        return hash.low64 ^ hash.high64;
    }
    else {
        return 0;
    }

}

void calculateHashStatistics()
{
    return;
    uint64_t totalUtilizedBuckets = 0;
    uint64_t totalUtilizedSlots = 0;

    for (size_t i = 0; i <= Table.hashMask; ++i)
    {
        uint64_t utilizedSlots = 0;

        // Search for a matching hash signature
        for (size_t j = 0; j < TT_BUCKET_NB; j++)
        {
            const BoardHashSrc *verificationBoard = &verificationHashes[i * 3 + j];

            utilizedSlots += (!(verificationBoard->packedSquares[0] == 0 &&
                                verificationBoard->packedSquares[1] == 0 &&
                                verificationBoard->packedSquares[2] == 0 &&
                                verificationBoard->packedSquares[3] == 0));
        }

        totalUtilizedBuckets += !!utilizedSlots;
        totalUtilizedSlots += utilizedSlots;
    }
    double avgO = (double)totalUtilizedSlots / totalUtilizedBuckets;
    double hashO = (100.0 * totalUtilizedSlots) / (3 * (Table.hashMask + 1));
    double scaledAvgO = avgO * (50 / hashO);
    printf("Hash occupancy: %.3f %% -- average bucket occupancy: %.4f -- scaled average bucket occupancy: %.4f\n",
           hashO, avgO, scaledAvgO);
}
