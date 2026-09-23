#include "multiplayer/acting_player_context.h"

namespace fallout {
namespace multiplayer {
namespace {

thread_local PlayerCharacterState* currentPlayer = nullptr;
thread_local Object* currentActor = nullptr;

} // namespace

ScopedActingPlayerContext::ScopedActingPlayerContext(PlayerCharacterState& player, Object* actor)
    : _previousPlayer(currentPlayer)
    , _previousActor(currentActor)
{
    currentPlayer = &player;
    currentActor = actor;
}

ScopedActingPlayerContext::~ScopedActingPlayerContext()
{
    currentPlayer = _previousPlayer;
    currentActor = _previousActor;
}

PlayerCharacterState* actingPlayerState()
{
    return currentPlayer;
}

CharacterBuild* actingCharacterBuild()
{
    return currentPlayer != nullptr ? &currentPlayer->build : nullptr;
}

Object* actingPlayerActor()
{
    return currentActor;
}

Object* actingPlayerActorOr(Object* fallback)
{
    return currentActor != nullptr ? currentActor : fallback;
}

CharacterBuild* actingCharacterBuildFor(const Object* actor)
{
    return actor != nullptr && actor == currentActor ? actingCharacterBuild() : nullptr;
}

bool isActingPlayerActor(const Object* actor)
{
    return actingCharacterBuildFor(actor) != nullptr;
}

} // namespace multiplayer
} // namespace fallout
