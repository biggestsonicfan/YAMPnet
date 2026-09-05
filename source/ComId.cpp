#include "ComId.h"

#include <cstdio>
#include <cstring>

namespace yampnet::comid
{
    namespace
    {
        // Six characters of [A-Z0-9], and nothing else - a code that is not exactly that would
        // make the ComId malformed and the server would answer InvalidInput at discovery.
        constexpr uint32_t kCodeLength = 6;

        struct RegistryEntry
        {
            const char* key;    // already normalised: uppercase, [A-Z0-9] only
            const char* code;   // exactly kCodeLength characters
        };

        // Hand-assigned codes for the games we know about, so their ids are readable in a log and
        // stay put forever. This table is a CONVENIENCE, not the mechanism: a game that is not
        // listed still gets a lobby space of its own from the hash below, and adding it here later
        // MOVES it - so add a game before people play it, or leave it derived.
        //
        // Alias rows exist because the key is whatever the host passes, and two spellings of one
        // game are two lobby spaces. Every plausible spelling a build might use should land on the
        // same code.
        constexpr RegistryEntry kRegistry[] = {
            // Sonic the Fighters - the lockstep netcode's first game, modelled on the PS3 port.
            { "SONICTHEFIGHTERS", "SNCFTR" },
            { "SONICFIGHTERS",    "SNCFTR" },
            { "SONICCHAMPIONSHIP","SNCFTR" },   // the Japanese title of the same board
            { "STF",              "SNCFTR" },

            // The other two Model 2 fighters. Both are lockstep games, same as StF.
            { "VIRTUAFIGHTER2",   "VRTFT2" },
            { "VF2",              "VRTFT2" },
            { "FIGHTINGVIPERS",   "FGTVPR" },
            { "FVIPERS",          "FGTVPR" },   // the romset name
            { "FV",               "FGTVPR" },

            // Fighting Vipers 2 - a different board from the three above, same netcode.
            { "FIGHTINGVIPERS2",  "FGTVP2" },
            { "FVIPERS2",         "FGTVP2" },
            { "FV2",              "FGTVP2" },

            // Cyber Troopers Virtual On - the first game to use the linked-cabinet channel.
            { "VIRTUALON",        "VRTLON" },
            { "CYBERTROOPERS",    "VRTLON" },
            { "VONLINE",          "VRTLON" },
            { "VON",              "VRTLON" },

            // Daytona USA 2, linked-cabinet like Virtual On - and the reason the two EDITIONS get
            // separate codes rather than aliasing onto one: they are separate ROM sets whose link
            // protocol never spoke across the version boundary in the arcade either, so a shared
            // lobby list would advertise matches that cannot be played. The bare key follows MAME,
            // where `daytona2` IS Battle on the Edge and Power Edition is `dayto2pe`.
            { "DAYTONAUSA2",      "DYT2BE" },
            { "DAYTONA2",         "DYT2BE" },
            { "DAYTONAUSA2BATTLEONTHEEDGE", "DYT2BE" },
            { "DAYTO2PE",         "DYT2PE" },
            { "DAYTONA2POWEREDITION",       "DYT2PE" },
            { "DAYTONAUSA2POWEREDITION",    "DYT2PE" },
        };

        // ON ROM REVISIONS. The granularity here is the GAME, so `vf2`, `vf2a` and `vf2b` share a
        // lobby space - a deliberate limit rather than an oversight. Two peers on different
        // revisions of one ROM can still desync, but a ComId cannot tell them that: it is resolved
        // before login, from a key, and it partitions the room list rather than checking anything.
        // Guarding a revision mismatch belongs in the room's attribute word, where the joiner can
        // read it and be told why. Splitting revisions into separate spaces here would only hide
        // the problem behind an empty room list.

        bool IsIdChar(char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        }

        // Case folded, everything outside [A-Z0-9] dropped. "Sonic the Fighters", "sonic_the
        // _fighters" and "SONICTHEFIGHTERS" are one key on purpose: a difference in how a host
        // spells a title must not silently split its players across two lobby lists.
        uint32_t Normalise(const char* in, char* out, uint32_t cap)
        {
            uint32_t n = 0;
            for (const char* p = in; *p && n + 1 < cap; ++p)
            {
                char c = *p;
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
                if (IsIdChar(c))
                    out[n++] = c;
            }
            out[n] = '\0';
            return n;
        }

        // FNV-1a. Chosen for being four lines long and identical on every compiler: the two peers
        // must derive the same id from the same key, and this runs on both of them.
        uint32_t Hash(const char* s)
        {
            uint32_t h = 2166136261u;
            for (const char* p = s; *p; ++p)
            {
                h ^= static_cast<uint8_t>(*p);
                h *= 16777619u;
            }
            return h;
        }

        // 6 characters of base32 = 30 bits, so the top two are folded back in first rather than
        // dropped. The alphabet is RFC 4648's, which is entirely inside [A-Z0-9] - no padding
        // characters, nothing lowercase, nothing RPCN would reject.
        void EncodeCode(uint32_t hash, char* out)
        {
            static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
            uint32_t h = hash ^ (hash >> 30);
            for (uint32_t i = 0; i < kCodeLength; ++i)
            {
                const uint32_t shift = 5 * (kCodeLength - 1 - i);
                out[i] = kAlphabet[(h >> shift) & 31u];
            }
            out[kCodeLength] = '\0';
        }

        bool CodeIsUsable(const char* code)
        {
            if (!code || strlen(code) != kCodeLength)
                return false;
            for (uint32_t i = 0; i < kCodeLength; ++i)
            {
                if (!IsIdChar(code[i]))
                    return false;
            }
            return true;
        }

        void Compose(const char* code, char out[kBufferSize])
        {
            snprintf(out, kBufferSize, "%s%s_%02u", kPrefix, code, kRevision % 100u);
        }
    }

    bool IsWellFormed(const char* id)
    {
        if (!id)
            return false;
        const size_t len = strlen(id);
        // Fewer than 9 characters leaves a NUL inside the region the server validates; more than
        // 12 would be silently truncated, which is a different id than the caller asked for and
        // is better refused than quietly honoured.
        if (len < 9 || len > kLength)
            return false;
        for (uint32_t i = 0; i < 9; ++i)
        {
            if (!IsIdChar(id[i]))
                return false;
        }
        return true;
    }

    bool LooksLikeComId(const char* id)
    {
        // Exactly the 12-character "XXXXXXXXX_NN" shape. Deliberately stricter than IsWellFormed:
        // this one is asked whether a string IS an id, and every answer of "yes" to a game key
        // costs that game a lobby space of its own.
        if (!IsWellFormed(id) || strlen(id) != kLength)
            return false;
        return id[9] == '_' && id[10] >= '0' && id[10] <= '9' && id[11] >= '0' && id[11] <= '9';
    }

    bool IsStandard(const char* id)
    {
        return LooksLikeComId(id) && strncmp(id, kPrefix, strlen(kPrefix)) == 0;
    }

    bool IsSharedLobbySpace(const char* id)
    {
        return LooksLikeComId(id) && !IsStandard(id);
    }

    bool ForGame(const char* game_key, char out[kBufferSize])
    {
        if (!game_key || !out)
            return false;

        char key[64];
        if (Normalise(game_key, key, sizeof(key)) == 0)
            return false;   // nothing but punctuation - no key at all

        for (const RegistryEntry& e : kRegistry)
        {
            if (strcmp(e.key, key) == 0 && CodeIsUsable(e.code))
            {
                Compose(e.code, out);
                return true;
            }
        }

        char code[kCodeLength + 1];
        EncodeCode(Hash(key), code);
        Compose(code, out);
        return true;
    }

    bool Resolve(const char* input, char out[kBufferSize], const char** out_note)
    {
        const char* note = "";
        bool ok = false;

        if (!input || !*input)
        {
            note = "no communication id or game key was given";
        }
        else if (IsStandard(input))
        {
            strncpy_s(out, kBufferSize, input, _TRUNCATE);
            note = "supplied by the host, already in the YAMP namespace";
            ok = true;
        }
        else if (LooksLikeComId(input))
        {
            // Deliberately not rewritten. Someone pointing the plugin at a real title's lobbies is
            // doing something valid, and a plugin that quietly redirected them would be impossible
            // to reason about. The note is what makes the OTHER case - a host passing one hardcoded
            // id for every game it runs - visible in the log instead of silent.
            strncpy_s(out, kBufferSize, input, _TRUNCATE);
            note = "a fixed id outside the YAMP namespace: every game using it shares one lobby "
                   "list, so pass the game's name instead to get a space of its own";
            ok = true;
        }
        else if (ForGame(input, out))
        {
            note = "derived from the game key";
            ok = true;
        }
        else
        {
            note = "not a usable communication id, and no letters or digits to derive one from";
        }

        if (out_note)
            *out_note = note;
        return ok;
    }
}
