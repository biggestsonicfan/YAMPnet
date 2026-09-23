# yampnet

The netplay plugin for [YAMP](https://github.com/biggestsonicfan/YAMP) — a single
`yampnet.dll` that adds online play to the Sega arcade boards YAMP hosts, and that YAMP works
perfectly well without.

It lives in its own repository for the same reason it is a DLL rather than a static library: the
netcode is expected to churn long after the emulator half is stable, and a YAMP release must be
able to ship with no netplay in it at all. YAMP never links against this. It `LoadLibrary`s the
DLL at runtime and disables the feature when it is missing or refuses to load, so *not shipping
`yampnet.dll`* is the entire "exclude netplay" switch.

## What it does

Matchmaking runs over [**RPCN**](https://github.com/RipleyTom/rpcn), the community server for
PlayStation Network emulation: a TLS session for login and the room list, a UDP address-discovery
exchange, and then **direct peer-to-peer game traffic that never passes through the server**.
Rooms carry per-game settings owned by the host, optional passwords, and join-by-ID.

Two netcodes, chosen per game by what the board actually needs:

* **Delay-based lockstep** (`Lockstep.cpp`, `PadCodec.cpp`) for the fighting games, modelled on
  the Sonic the Fighters PS3 port (NPUB30927): inputs keyed by absolute frame into per-player
  rings, every packet redundantly re-carrying the last N frames so loss repairs itself, a round
  start barrier, an adjustable frame delay, and one shared match seed so both peers' RNG streams
  agree.
* **The arcade's own linked-cabinet protocol**, tunnelled as raw datagrams, for the games that
  were designed as two machines with a comm board between them. No lockstep and no shared seed —
  the boards talk to each other the way they did in the arcade.

| Layer | Files |
| --- | --- |
| Plugin ABI entry points | `source/Plugin.cpp` |
| Lockstep engine | `source/Lockstep.{h,cpp}` |
| Pad encode/decode (the determinism rule) | `source/PadCodec.{h,cpp}` |
| RPCN client — login, rooms, signaling | `source/RpcnClient.{h,cpp}`, `source/RpcnTransport.{h,cpp}` |
| Account registration | `source/Account.{h,cpp}` |
| Twitch sign-in (OAuth device flow) | `source/TwitchAuth.{h,cpp}` |
| Per-game Communication IDs | `source/ComId.{h,cpp}` |
| RPCN's protobuf wire format | `source/Protobuf.{h,cpp}` |
| Schannel TLS | `source/TlsClient.{h,cpp}` |
| Plain UDP fallback for LAN testing | `source/Transport.{h,cpp}` |

## One lobby space per game

RPCN partitions everything by **Communication ID**: the server list, the world list, room
creation, room search. Two clients see each other's rooms only if their ComIds match byte for
byte, so a build that hardcodes one id — the PS3 port's `NPWR02113_00`, say — drops every game
YAMP hosts into a single lobby list. A Virtual On cabinet then advertises next to a Sonic the
Fighters one, both look joinable, and the mismatch only shows up once the netcodes are already
talking. `source/ComId.{h,cpp}` is the standard that prevents that.

```
YMP SNCFTR _00      12 characters
^^^ ^^^^^^  ^^
|   |       netcode revision channel
|   six-character game code
the YAMP namespace — never a real PSN title's prefix
```

Nine characters is the whole of the usable namespace: RPCN reads the ComId as exactly 12 bytes and
rejects the request as *Malformed* unless the first 9 are ASCII uppercase letters or digits. It
does not police the last three, and PSN's own ids spell them `_NN`.

| Game | ComId | Keys it answers to |
| --- | --- | --- |
| Sonic the Fighters | `YMPSNCFTR_00` | `Sonic the Fighters`, `StF`, `StF-Gaiden` |
| Virtua Fighter 2 | `YMPVRTFT2_00` | `Virtua Fighter 2`, `VF2`, `VF2-K2` |
| Fighting Vipers | `YMPFGTVPR_00` | `Fighting Vipers`, `FV`, `fvipers` |
| Fighting Vipers 2 | `YMPFGTVP2_00` | `Fighting Vipers 2`, `FV2` |
| Motor Raid | `YMPMTRRAD_00` | `Motor Raid`, `MR`, `MR-Gaiden` |
| Cyber Troopers Virtual On | `YMPVRTLON_00` | `Virtual On`, `VON`, `VON-K2` |
| Sega Racing Classic 2 (Daytona USA 2) | `YMPDYTNA2_00` | `Sega Racing Classic 2`, `SRC2`, `Daytona USA 2` |
| Virtua Fighter 5: Final Showdown | `YMPVRTFT5_00` | `Virtua Fighter 5: Final Showdown`, `VF5FS` |
| anything else | derived — see below | |

Those keys are YAMP's own: `GameRegistry.cpp`'s `arcadeName` is what `net::AutoComIdKey()` sends,
and each game's tag is listed beside it so either spelling lands in the same place. The tag rows
fold the parent title back out on purpose — YAMP scopes save data by `VF2` vs `VF2-K2` because
Yakuza: Like a Dragon and Kiwami 2 ship separate installs, but the arcade game is what a match is
played against, so both belong in one lobby.

Revisions of a single ROM share a space too, and that granularity is deliberate: telling a peer
its ROM revision differs is the room attribute word's job, where the joiner can be shown why, not
a namespace's — a separate space would only hide the mismatch behind an empty room list.

A game that is not in that table is **not** left to share someone else's space: its code is a
base32 hash of its key, so it gets one of its own with no central registry to agree on and no
`servers.cfg` edit — RPCN's `CreateMissing` registers a ComId the first time it is asked about.
Naming a game in the table only buys a readable id, and *moves* it, so add a game before people
play it or leave it derived.

**The game key is part of the standard.** The plugin learns nothing about the title through the
ABI, so the key is whatever YAMP passes in `yampnet_rpcn_config::communication_id`, and
`comid::Resolve()` sorts out what it got:

* an id already in the `YMP` namespace — used as-is,
* any other id in canonical `XXXXXXXXX_NN` form — used as-is, and **logged as a warning**, because
  wanting a real title's lobbies is legitimate and hardcoding one id for every game looks exactly
  the same from here,
* anything else — read as a game key and turned into an id.

Keys are case- and punctuation-insensitive (`Sonic the Fighters`, `sonic_the_fighters` and `STF`
are one key), and the table carries aliases, because two spellings would otherwise be two lobby
lists. A ROM or romset name makes a better key than a display title, which gets retitled.

The `_NN` tail is a compatibility channel, bumped only for a change that breaks peer-to-peer
compatibility with the previous build. It is not `YAMPNET_ABI_VERSION`: that one guards
host-to-plugin compatibility and is refused at load. This one guards plugin-to-plugin
compatibility, where the only enforcement available is that mismatched builds never see the same
rooms — so bumping it strands everyone still on the old build, which is the point.

## Getting a build

You do not have to build the plugin yourself. Every push to `master` is compiled by GitHub Actions
against a YAMP checkout and the DLL is attached to the run:

1. Open the [**Actions**](https://github.com/biggestsonicfan/YAMPnet/actions) tab.
2. Click the newest **build** run with a green tick. A red cross means that commit did not
   compile — take the newest green run below it instead.
3. Scroll to **Artifacts** at the bottom of the run summary and download **`yampnet-Release`**.

Unzip it and put `yampnet.dll` **beside `YAMP.exe`**. That is the only place YAMP looks, on
purpose — the loader does not search `PATH`, so a stray copy elsewhere on the system can never be
picked up instead. Restart YAMP; the **Netplay** settings page reports whether it loaded, and says
so plainly if it did not.

`yampnet-Debug` is the same commit built unoptimized, worth taking only when chasing a netplay
bug. The `.pdb` in either zip is not needed to play — it carries the symbols that turn a crash
address into a function name and line number.

Two GitHub facts, neither of them a setting in this repository: you must be **signed in** to
download an artifact even though this repository is public, and artifacts are **deleted after 90
days**, after which the run is still listed but its downloads are gone.

### Match it to your YAMP

A plugin and a YAMP build from wildly different dates may refuse to work together, and that is the
[layout handshake](#the-layout-handshake) doing its job rather than a bug: the plugin writes the
emulator's pad structures itself, so YAMP rejects one compiled against layouts that no longer
match. You will not be left guessing — the Netplay page says *"The plugin was found but rejected"*
and names the reason. Take a newer DLL, or a YAMP build from nearer the plugin's date.

## Building

This needs a YAMP checkout for its headers — see below — and then:

```
premake5.exe --yamp-dir="../YAMP/source" vs2022
msbuild build/YampNet.vcxproj -p:Configuration="Release Win64" -p:Platform=x64 -m
```

Note the `.vcxproj`, not the `.sln`: premake maps the `Release`/`Win64` **solution** configuration
onto a `Release Win64`/x64 **project** one, and MSBuild wants the project's names. Handing the
solution `Release Win64` fails with "the specified solution configuration is invalid".

C++17, Visual Studio 2022, `Debug` / `Release` / `Master` on the `Win64` platform. The output is
`build/bin/Win64/<config>/yampnet.dll`; **copy it next to `YAMP.exe`**, which is where YAMP's
loader looks.

`premake5.lua` is the source of truth for the project files. Do not hand-edit anything under
`build/`; edit the Lua and regenerate.

## What it needs from YAMP

Three headers, resolved through an include path rather than vendored here:

| Header | What it is |
| --- | --- |
| `source/net/YampNet.h` | the plugin ABI — the whole contract between the two halves |
| `source/pxd/LJ/sl.h` (+ `pxd_types.h`, `sl_internal.h`) | the engine's pad struct |
| `source/m2ftg/m2ftg.h` | the arcade `execute_info` block |

The plugin writes `execute_info.pad[]` itself — a deliberate scope choice — so it is coupled to
those layouts. Copying them into this repository would let the two drift silently; pointing at a
checkout means a rebuild picks the change up immediately.

Point premake at YAMP's `source/` directory, in this order of precedence:

1. `premake5 --yamp-dir="C:/src/YAMP/source" vs2022`
2. the `YAMP_DIR` environment variable
3. `../YAMP/source` — the default, a sibling checkout

The first of those that is **set** is the one used — a path you named that turns out to be wrong
is a hard error, not a quiet fall-through to the next candidate, because building against a
different YAMP tree than the one you asked for is worse than not building. Either way premake
stops with a message naming the path it tried and where that path came from, rather than letting
the failure surface as a missing include three minutes into a compile.

### The layout handshake

Because the coupling above is real, `YampNet.h` carries a `yampnet_layout` struct: the host
declares the struct sizes and field offsets it was built with, the plugin compares them against
its own, and a mismatch is refused cleanly at load time. That is the backstop for the case where
only one of the two halves got rebuilt — without it, a stale plugin would write pad bytes at the
wrong offsets into the emulator's memory. Keep it honest when either layout changes.

`YAMPNET_ABI_VERSION` is checked the same way, and is currently **13**.

## The official server

YAMP can sign in to the official RPCN server as well as ours. That is **np.rpcs3.net**, the one
RPCS3 itself uses. m2-hle2 works this way too. Three things are different there:

* **Its certificate is self-signed** (`CN=RPCN`, valid to 2030-07-21), so the ordinary check
  used when no fingerprint is set can never accept it. Its pin is built into the plugin
  (`kRpcnOfficialFingerprint`) and is used whenever the player has not set one of their own, so
  the host name is all anyone has to type. A fingerprint the player sets still wins.
* **It has no Twitch sign-in** (see [below](#a-server-that-does-not-offer-it)). Accounts there are
  an npid and a password.
* **Accounts belong to one server.** An account on ours does not exist there, and neither does a
  Twitch login token. A token is a password, so it must only ever go to the server that issued it.
  The plugin does not store credentials; YAMP does, and keeping each one with its own server is
  YAMP's job.

The lobbies are still YAMP's own. Every ComId is in the `YMP` namespace (see
[above](#one-lobby-space-per-game)), so a YAMP room never shows up in RPCS3's lobbies and theirs
never show up in ours, on either server.

## Signing up

RPCN's `Create` command runs on its own connection and before any login, so the plugin can
register an account for a player who has none — which is otherwise a strange first step for
someone whose only PlayStation-anything is this emulator. `Account.{h,cpp}` is that: one
connection, one request, one reply, with its own timeout so a server that accepts the socket and
then says nothing still resolves to an error a UI can show.

It is deliberately not part of `RpcnTransport`. That class is a logged-in session with discovery,
a room, a signaling socket and a peer — none of which exist yet, and none of which a sign-up
should be able to disturb. Nothing here touches `get_state()`: a session that is IDLE stays IDLE
while an account is being made.

The server requires five non-empty strings and YAMP asks for three of them. The online name
defaults to the account name, and the avatar URL to this project's page — a second display name is
a question with no useful answer for someone with one account, and an avatar is a URL nobody has
to hand.

## The verification token

RPCN's login takes **three** things, and the third is not a second password: `Login` carries
`(npid, password, token)`, where the token is the 16 hexadecimal characters a server **mails**
when the account is created. `yampnet_rpcn_config` names them apart — `password` and `token` —
which is what ABI 12 is for: the field called `token` was carrying the password.

**Empty is the normal value.** The server compares the token only when *it* has e-mail validation
switched on, which is off by default and off on most community servers: `cmd_account.rs` passes
`is_email_validated()` as `check_user`'s `check_token` argument. A client is never told which sort
of server it has reached, so the plugin sends whatever the player has and lets the server decide.
That is also why nothing here refuses to connect without one — the refusal belongs to the server,
and a client-side rule would lock every player out of every server that does not validate.

What the plugin does instead is make the answer legible. A refused login used to read
`login rejected (ErrorType=9)`; RPCN distinguishes the three credentials
(`LoginInvalidUsername`, `LoginInvalidPassword`, `LoginInvalidToken`) and each now says which box
to correct — including the case worth splitting in two, where the token was refused because none
was sent at all. Tokens are pasted out of e-mail, so they are trimmed of whitespace and
upper-cased when they are entirely hexadecimal, matching what the server stores; anything else is
sent unchanged with a warning in the log, because the format belongs to the server and this must
never be the thing that locks someone out of one that changed it.

### When the e-mail never arrives

An account registered here that cannot log in until a token arrives is an account with no way
out — so `SendToken` (command 4) is exposed as well, and `AccountMaker` runs it as its second
job: same connection, same timeout, same reporting, with `GetJob()` saying which of the two
succeeded so a UI can tell *account created* from *token sent*.

It authenticates on the account name and password with `check_token = false` — you do not need
the token to ask for the token, which is the entire point of the command. The server refuses it
outright (`Invalid`) if it does no e-mail validation, and once per account per 24 hours
(`TooSoon`); both arrive as a message rather than a state of their own.

## Signing in with Twitch

Everything above assumes an RPCN account, and an account is a thing someone has to be talked
into making. RPCN also accepts an [OAuth device code](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#device-code-grant-flow)
grant against Twitch, which skips the question entirely for a player who already has a Twitch
login — and for an arcade emulator that is most of them. `TwitchAuth.{h,cpp}` is the client
half.

The exchange is two commands, both unauthenticated like `Create`, on their own connection:

```
TwitchDeviceStart      ->  flow_id, user_code, verification_uri, expires_in, interval
  show the code, open the page, then every `interval` seconds:
TwitchDevicePoll(flow) ->  TwitchAuthPending ... until it is not
  on success:              npid, online_name, avatar_url, login_token
```

**The login token is the password from then on.** It goes in
`yampnet_rpcn_config::password`, the verification token field stays empty — a login that
arrives on a Twitch token is not checked against one — and `connect()` needs no change at all.
That is the whole reason this is usable from a game: the browser trip happens once, not once
per session. A completed flow issues a *fresh* token and invalidates the one before it, so it
is the credential rather than a second one kept alongside a password.

No Twitch credential ever reaches the plugin. The server does the talking to Twitch and keeps
the device code; a client only ever holds an opaque flow id and a code to put on screen.

### Waiting is the normal state

Every other request in this plugin resolves in a round trip. This one waits for a person to
find a browser, so `WAITING` can legitimately last until the code expires — half an hour — and
a timeout that "always resolves" would be the bug rather than the safeguard. What is bounded
is everything either side of it: the device request, each individual poll, and the code itself.

Polling is paced by the interval the server hands out, which is also what keeps the connection
alive: an unauthenticated socket normally gets ten seconds between packets, and RPCN relaxes
that to 120 once a flow is running.

### A server that does not offer it

This is deliberately **not** behind a protocol version bump on the server side, so an unpatched
client still connects to a patched server and vice versa. The cost is that there is no way to
ask whether a server has the feature — only to try it — and the three ways of finding out are
different:

* a server with it compiled in but not configured answers `TwitchDisabled` (34) and keeps the
  connection,
* a server that predates it does not know command 63 at all, answers `Malformed` and **hangs
  up**,
* a server whose command 63 is some *other* command answers `Invalid` (2), which is what a
  client that has not logged in gets for any command it may not send yet, and keeps the
  connection. That is the official server, np.rpcs3.net. A server with Twitch never answers the
  start with `Invalid`.

All three land in `YAMPNET_TWITCH_UNSUPPORTED`, which is a state of its own rather than a failure:
the answer to any of them is to offer the password boxes, not to show an error about something
nobody did wrong. Every *other* Twitch error code leaves the connection open on purpose, so a
refused sign-in never costs a player the ability to log in the ordinary way.

## History

This code was part of YAMP (under `plugin/yampnet/`) until it was split out into this repository;
its commit history came with it.

## On the use of AI in this project

Effectively all of it, as with YAMP itself: the netcode, the RPCN client, the TLS layer and the
documentation were written in [Claude Code](https://claude.com/claude-code) sessions, mostly
Claude Opus 5. The commit history carries `Co-Authored-By: Claude` trailers where that applies.

## Licence

MIT — see [LICENSE](LICENSE). Same licence as YAMP, whose copyright line it carries forward.
