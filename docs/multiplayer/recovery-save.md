# Hidden multiplayer recovery save

The authoritative host writes the current shared campaign to
`SAVEGAME/RECOVERY` when leaving a live network world. This directory is not a
numbered slot and never appears in Fallout's single-player save picker. It uses
the native `SAVE.DAT`, map `.SAV`, and automap files plus the versioned
`MULTI.DAT` sidecar.

The native backup transaction protects the previous recovery generation while
the new legacy files are written. `SAVE.DAT` is digested before `MULTI.DAT` is
published through `MULTI.TMP`/`MULTI.BAK`; a metadata failure restores the
previous legacy generation. Recovery is offered only when the sidecar decodes
and its digest matches the current `SAVE.DAT`.

The host's deliberate exit first saves, then publishes an `Ending` snapshot and
waits for the guest to acknowledge that exact phase revision before closing the
session (with a bounded timeout if the peer is already gone). In the next lobby both players choose a character so their
network roles are authenticated. The host can then choose `RESUME`. The host
loads the hidden native world and restores the saved guest actor, builds,
inventories, ownership records, loot cursors, and shared activity feed. The
guest starts through the normal replica path; snapshot version 19 includes the
authoritative map ID so it loads the recovered map before applying the complete
checkpoint.

Content compatibility hashes exclude mutable `MAPS/*.SAV`, backup/temp files,
generated `.EDG` caches, and `AUTOMAP.DB`. These files share the patch tree with
immutable map content but legitimately differ after each peer has visited or
loaded a map; `.MAP` patches and the other gameplay patch trees remain hashed.

A matching reconnect token claims the saved guest slot exactly. Because the
guest slot is explicitly replaceable, another authenticated guest may claim it;
the claim rotates the token but retains the saved player ID, actor ID, build,
inventory, and distribution cursors. Invalid versions, corrupt metadata,
ownership mismatches, or a mismatched `SAVE.DAT` fail closed.
