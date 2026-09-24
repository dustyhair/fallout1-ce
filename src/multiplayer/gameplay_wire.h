#ifndef FALLOUT_MULTIPLAYER_GAMEPLAY_WIRE_H_
#define FALLOUT_MULTIPLAYER_GAMEPLAY_WIRE_H_

#include <cstdint>

#include "multiplayer/protocol.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kGameplayWireVersion = 13;

enum class GameplayWireError {
    None,
    WrongMessageKind,
    InvalidSessionId,
    InvalidEnvelopeSequence,
    UnsupportedVersion,
    UnknownPayloadType,
    InvalidLength,
    InvalidReservedField,
    InvalidCommandSequence,
    InvalidEventSequence,
    InvalidPlayerId,
    InvalidEntityId,
    InvalidPhase,
    InvalidPhaseRevision,
    InvalidMove,
    InvalidRotation,
    InvalidSkill,
    InvalidAttack,
    InvalidModal,
    InvalidQuantity,
    InvalidStatus,
    InvalidRejection,
    InconsistentResult,
};

struct GameCommandDecodeResult {
    GameplayWireError error = GameplayWireError::None;
    GameCommand command;

    explicit operator bool() const
    {
        return error == GameplayWireError::None;
    }
};

struct CommandResultDecodeResult {
    GameplayWireError error = GameplayWireError::None;
    CommandResult result;

    explicit operator bool() const
    {
        return error == GameplayWireError::None;
    }
};

struct GameEventDecodeResult {
    GameplayWireError error = GameplayWireError::None;
    GameEvent event;

    explicit operator bool() const
    {
        return error == GameplayWireError::None;
    }
};

GameplayWireError encodeGameCommand(const GameCommand& command, ProtocolEnvelope& envelope);
GameCommandDecodeResult decodeGameCommand(const ProtocolEnvelope& envelope);
GameplayWireError encodeCommandResult(const CommandResult& result, ProtocolEnvelope& envelope);
CommandResultDecodeResult decodeCommandResult(const ProtocolEnvelope& envelope);
GameplayWireError encodeGameEvent(const GameEvent& event, ProtocolEnvelope& envelope);
GameEventDecodeResult decodeGameEvent(const ProtocolEnvelope& envelope);
const char* gameplayWireErrorMessage(GameplayWireError error);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_GAMEPLAY_WIRE_H_ */
