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
| RPCN's protobuf wire format | `source/Protobuf.{h,cpp}` |
| Schannel TLS | `source/TlsClient.{h,cpp}` |
| Plain UDP fallback for LAN testing | `source/Transport.{h,cpp}` |

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

`YAMPNET_ABI_VERSION` is checked the same way, and is currently **10**.

## History

This code was part of YAMP (under `plugin/yampnet/`) until it was split out into this repository;
its commit history came with it.

## On the use of AI in this project

Effectively all of it, as with YAMP itself: the netcode, the RPCN client, the TLS layer and the
documentation were written in [Claude Code](https://claude.com/claude-code) sessions, mostly
Claude Opus 5. The commit history carries `Co-Authored-By: Claude` trailers where that applies.

## Licence

MIT — see [LICENSE](LICENSE). Same licence as YAMP, whose copyright line it carries forward.
