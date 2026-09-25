# Multiplayer save sidecar

A multiplayer save keeps Fallout's original slot files intact and adds
`MULTI.DAT` beside `SAVE.DAT`. Developer and live authoritative-host sessions
write the sidecar only while a two-player world is active. A normal
single-player save removes stale multiplayer metadata from that slot after
`SAVE.DAT` succeeds.

The current version 4 sidecar contains:

- A monotonically increasing slot generation.
- An FNV-1a digest of the exact `SAVE.DAT` bytes it belongs to. This is a mismatch and corruption check, not a security boundary.
- A bounded, canonically ordered player roster. The MVP still requires host and
  guest slots, while the representation permits up to 16 saved player slots.
- Each player's printable name and complete `CharacterBuild`: base and bonus stats, skill investments, perk ranks, tagged skills, traits, prototype flags, unspent skill points, level, and experience.
- Every saved player's stable actor ID, reconnect credential, and explicit
  replacement policy. The host remains represented by `SAVE.DAT`; remote actor
  records contain carried and equipped items, stack quantities, weapon
  ammunition, item charges, nested containers, health, and map position.
- The deterministic cap-extra and contested-loot cursors over the saved roster.
- Canonical entity-to-player ownership records and versioned session rules.
- The bounded, ordered shared Pip-Boy activity feed, including its stable IDs,
  source players, kinds, subjects, values, and presentation text.
- A payload length and checksum. The decoder rejects unknown versions, oversized or malformed values, duplicate players, truncation, trailing bytes, and checksum failures before changing session state.

Versions 1, 2, and 3 remain loadable. Version 1 restores both character
builds and starts the guest with an empty inventory; version 2 also restores
the guest object record. Both migrate in memory to a host/guest roster with
actor ownership and zeroed distribution cursors. The next successful
Version 3 restores the repeated player roster, loot cursors, reconnect slots,
and ownership records with an empty durable activity feed. The next successful
multiplayer save writes version 4 without changing `SAVE.DAT`.

Missing remote players may leave an explicitly replaceable slot dormant. A
matching reconnect credential reclaims the slot exactly. A replacement claim
rotates the credential but preserves `PlayerId`, actor identity, object record,
inventory ownership, and loot cursors; the non-replaceable host slot can never
be silently reassigned.

## Save transaction

The legacy save backup remains authoritative. Once the new `SAVE.DAT` closes, the game digests it, encodes both player builds, writes `MULTI.TMP`, and atomically renames it to `MULTI.DAT`. The prior sidecar is held as `MULTI.BAK` during that rename. If metadata capture or publication fails, the save is reported as failed and the existing `SAVE.DAT` backup is restored. The old sidecar remains paired with that restored file.

## Load behavior

With `--multiplayer-dev`, a matching sidecar is decoded after the legacy load
completes. The first subsequent session update recreates the temporary guest
actor, restores both builds against the registered host and guest IDs, binds
presentation to the host, and enters Exploration. Loading during an active
developer session follows the same path after the guest actor is rebound.
Live authoritative hosts also write the v4 sidecar, including the active
reconnect credential, roster ownership, current loot cursors, and shared
activity history. A hidden `SAVEGAME/RECOVERY` directory uses the same native
save transaction without occupying one of the ten visible slots. The host
writes it when leaving a live world. `RESUME` in the multiplayer lobby loads
that save after both players are ready; a matching guest credential reclaims
the saved slot and an allowed replacement rotates the credential while keeping
the saved identity and inventory.

A missing, corrupt, unsupported, or mismatched sidecar never modifies the
original save. The load continues without multiplayer and prints a diagnostic.
Ordinary loads ignore `MULTI.DAT` unless developer multiplayer is enabled; a
connected authoritative host explicitly stages it when resuming recovery.

The guest actor remains `OBJECT_NO_SAVE`; map objects and the original save format are unchanged. The sidecar restores player character state, not a second copy of the world.
