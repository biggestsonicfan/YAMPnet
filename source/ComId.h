#pragma once
// ComId.h - the YAMP Communication ID standard.
//
// RPCN partitions EVERYTHING by ComId. The server list, the world list, the room list, room
// creation and room search all carry one, and two clients only ever see each other's rooms if
// their ComIds match byte for byte. That makes the ComId the one field that decides which lobby
// space a game plays in - and it means a build that hardcodes a single id puts every game YAMP
// hosts into the SAME lobby list. A Virtual On cabinet then advertises alongside a Sonic the
// Fighters one, both are joinable, and the pairing fails at the netcode rather than at the room
// list, which is the worst possible place to find out.
//
// So: one ComId per game, assigned by a rule rather than by hand-editing a config.
//
//   YMP <6-char game code> _ <2-digit revision>          12 characters, e.g. "YMPSNCFTR_00"
//   ^^^                     ^
//   |                       netcode revision channel (see kRevision)
//   the YAMP namespace - never a real PSN title's prefix
//
// WHY THAT SHAPE. RPCN reads the ComId as exactly 12 bytes and rejects the request as Malformed
// unless the first 9 are ASCII uppercase letters or digits (RpcnClient.cpp, FrameRoomPayload);
// bytes 9-11 it does not police, and PSN's own ids spell them "_NN". Nine characters is therefore
// the whole of the usable namespace: three go to "YMP" so a YAMP lobby space can never collide
// with a genuine PSN title's, and six identify the game.
//
// This works against a stock RPCN server because of `CreateMissing`: GetServerList/GetWorldList on
// an unknown ComId REGISTER it, so a new game needs no servers.cfg edit on anyone's part. That is
// also why the derivation must be deterministic - both peers compute the same id from the same
// game key with no central registry to agree on.
//
// THE GAME KEY IS PART OF THE STANDARD. The plugin cannot tell which game is running: it learns
// nothing about the title through the ABI, so the key is whatever the host passes in
// yampnet_rpcn_config::communication_id. Two builds that pass different spellings for one game
// land in two lobby spaces and never see each other, which is why normalisation folds case and
// punctuation and why kRegistry carries aliases. Prefer a stable key - the ROM/romset name is a
// better one than a display title, which gets retitled.

#include <stdint.h>

namespace yampnet::comid
{
    // 12 characters plus a NUL. RPCN truncates at 12 and NUL-pads a shorter one.
    constexpr uint32_t kLength = 12;
    constexpr uint32_t kBufferSize = kLength + 1;

    // The three characters that make an id ours. Every ComId this plugin derives starts with it.
    constexpr char kPrefix[] = "YMP";

    // The revision channel: the "_NN" tail, which puts incompatible builds in separate lobby
    // spaces instead of letting them meet and desync.
    //
    // Bump this ONLY for a change that breaks peer-to-peer compatibility with the previous build -
    // a packet layout change, a different pad encoding, a lockstep rule both sides must agree on.
    // It is NOT YAMPNET_ABI_VERSION: that one guards host-to-plugin compatibility and is checked
    // at load, where a mismatch is a clean refusal. This one guards plugin-to-plugin compatibility,
    // where the only enforcement available is that the two never see the same rooms. Bumping it
    // strands everyone still on the old build in the old lobby space - which is the point, and the
    // reason not to bump it for a change that would have interoperated.
    constexpr uint32_t kRevision = 0;

    // RPCN's own rule, applied before the request is framed rather than after the server has
    // rejected it: 9 to 12 characters, of which the first 9 are ASCII uppercase or digits.
    bool IsWellFormed(const char* id);

    // The full canonical shape - 9 characters of [A-Z0-9], then '_', then two digits.
    //
    // This, not IsWellFormed(), is what tells a ComId apart from a game key, and the difference
    // matters: "VIRTUALON" is nine uppercase letters and therefore passes RPCN's rule, so a looser
    // test would send a game's NAME to the server as its ComId and quietly give it a lobby space
    // nobody else computes. Requiring the "_NN" tail leaves no plain-language key that can be
    // mistaken for an id.
    bool LooksLikeComId(const char* id);

    // Canonical AND in the YAMP namespace - i.e. produced by this standard.
    bool IsStandard(const char* id);

    // Derives the ComId for a game key. `game_key` is normalised (case folded, everything outside
    // [A-Z0-9] dropped) and then either matched against kRegistry or hashed. Returns false, leaving
    // `out` untouched, only for a null or effectively empty key.
    bool ForGame(const char* game_key, char out[kBufferSize]);

    // What the host passed, turned into the ComId to actually use. Handles the three cases the
    // plugin sees, and reports which one it took through `out_note` (a static string, never null,
    // suitable for a log line):
    //
    //   * a ComId already in the YAMP namespace  -> used verbatim; the host did this itself.
    //   * any other canonical ComId              -> used verbatim, because deliberately joining a
    //                                               real PSN title's lobbies is a legitimate thing
    //                                               to do - but the note SAYS it is a shared space,
    //                                               since this is also exactly what a build that
    //                                               hardcodes "NPWR02113_00" for every game looks
    //                                               like from here.
    //   * anything else                          -> read as a game key and derived.
    //
    // Returns false (with a note explaining why) only when nothing usable can be produced.
    bool Resolve(const char* input, char out[kBufferSize], const char** out_note);

    // True for a canonical foreign ComId, i.e. one Resolve() passes through but that is not
    // per-game under this standard. Callers use it to decide whether to warn.
    bool IsSharedLobbySpace(const char* id);
}
