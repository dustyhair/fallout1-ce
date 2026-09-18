# Object identity audit

Status: decision recorded for Phase 0

## Decision

Do not use `Object::id` as the network identity. Multiplayer uses a separate, host-assigned `EntityId`.

`EntityId` value zero is invalid. The host assigns increasing values and does not reuse them during a session. A local registry maps each `EntityId` to the current `Object*` and optional player owner. Pointers stay local and never enter protocol data.

Save restore, map transitions, and snapshot application can replace an `Object*` while preserving its `EntityId`. They must use the registry's explicit rebind or restore operation.

## Findings

| Code path | Current `Object::id` behavior | Multiplayer consequence |
| --- | --- | --- |
| `obj_new` | Calls `new_obj_id()` | A new engine object normally starts with a new legacy ID |
| `obj_copy` | Copies the full object, then replaces the copied ID with `new_obj_id()` | A split stack or clone becomes a distinct entity |
| `new_obj_id` | Keeps a process-local static counter and checks objects on map tiles | The counter is not saved and the collision scan does not visit nested inventory objects |
| `obj_write_obj` and `obj_read_obj` | Write and read the ID for map objects and nested inventory objects | Most legacy IDs survive ordinary map serialization |
| `obj_load_dude` | Loads the player object, then restores the process-created player's old ID | The ID stored for `obj_dude` does not win during save load |
| `partyMemberAdd` | Replaces the actor ID with a value derived from its prototype ID | Joining the party changes identity, and two actors with the same prototype cannot both join |
| `partyMemberItemSave` | Moves scripted inventory item IDs into a separate range starting at 20000 | A map transition can rewrite item IDs |
| `obj_new_sid_inst` and script object creation | Can assign another new ID after object creation | Script attachment can change an existing object's ID |
| `item_add_force` | When equal stacks merge, destroys the old representative and keeps the incoming object's ID | Stack identity can change during an inventory transaction |
| `item_remove_mult` | For a partial stack removal, preserves the removed object's ID and gives the remainder a copied object with a new ID | One stack becomes two entities |

These behaviors are valid single-player implementation details. Changing them would risk save and script compatibility. A separate multiplayer ID avoids that risk.

## Registry contract

- Register each replicated object before placing its ID in a command, event, or snapshot.
- Reject duplicate entity IDs and duplicate object pointers.
- Record a player owner only for entities controlled by that player. World objects and ordinary items can remain unowned.
- Validate command ownership against the registry, never against a client-supplied legacy object ID.
- Unregister an entity before destroying its object.
- Rebind an entity when load or transition code replaces the local object instance.
- Restore persistent entity IDs from multiplayer save metadata before accepting commands.
- Clear the registry when the session ends. A new session starts allocation at one.

The registry runs on the simulation thread at the safe points defined in the multiplayer plan. It is not a transport callback data structure.

## Integration work still required

The registry is deliberately separate from engine object allocation in this change. The local session controller will own it first. Later changes need explicit hooks for:

- Player actor creation and destruction.
- Map unload and snapshot application.
- Persistent guest actor and inventory restore.
- Stack merge and split results.
- Script-created replicated objects.

The headless tests cover unique creation, clone identity, inventory moves, legacy ID rewrites, pointer replacement during load, save metadata restore, ownership, collisions, and session reset. Engine integration scenarios will exercise the hooks once the local session controller exists.
