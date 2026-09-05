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
            // Keyed on YAMP's arcadeName (GameRegistry.cpp), which is what NetPlugin's
            // AutoComIdKey() passes, with each game's TAG alongside it - a key is whatever the
            // host sends, and the two spellings must not become two lobby lists.
            //
            // The tag rows deliberately fold the parent title back out. YAMP scopes save data by
            // "VF2" vs "VF2-K2" because Yakuza: Like a Dragon and Kiwami 2 ship separate installs,
            // but the ARCADE GAME is what a match is played against, so both belong in one lobby.

            // Sonic the Fighters - the lockstep netcode's first game, modelled on the PS3 port.
            { "SONICTHEFIGHTERS", "SNCFTR" },
            { "SONICFIGHTERS",    "SNCFTR" },
            { "SONICCHAMPIONSHIP","SNCFTR" },   // the Japanese title of the same board
            { "STF",              "SNCFTR" },
            { "STFGAIDEN",        "SNCFTR" },

            // The other Model 2 fighters, lockstep like StF.
            { "VIRTUAFIGHTER2",   "VRTFT2" },
            { "VF2",              "VRTFT2" },
            { "VF2K2",            "VRTFT2" },
            { "FIGHTINGVIPERS",   "FGTVPR" },
            { "FVIPERS",          "FGTVPR" },   // the romset name
            { "FV",               "FGTVPR" },

            // Motor Raid - Model 2, and a linked-cabinet racer rather than a fighter.
            { "MOTORRAID",        "MTRRAD" },
            { "MR",               "MTRRAD" },
            { "MRGAIDEN",         "MTRRAD" },

            // Cyber Troopers Virtual On - the first game to use the linked-cabinet channel.
            { "VIRTUALON",        "VRTLON" },
            { "CYBERTROOPERS",    "VRTLON" },
            { "VONLINE",          "VRTLON" },
            { "VON",              "VRTLON" },
            { "VONK2",            "VRTLON" },

            // Fighting Vipers 2 - a Model 3 board, same netcode as the Model 2 fighters.
            { "FIGHTINGVIPERS2",  "FGTVP2" },
            { "FVIPERS2",         "FGTVP2" },
            { "FV2",              "FGTVP2" },

            // Sega Racing Classic 2, which is Daytona USA 2 under its re-release name - YAMP's own
            // sources call it Daytona 2 throughout. Linked-cabinet, like Virtual On.
            //
            // ONE code covers the whole lineage, including the ROM factory's two Daytona USA 2
            // variants. Splitting them looks safer and is not: YAMP boots them under a single
            // GameId, so both would send the same key here and only ONE of the two spaces could
            // ever be reached - the other would be an empty room list with no way to explain
            // itself. A variant mismatch is the ROM-revision case below.
            { "SEGARACINGCLASSIC2", "DYTNA2" },
            { "SRC2",             "DYTNA2" },
            { "DAYTONAUSA2",      "DYTNA2" },
            { "DAYTONA2",         "DYTNA2" },

            // Virtua Fighter 5: Final Showdown - the modern-widescreen odd one out, listed so it
            // is not left to the hash if netplay ever reaches it.
            { "VIRTUAFIGHTER5FINALSHOWDOWN", "VRTFT5" },
            { "VIRTUAFIGHTER5",   "VRTFT5" },
            { "VF5FS",            "VRTFT5" },
            { "VF5FSLJ",          "VRTFT5" },
            { "VF5FSYLAD",        "VRTFT5" },
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
