#ifndef FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_
#define FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_

#include <optional>

#include "multiplayer/character_lobby.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/snapshot.h"

namespace fallout {

struct Object;

namespace multiplayer {

bool networkWorldEnter(NetworkLaunchMode mode,
    const CharacterCreationSheet& localSheet,
    const CharacterCreationSheet& peerSheet);
bool networkWorldApplyPeerMove(const ActorMovementStartedEvent& movement);
bool networkWorldApplyPeerFacing(const ActorFacingChangedEvent& facing);
bool networkWorldApplyPeerDoorUse(const DoorUseStartedEvent& doorUse);
bool networkWorldApplyPeerPickup(const ItemPickupStartedEvent& pickup);
bool networkWorldBeginLocalPickup(Object* target);
void networkWorldFinishPickup(Object* target, bool succeeded);
AuthoritativeCommandResult networkWorldProcessCommand(const GameCommand& command);
SessionPhase networkWorldPhase();
std::uint32_t networkWorldPhaseRevision();
bool networkWorldCaptureSnapshot(EventSequence lastIncludedEvent, WorldSnapshot& snapshot);
bool networkWorldApplySnapshot(const WorldSnapshot& snapshot);
std::optional<EntityId> networkWorldFindEntity(const Object* object);
void networkWorldLeave();
bool networkWorldActive();

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_WORLD_H_ */
