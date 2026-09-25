# Direct trading

Phase 6 starts with a host-owned bilateral negotiation controller. A trade is
identified by a nonzero transaction ID and exactly two player/actor pairs. The
controller stores participants in `PlayerId` order and stores each offer's
registered item IDs in `EntityId` order, giving snapshots and wire messages one
canonical representation.

An offer contains at most 64 distinct registered item stacks plus an explicit
cap quantity. Changing either offer advances the shared revision and clears
both confirmations. A commit plan becomes visible only after both named
participants confirm that same revision. It retains two directional legs
instead of netting quantities, so host validation can prove ownership and item
and cap conservation before changing either inventory.

The engine integration must validate the complete plan and apply both legs as
one host-side transaction before marking it committed. If inventory changed
after confirmation, invalidating the commit advances the revision and requires
fresh confirmations. Either participant may cancel before commit, and either
participant disconnecting cancels the negotiation. A committed revision cannot
be committed again.

Gameplay wire version 30 carries bounded trade commands and complete state
events. Snapshot version 19 restores an interrupted negotiation, including its
offers and confirmations, without moving assets. A successful second
confirmation validates both inventories again, applies both item legs and the
net cap delta on the host, marks the revision committed once, and sends an
immediate authoritative checkpoint. Replicas never replay inventory rules.
Disconnect cancels an active negotiation and returns the shared phase to
exploration.

In a running game, either adjacent player presses `T` to open the shared trade
prompt. `O` replaces that player's offer (caps plus an optional registered item
stack), `C` confirms the displayed revision, and Escape cancels. Semantic test
drivers use `game_trade_begin`, `game_trade_offer`, `game_trade_confirm`, and
`game_trade_cancel`; the machine-readable world state exposes both offers and
the current revision. Direct gifts remain available as a narrow convenience
command, but do not weaken transactional trade validation.

The engine reserves inventory capacity and detaches both outgoing offers before
inserting either into its destination. This prevents Fallout's stack merging
from destroying an item still named by the opposite offer. The installed-data
`trade` fixture exchanges partial Stimpak stacks in both directions plus caps,
then checks conservation and matching complete peer digests.
