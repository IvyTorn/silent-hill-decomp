# Simple Co-op Mode — design and build plan

This is the plan for the **simple co-op multiplayer mode**: host a lobby, invite
friends over Steam, play the campaign together with up to four players. It is a
*second* online mode that sits alongside the existing **living world** (ghosts +
memos + master server), which stays exactly as it is. Co-op becomes the headline
mode but is gated by a config option so the living world can be run on its own.

## The two modes, and the gate

| | living world | simple co-op (new) |
|---|---|---|
| transport | UDP master server | Steam lobby + Valve relay (P2P) |
| other players | ghosts (a picture) | real players in your world |
| the clock | yours alone | shared; the world cannot be paused |
| entry point | seamless, `online_enabled` | Main menu → **Multiplayer** |

They share almost all of their plumbing: the Steam session layer
(`sh_net_session.c`), the co-op seam (`sh_net_coop.c`, `g_ShNetCoopActive`), the
wire protocol (`sh_net_proto.h`), nameplates and chat (`sh_net_ghost.c`,
`sh_net_chat.c`, `sh_net_ui.c`), and the worker/game-thread split.

**Gate:** `coop_mode` config key (default **1**). It turns on the Multiplayer
main-menu entry and the in-game M menu. Setting it to 0 leaves the build behaving
as the living-world branch did.

## Hosting: no port forwarding, no IP (Steam)

The host creates a Steam lobby and invites friends through the overlay / friends
list. All traffic rides Valve's relay (Steam Datagram Relay), so **nobody
forwards a port and nobody shares an IP**. The IP + port-forward path survives
only as an optional fallback for people not on Steam (the master-server route).
`ShSession_RequestHost` / `RequestJoin` / `RequestInvite` already exist.

---

## Phases

### Phase A — Merge + clean build  ✅ (this session)
Merge the latest `pc-port` (167 commits ahead) into `silenthill-online`, bump the
PsyCross submodule pointer, resolve the three integration conflicts
(`AssemblyInfo.cs`, `dbg_overlay.c`, `pc_console_cmd.c`), get a clean build.

### Phase B — Entry point + config  ✅
- `coop_mode` config flag (default 1).
- Main-menu **Multiplayer** entry, drawn *before* Option. Done.
- Multiplayer menu overlay — rendered through sh_net_ui's clean panel
  (`Nu_DrawCoopMenu`), with **mouse** support (hover/click + own cursor). Done.

> **Status (2026-10-02):** B ✅, C1 ✅ (host setup page), E ✅ (in-game M menu),
> D ✅ core (MP save files + Save/Save & Exit; load + notepad reroute pending),
> no-pause-in-session ✅, G partial ✅ (`status`). Remaining: the game-start boot
> into co-op, the pregame lobby screen + "Killing Time" BGM + minimap, the public
> browser, host kick, and Phase F gameplay sync. See the online branch memory.

### Phase C — Lobby host / join
- **Join:** Steam overlay invite / friends list; a public lobby list if the
  Matchmaking list API is wired (needs a `ShSession_RequestLobbyList` +
  published results — new, small).
- **Host options screen** (before the lobby opens): max players 2–4
  (`onlineSteamMaxPlayers`), private vs public (`onlineSteamPublic`), FPS lock
  30/60 (new lobby metadata, applied to `fpsCap` on all members), and the save
  to load from or **Start Fresh** (Phase D).
- **Pregame lobby screen:** member list, each player's ready state, the host's
  Start button; plays the **"Killing Time"** BGM (the results-screen track);
  minimap available here and shows the other members.

### Phase D — Multiplayer save system
A parallel save namespace that mirrors single-player but with its own files and
custom names. The normal save UI is replaced by this while in co-op.
- Separate files (e.g. `gamedata/coopsaves/<name>.sav`), listed by custom name.
- Host picks a save or **Start Fresh** in the host options.
- End of session: host can save. **Save & Exit** from the M menu cleanly ends the
  session. Save / save-and-quit allowed from anywhere **except boss fights**
  (reuse the quick-save gate) and at notepads (normal save points).
- Guests save **locally** and can save-and-disconnect.
- The stock memcard save path is disabled while `coop_mode` session is active and
  routed to this system.

### Phase E — In-game **M** menu (replaces the quick menu)
Safe options only; nothing that risks desync. Rows:
- **Save & Exit** / **Save** (Phase D).
- **Minimap** (all players; shows other players).
- **Players** — host can **kick**; everyone sees ping / map.
- **Preferences** — on-the-fly toggles that are safe: nameplates on/off, chat
  channel, volume. (The broad PC Options menu is disabled in co-op.)

### Phase F — Gameplay sync (the large one)
- Up to 4 players rendered as real characters (reuse the modeled-ghost render +
  nameplates; promote from "ghost" to "party member").
- **No pause.** Anything that would pause shows the live game underneath — e.g.
  the inventory draws over the running world with its black backdrop removed.
- **Shared key items** (everyone receives a key item pickup), **separate ammo
  pools** (a picked-up ammo stack is granted to every player, but each has their
  own counter).
- **Proximity:** a player may be at most 2 rooms from the party; past that,
  "Please stay with your companions." A player must never be trappable in a
  single-door room.
- **Enemies:** unlimited spawns enabled; host-authoritative. When players share a
  map / a monster's render area, that monster's state is synced to all.

Phase F is itself a substantial networked-simulation project (authoritative enemy
AI, item/ammo events, room-occupancy constraints) and will land incrementally.

### Phase G — Console in co-op
- Console allowed but **never pauses** the game in a session.
- Co-op-oriented commands: `status` (connected players + their info); host-only
  `give` item/weapon and set-flag that apply to **all** players.
- Most PC-options console surface disabled in a session (safe commands only).

---

## Reused building blocks (do not rebuild)
- `sh_net_session.*` — lobby create/join/leave/invite, member list, ping, presence.
- `sh_net_coop.*` — `g_ShNetCoopActive`, `ShNet_PauseBlocked()` (pause seam).
- `sh_net_ghost.*` — world-space player models + nameplates.
- `sh_net_chat.*` / `sh_net_ui.*` — chat + overlay UI, input lockout while typing.
- `sh_net_proto.h` — wire format; add co-op message types in the reserved range.
- `pc_quick_options.c` — the overlay style the Multiplayer/M menus should match.

## Open design questions to settle as Phase C/D land
- Public lobby list: is the Steam matchmaking list API worth wiring now, or start
  invite-only and add the browser later?
- Save format: reuse the exact single-player savegame blob under a new filename
  (simplest, keeps load parity) vs. a co-op-specific container. Leaning: reuse
  the blob, new filename + a small sidecar for the custom name and party info.
