# Player-count expansion audit

Phase 7 re-audited the completed MVP against the expansion guardrails. New
state uses bounded, canonically ordered `PlayerId` rosters where it affects
simulation or persistence. Headless coverage already includes three-player
combat initiative, dialogue majority, activity attribution, shared XP, loot
rotation, and sidecar validation. Exit/scenery/world-map transitions carry
player-keyed placements; sidecar v4 carries repeated players, ownership,
reconnect policy, and loot cursors.

The following pair-shaped debt is intentionally frozen into the two-player v1
release and requires a new protocol/sidecar generation before more players:

| Boundary | Remaining two-player shape | Expansion change |
| --- | --- | --- |
| Bootstrap and transport | One listener peer, one authenticated guest token/connection | Per-player connections, queues, credentials, pins, and failure state |
| Network lobby | One local sheet, one peer sheet, one transport, guest-specific command checks | Roster readiness and independent peer streams |
| Runtime/world bridge | Host/guest IDs and actors are named in orchestration and presentation | Local-player ID plus bounded player/actor maps |
| Elevator wire v13 | Fixed host and guest placement fields | Player-keyed placement vector in a new gameplay-wire version |
| Character/save object bridge | Native `SAVE.DAT` story actor plus one separately stored remote object | Repeated remote object records and migration |
| Direct trade | Exactly two named participants | Keep bilateral trades, but select any two roster members and isolate concurrent negotiations |
| Recovery acknowledgement | One guest event/Ending acknowledgement | Per-recipient acknowledgement and journal retention |
| UI | Two character slots, two ballots/labels in some presentation paths | Roster-driven lobby and presentation widgets |

The v1 guardrails remain: never swap `obj_dude`; never make a guest an AI party
member; keep story/local/acting roles distinct; key ownership and state by
stable IDs; execute rules only on the host; reject unknown roster members; and
use deterministic ordering for every party-wide operation. No v1 decoder may
reinterpret a third player as the guest slot.
