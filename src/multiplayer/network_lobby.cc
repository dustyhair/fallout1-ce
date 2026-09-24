#include "multiplayer/network_lobby.h"

#include <limits>
#include <utility>
#include <variant>

#include "multiplayer/gameplay_wire.h"
#include "multiplayer/protocol.h"

namespace fallout {
namespace multiplayer {
namespace {

constexpr std::size_t kLobbyHeaderSize = 4;
constexpr std::size_t kChatHeaderSize = 6;
constexpr std::size_t kMaxQueuedChatMessages = 32;
constexpr std::uint16_t kRecoveryWireVersion = 1;
constexpr std::size_t kRecoveryHeaderSize = 4;

enum class RecoveryMessageType : std::uint8_t {
    Request = 1,
    Snapshot = 2,
    Complete = 3,
};

void appendUInt16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUInt32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUInt64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::uint16_t readUInt16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t readUInt32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t readUInt64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int index = 0; index < 8; index++) {
        value = (value << 8) | bytes[offset + index];
    }
    return value;
}

bool isKnownCharacterLobbyError(CharacterLobbyError error)
{
    return error > CharacterLobbyError::None && error <= CharacterLobbyError::TrailingData;
}

bool isValidChatText(const std::string& text)
{
    if (text.empty() || text.size() > kMaxLobbyChatMessageLength) {
        return false;
    }
    for (unsigned char ch : text) {
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
    }
    return true;
}

EntityId eventActorId(const GameEventPayload& payload)
{
    return std::visit([](const auto& event) {
        return event.actorId;
    }, payload);
}

bool isSupportedLiveEvent(const GameEventPayload& payload)
{
    return std::holds_alternative<ActorMovementStartedEvent>(payload)
        || std::holds_alternative<ActorFacingChangedEvent>(payload)
        || std::holds_alternative<DoorUseStartedEvent>(payload)
        || std::holds_alternative<ItemPickupStartedEvent>(payload)
        || std::holds_alternative<ItemPickupCompletedEvent>(payload)
        || std::holds_alternative<LootStartedEvent>(payload)
        || std::holds_alternative<SkillUseStartedEvent>(payload)
        || std::holds_alternative<ItemUseStartedEvent>(payload)
        || std::holds_alternative<ElevatorTransitionedEvent>(payload)
        || std::holds_alternative<ExitGridTransitionedEvent>(payload)
        || std::holds_alternative<SceneryTransitionedEvent>(payload)
        || std::holds_alternative<RestStateChangedEvent>(payload)
        || std::holds_alternative<InventoryTransferredEvent>(payload)
        || std::holds_alternative<ItemDroppedEvent>(payload)
        || std::holds_alternative<AttackStartedEvent>(payload)
        || std::holds_alternative<SharedModalStateChangedEvent>(payload);
}

} // namespace

bool NetworkLobby::start(NetworkLaunchMode mode, SessionId sessionId, std::unique_ptr<Transport> transport)
{
    stop();
    _mode = mode;
    _sessionId = sessionId;
    _error = NetworkLobbyError::None;
    _sheetError = CharacterLobbyError::None;
    _nextSendSequence = 2;
    _nextReceiveSequence = 2;
    _localSheet.reset();
    _peerSheet.reset();
    _startRequested = false;
    _nextLocalCommandSequence = 1;
    _nextEventSequence = 1;
    _nextExpectedEventSequence = 1;
    _lastAppliedEventSequence = {};
    _pendingCommandSequences.clear();
    _recoveryRequests.clear();
    _peerCommands.clear();
    _commandResults.clear();
    _peerEvents.clear();
    _chatMessages.clear();
    _peerSnapshots.clear();
    _authoritativeStates.clear();
    _recovering = false;
    _eventJournal.clear();

    if ((mode != NetworkLaunchMode::Host && mode != NetworkLaunchMode::Join)
        || sessionId.value == 0
        || transport == nullptr
        || !transport->isConnected()) {
        _state = NetworkLobbyState::Failed;
        _error = NetworkLobbyError::InvalidStart;
        return false;
    }

    _transport = std::move(transport);
    _state = NetworkLobbyState::Waiting;
    return true;
}

bool NetworkLobby::sendLocalMove(
    int destinationTile,
    int elevation,
    bool running,
    int startingTile,
    const std::vector<std::uint8_t>& path,
    std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        ActorMovementStartedEvent { actorId, destinationTile, elevation, running, startingTile, path },
        MoveCommand { destinationTile, elevation, running },
        phaseRevision);
}

bool NetworkLobby::sendLocalDoorUse(EntityId targetId, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        DoorUseStartedEvent { actorId, targetId },
        InteractCommand { targetId },
        phaseRevision);
}

bool NetworkLobby::sendLocalPickup(EntityId targetId, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        ItemPickupStartedEvent { actorId, targetId },
        PickupCommand { targetId },
        phaseRevision);
}

bool NetworkLobby::sendLocalLoot(EntityId targetId, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        LootStartedEvent { actorId, targetId },
        LootCommand { targetId },
        phaseRevision);
}

bool NetworkLobby::sendLocalSkillUse(EntityId targetId, ExplorationSkill skill, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        SkillUseStartedEvent { actorId, targetId, skill },
        UseSkillCommand { targetId, skill },
        phaseRevision);
}

bool NetworkLobby::sendLocalItemUse(EntityId itemId, EntityId targetId, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        ItemUseStartedEvent { actorId, itemId, targetId },
        UseItemOnCommand { itemId, targetId },
        phaseRevision);
}

bool NetworkLobby::sendLocalElevator(std::int32_t elevatorType, std::int32_t destinationLevel, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        ElevatorTransitionedEvent { actorId, elevatorType, 0, 0, 0, 0, 1, 0, 0, phaseRevision },
        ElevatorCommand { elevatorType, destinationLevel },
        phaseRevision);
}

bool NetworkLobby::sendLocalExitGrid(EntityId exitId, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        ExitGridTransitionedEvent {
            actorId,
            exitId,
            0,
            { PlayerTransitionPlacement { playerId, actorId, 0, 0, 0 } },
            phaseRevision,
        },
        ExitGridCommand { exitId },
        phaseRevision);
}

bool NetworkLobby::sendLocalSceneryTransition(EntityId transitionId, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        SceneryTransitionedEvent {
            actorId,
            transitionId,
            0,
            { PlayerTransitionPlacement { playerId, actorId, 0, 0, 0 } },
            phaseRevision,
        },
        SceneryTransitionCommand { transitionId },
        phaseRevision);
}

bool NetworkLobby::sendLocalRest(std::int32_t minutes, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        RestStateChangedEvent { actorId, minutes, false, 1, phaseRevision },
        RestCommand { minutes },
        phaseRevision);
}

bool NetworkLobby::sendLocalAttack(EntityId targetId, std::int32_t hitMode, std::int32_t hitLocation, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        AttackStartedEvent { actorId, targetId, hitMode, hitLocation },
        AttackCommand { targetId, hitMode, hitLocation },
        phaseRevision);
}

bool NetworkLobby::sendLocalSharedModal(SharedModalKind kind, bool open, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    SessionPhase phase = open ? sharedModalPhase(kind) : SessionPhase::Exploration;
    return sendLocalAction(
        SharedModalStateChangedEvent { actorId, kind, open, phase, phaseRevision },
        SharedModalCommand { kind, open },
        phaseRevision);
}

bool NetworkLobby::sendLocalInventoryTransfer(EntityId sourceId,
    EntityId destinationId,
    EntityId itemId,
    std::uint32_t quantity,
    std::uint32_t sourceQuantity,
    std::uint32_t phaseRevision,
    EntityId remainderItemId,
    ItemDescriptor itemDescriptor)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        InventoryTransferredEvent { actorId, sourceId, destinationId, itemId, quantity, sourceQuantity, remainderItemId, itemDescriptor },
        InventoryTransferCommand { sourceId, destinationId, itemId, quantity, sourceQuantity, itemDescriptor },
        phaseRevision);
}

bool NetworkLobby::sendLocalItemDrop(EntityId sourceId,
    EntityId itemId,
    std::uint32_t quantity,
    std::uint32_t sourceQuantity,
    std::uint32_t phaseRevision,
    EntityId remainderItemId,
    std::int32_t tile,
    std::int32_t elevation,
    ItemDescriptor itemDescriptor)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        ItemDroppedEvent { actorId, sourceId, itemId, quantity, sourceQuantity, remainderItemId, tile, elevation, itemDescriptor },
        ItemDropCommand { sourceId, itemId, quantity, sourceQuantity, itemDescriptor },
        phaseRevision);
}

bool NetworkLobby::sendLocalFacing(int rotation, std::uint32_t phaseRevision)
{
    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    EntityId actorId { playerId.value };
    return sendLocalAction(
        ActorFacingChangedEvent { actorId, rotation },
        FaceCommand { rotation },
        phaseRevision);
}

bool NetworkLobby::sendLocalAction(
    GameEventPayload eventPayload,
    GameCommandPayload commandPayload,
    std::uint32_t phaseRevision)
{
    bool connected = _state == NetworkLobbyState::Ready && _transport != nullptr;
    bool hostContinuing = _mode == NetworkLaunchMode::Host && _state == NetworkLobbyState::Disconnected;
    if ((!connected && !hostContinuing) || !_startRequested) {
        return false;
    }
    if (_mode == NetworkLaunchMode::Join && _recovering) {
        return false;
    }

    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    if (_mode == NetworkLaunchMode::Join) {
        GameCommand command;
        command.sequence.value = _nextLocalCommandSequence;
        command.playerId = playerId;
        command.actorId = EntityId { playerId.value };
        const SharedModalCommand* modal = std::get_if<SharedModalCommand>(&commandPayload);
        command.expectedPhase = std::holds_alternative<AttackCommand>(commandPayload)
            ? SessionPhase::Combat
            : modal != nullptr && !modal->open
            ? sharedModalPhase(modal->kind)
            : SessionPhase::Exploration;
        command.expectedPhaseRevision = phaseRevision;
        command.payload = std::move(commandPayload);

        ProtocolEnvelope envelope;
        envelope.sessionId = _sessionId;
        envelope.sequence = _nextSendSequence++;
        if (encodeGameCommand(command, envelope) != GameplayWireError::None
            || !sendGameplayEnvelope(std::move(envelope))) {
            return false;
        }
        _pendingCommandSequences.push_back(command.sequence);
        _nextLocalCommandSequence++;
        return true;
    }

    GameEvent event;
    event.sequence.value = _nextEventSequence;
    event.causedBy.value = _nextLocalCommandSequence;
    event.payload = std::move(eventPayload);
    if (!sendAuthoritativeEvent(std::move(event))) {
        return false;
    }
    _nextLocalCommandSequence++;
    return true;
}

bool NetworkLobby::sendCommandOutcome(AuthoritativeCommandResult outcome)
{
    if (_mode != NetworkLaunchMode::Host
        || (_state != NetworkLobbyState::Ready && _state != NetworkLobbyState::Disconnected)
        || !_startRequested
        || (_state == NetworkLobbyState::Ready && _transport == nullptr)) {
        return false;
    }

    if (outcome.event.has_value() && !outcome.replayed) {
        outcome.event->sequence.value = _nextEventSequence;
        outcome.result.firstEventSequence = outcome.event->sequence;
        outcome.result.eventCount = 1;
    }

    if (_state == NetworkLobbyState::Ready) {
        ProtocolEnvelope envelope;
        envelope.sessionId = _sessionId;
        envelope.sequence = _nextSendSequence++;
        if (encodeCommandResult(outcome.result, envelope) != GameplayWireError::None
            || !sendGameplayEnvelope(std::move(envelope))) {
            if (_state != NetworkLobbyState::Disconnected) {
                return false;
            }
        }
    }
    return !outcome.event.has_value()
        || outcome.replayed
        || sendAuthoritativeEvent(std::move(*outcome.event));
}

bool NetworkLobby::publishLocalCommandOutcome(AuthoritativeCommandResult outcome)
{
    if (_mode != NetworkLaunchMode::Host
        || (_state != NetworkLobbyState::Ready && _state != NetworkLobbyState::Disconnected)
        || !_startRequested) {
        return false;
    }
    if (outcome.result.status == CommandStatus::Rejected) {
        return true;
    }
    if (outcome.replayed) {
        return true;
    }
    if (!outcome.event.has_value()) {
        return outcome.result.eventCount == 0;
    }
    outcome.event->sequence.value = _nextEventSequence;
    return sendAuthoritativeEvent(std::move(*outcome.event));
}

bool NetworkLobby::publishDeferredEvent(GameEvent event)
{
    if (_mode != NetworkLaunchMode::Host
        || (_state != NetworkLobbyState::Ready && _state != NetworkLobbyState::Disconnected)
        || !_startRequested) {
        return false;
    }
    event.sequence.value = _nextEventSequence;
    return sendAuthoritativeEvent(std::move(event));
}

bool NetworkLobby::sendAuthoritativeEvent(GameEvent event)
{
    if (_mode != NetworkLaunchMode::Host
        || (_state != NetworkLobbyState::Ready && _state != NetworkLobbyState::Disconnected)
        || event.sequence.value != _nextEventSequence) {
        return false;
    }

    ProtocolEnvelope envelope;
    envelope.sessionId = _sessionId;
    envelope.sequence = _nextSendSequence++;
    if (encodeGameEvent(event, envelope) != GameplayWireError::None
        || _eventJournal.append(event) != EventJournalError::None) {
        fail(NetworkLobbyError::EncodeFailed);
        return false;
    }
    if (_state == NetworkLobbyState::Ready && !sendGameplayEnvelope(std::move(envelope))) {
        if (_state != NetworkLobbyState::Disconnected) {
            return false;
        }
    }
    _nextEventSequence++;
    return true;
}

bool NetworkLobby::sendGameplayEnvelope(ProtocolEnvelope envelope)
{
    Packet packet;
    if (encodeEnvelope(envelope, packet) != ProtocolError::None) {
        fail(NetworkLobbyError::EncodeFailed);
        return false;
    }
    if (_transport == nullptr) {
        fail(NetworkLobbyError::SendFailed);
        return false;
    }
    TransportSendResult sent = _transport->send(std::move(packet));
    if (sent != TransportSendResult::Sent) {
        if (sent == TransportSendResult::Disconnected
            && _state == NetworkLobbyState::Ready
            && _startRequested) {
            markDisconnected();
        } else {
            fail(NetworkLobbyError::SendFailed);
        }
        return false;
    }
    return true;
}

std::optional<GameCommand> NetworkLobby::takePeerCommand()
{
    if (_peerCommands.empty()) {
        return std::nullopt;
    }
    GameCommand command = std::move(_peerCommands.front());
    _peerCommands.pop_front();
    return command;
}

std::optional<CommandResult> NetworkLobby::takeCommandResult()
{
    if (_commandResults.empty()) {
        return std::nullopt;
    }
    CommandResult result = _commandResults.front();
    _commandResults.pop_front();
    return result;
}

std::optional<GameEvent> NetworkLobby::takePeerEvent()
{
    if (_peerEvents.empty()) {
        return std::nullopt;
    }

    GameEvent event = std::move(_peerEvents.front());
    _peerEvents.pop_front();
    return event;
}

bool NetworkLobby::confirmPeerEventApplied(EventSequence sequence)
{
    if (_mode != NetworkLaunchMode::Join
        || sequence.value == 0
        || _lastAppliedEventSequence.value == std::numeric_limits<std::uint64_t>::max()
        || sequence.value != _lastAppliedEventSequence.value + 1) {
        return false;
    }
    _lastAppliedEventSequence = sequence;
    return true;
}

bool NetworkLobby::confirmSnapshotApplied(EventSequence sequence)
{
    if (_mode != NetworkLaunchMode::Join
        || sequence.value < _lastAppliedEventSequence.value
        || sequence.value >= _nextExpectedEventSequence) {
        return false;
    }
    _lastAppliedEventSequence = sequence;
    return true;
}

EventReplay NetworkLobby::replayAfter(EventSequence lastApplied) const
{
    return _eventJournal.replayAfter(lastApplied);
}

bool NetworkLobby::requestRecovery(EventSequence lastApplied)
{
    if (_mode != NetworkLaunchMode::Join
        || _state != NetworkLobbyState::Ready
        || !_startRequested
        || _transport == nullptr
        || lastApplied.value == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    std::vector<std::uint8_t> body;
    appendUInt64(body, lastApplied.value);
    if (!sendRecoveryMessage(static_cast<std::uint8_t>(RecoveryMessageType::Request), body)) {
        return false;
    }
    _recovering = true;
    _nextExpectedEventSequence = lastApplied.value + 1;
    _peerSnapshots.clear();
    return true;
}

bool NetworkLobby::beginReconnectRecovery(EventSequence lastApplied)
{
    if (_mode != NetworkLaunchMode::Join
        || _state != NetworkLobbyState::Ready
        || !_startRequested
        || lastApplied.value == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    _recovering = true;
    _nextExpectedEventSequence = lastApplied.value + 1;
    _peerSnapshots.clear();
    return true;
}

std::optional<EventSequence> NetworkLobby::takeRecoveryRequest()
{
    if (_recoveryRequests.empty()) {
        return std::nullopt;
    }
    EventSequence sequence = _recoveryRequests.front();
    _recoveryRequests.pop_front();
    return sequence;
}

bool NetworkLobby::sendRecovery(EventSequence lastApplied, const WorldSnapshot& snapshot)
{
    if (_mode != NetworkLaunchMode::Host
        || _state != NetworkLobbyState::Ready
        || !_startRequested
        || _transport == nullptr) {
        return false;
    }

    EventReplay replay = _eventJournal.replayAfter(lastApplied);
    if (replay.status == EventReplayStatus::InvalidFutureSequence) {
        fail(NetworkLobbyError::ProtocolError);
        return false;
    }
    if (replay.status == EventReplayStatus::SnapshotRequired) {
        if (snapshot.lastIncludedEvent != _eventJournal.latestSequence()) {
            return false;
        }
        std::vector<std::uint8_t> encodedSnapshot;
        if (encodeSnapshot(snapshot, encodedSnapshot) != SnapshotError::None
            || !sendRecoveryMessage(static_cast<std::uint8_t>(RecoveryMessageType::Snapshot), encodedSnapshot)) {
            return false;
        }
    } else if (replay.status == EventReplayStatus::Available) {
        for (const GameEvent& event : replay.events) {
            ProtocolEnvelope envelope;
            envelope.sessionId = _sessionId;
            envelope.sequence = _nextSendSequence++;
            if (encodeGameEvent(event, envelope) != GameplayWireError::None
                || !sendGameplayEnvelope(std::move(envelope))) {
                return false;
            }
        }
    }

    std::vector<std::uint8_t> completeBody;
    appendUInt64(completeBody, _eventJournal.latestSequence().value);
    return sendRecoveryMessage(static_cast<std::uint8_t>(RecoveryMessageType::Complete), completeBody);
}

std::optional<WorldSnapshot> NetworkLobby::takePeerSnapshot()
{
    if (_peerSnapshots.empty()) {
        return std::nullopt;
    }
    WorldSnapshot snapshot = std::move(_peerSnapshots.front());
    _peerSnapshots.pop_front();
    return snapshot;
}

bool NetworkLobby::sendAuthoritativeState(const WorldSnapshot& snapshot)
{
    if (_mode != NetworkLaunchMode::Host
        || _state != NetworkLobbyState::Ready
        || !_startRequested
        || _transport == nullptr) {
        return false;
    }
    std::vector<std::uint8_t> payload;
    if (encodeSnapshot(snapshot, payload) != SnapshotError::None) {
        return false;
    }
    ProtocolEnvelope envelope;
    envelope.kind = MessageKind::Combat;
    envelope.sessionId = _sessionId;
    envelope.sequence = _nextSendSequence++;
    envelope.payload = std::move(payload);
    return sendGameplayEnvelope(std::move(envelope));
}

std::optional<WorldSnapshot> NetworkLobby::takeAuthoritativeState()
{
    if (_authoritativeStates.empty()) {
        return std::nullopt;
    }
    WorldSnapshot snapshot = std::move(_authoritativeStates.front());
    _authoritativeStates.pop_front();
    return snapshot;
}

EventSequence NetworkLobby::latestAuthoritativeEvent() const
{
    return _eventJournal.latestSequence();
}

bool NetworkLobby::recoveryInProgress() const
{
    return _recovering;
}

void NetworkLobby::abortRecovery()
{
    if (_mode == NetworkLaunchMode::Join) {
        _recovering = false;
        fail(NetworkLobbyError::ProtocolError);
    }
}

bool NetworkLobby::reattachTransport(std::unique_ptr<Transport> transport)
{
    if (_state != NetworkLobbyState::Disconnected
        || transport == nullptr
        || !transport->isConnected()) {
        return false;
    }
    if (_transport != nullptr) {
        _transport->close();
    }
    _transport = std::move(transport);
    _nextSendSequence = 2;
    _nextReceiveSequence = 2;
    _error = NetworkLobbyError::None;
    _pendingCommandSequences.clear();
    _commandResults.clear();
    _recovering = false;
    _state = NetworkLobbyState::Ready;
    return true;
}

bool NetworkLobby::queueRecovery(EventSequence lastApplied)
{
    if (_mode != NetworkLaunchMode::Host
        || _state != NetworkLobbyState::Ready
        || _eventJournal.replayAfter(lastApplied).status == EventReplayStatus::InvalidFutureSequence) {
        return false;
    }
    _recoveryRequests.push_back(lastApplied);
    return true;
}

EventSequence NetworkLobby::lastAppliedEvent() const
{
    if (_mode != NetworkLaunchMode::Join) {
        return {};
    }
    return _lastAppliedEventSequence;
}

CharacterLobbyError NetworkLobby::submitLocalSheet(const CharacterCreationSheet& sheet)
{
    CharacterLobbyError error = validateCharacterSheet(sheet);
    PlayerId expectedPlayer = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    if (_state != NetworkLobbyState::Waiting) {
        error = CharacterLobbyError::WrongPhase;
    } else if (sheet.playerId != expectedPlayer) {
        error = CharacterLobbyError::InvalidPlayerId;
    }
    if (error != CharacterLobbyError::None) {
        _sheetError = error;
        return error;
    }
    if (_localSheet.has_value()) {
        if (*_localSheet == sheet) {
            return CharacterLobbyError::None;
        }
        _sheetError = CharacterLobbyError::WrongPhase;
        return _sheetError;
    }

    std::vector<std::uint8_t> body;
    error = encodeCharacterSheet(sheet, body);
    if (error != CharacterLobbyError::None) {
        _sheetError = error;
        return error;
    }
    if (!sendMessage(MessageType::CharacterSheet, body)) {
        return CharacterLobbyError::SessionInactive;
    }

    _localSheet = sheet;
    tryReadyHost();
    return CharacterLobbyError::None;
}

bool NetworkLobby::sendChatMessage(const std::string& text)
{
    if ((_state != NetworkLobbyState::Waiting && _state != NetworkLobbyState::Ready)
        || _transport == nullptr
        || !isValidChatText(text)) {
        return false;
    }

    PlayerId playerId = _mode == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    std::vector<std::uint8_t> body;
    body.reserve(kChatHeaderSize + text.size());
    appendUInt32(body, playerId.value);
    appendUInt16(body, static_cast<std::uint16_t>(text.size()));
    body.insert(body.end(), text.begin(), text.end());
    return sendMessage(MessageType::Chat, body);
}

std::optional<LobbyChatMessage> NetworkLobby::takeChatMessage()
{
    if (_chatMessages.empty()) {
        return std::nullopt;
    }
    LobbyChatMessage message = std::move(_chatMessages.front());
    _chatMessages.pop_front();
    return message;
}

bool NetworkLobby::requestStart()
{
    if (_mode != NetworkLaunchMode::Host || _state != NetworkLobbyState::Ready) {
        return false;
    }
    if (_startRequested) {
        return true;
    }
    if (!sendMessage(MessageType::Start)) {
        return false;
    }

    _startRequested = true;
    return true;
}

void NetworkLobby::poll()
{
    if (_transport == nullptr) {
        return;
    }
    if (_state == NetworkLobbyState::Rejected) {
        _transport->poll();
        return;
    }
    if (_state != NetworkLobbyState::Waiting && _state != NetworkLobbyState::Ready) {
        return;
    }

    for (int packetCount = 0; packetCount < 16; packetCount++) {
        std::optional<Packet> packet = _transport->receive();
        if (!packet.has_value()) {
            if (!_transport->isConnected()) {
                if (_state == NetworkLobbyState::Ready && _startRequested) {
                    markDisconnected();
                } else {
                    fail(NetworkLobbyError::Disconnected);
                }
            }
            return;
        }
        handlePacket(*packet);
        if (_state != NetworkLobbyState::Waiting && _state != NetworkLobbyState::Ready) {
            return;
        }
    }
}

bool NetworkLobby::disconnectForReconnect()
{
    if (_state != NetworkLobbyState::Ready || !_startRequested || _transport == nullptr) {
        return false;
    }
    markDisconnected();
    return true;
}

void NetworkLobby::stop()
{
    if (_transport != nullptr) {
        _transport->close();
        _transport.reset();
    }
    _startRequested = false;
    _pendingCommandSequences.clear();
    _recoveryRequests.clear();
    _peerCommands.clear();
    _commandResults.clear();
    _peerEvents.clear();
    _chatMessages.clear();
    _peerSnapshots.clear();
    _authoritativeStates.clear();
    _recovering = false;
    _eventJournal.clear();
    _state = NetworkLobbyState::Stopped;
}

NetworkLobbyState NetworkLobby::state() const
{
    return _state;
}

NetworkLobbyError NetworkLobby::error() const
{
    return _error;
}

CharacterLobbyError NetworkLobby::sheetError() const
{
    return _sheetError;
}

const CharacterCreationSheet* NetworkLobby::localSheet() const
{
    return _localSheet.has_value() ? &*_localSheet : nullptr;
}

const CharacterCreationSheet* NetworkLobby::peerSheet() const
{
    return _peerSheet.has_value() ? &*_peerSheet : nullptr;
}

bool NetworkLobby::startRequested() const
{
    return _startRequested;
}

std::uint64_t NetworkLobby::nextSendSequence() const
{
    return _nextSendSequence;
}

std::uint64_t NetworkLobby::nextReceiveSequence() const
{
    return _nextReceiveSequence;
}

std::unique_ptr<Transport> NetworkLobby::takeTransport()
{
    if (_state != NetworkLobbyState::Ready) {
        return nullptr;
    }
    return std::move(_transport);
}

bool NetworkLobby::sendMessage(MessageType type, const std::vector<std::uint8_t>& body)
{
    ProtocolEnvelope envelope;
    envelope.kind = MessageKind::Lobby;
    envelope.sessionId = _sessionId;
    envelope.sequence = _nextSendSequence++;
    appendUInt16(envelope.payload, kNetworkLobbyVersion);
    envelope.payload.push_back(static_cast<std::uint8_t>(type));
    envelope.payload.push_back(0);
    envelope.payload.insert(envelope.payload.end(), body.begin(), body.end());

    Packet packet;
    if (encodeEnvelope(envelope, packet) != ProtocolError::None) {
        fail(NetworkLobbyError::EncodeFailed);
        return false;
    }
    if (_transport == nullptr || _transport->send(std::move(packet)) != TransportSendResult::Sent) {
        fail(NetworkLobbyError::SendFailed);
        return false;
    }
    return true;
}

bool NetworkLobby::sendRecoveryMessage(std::uint8_t type, const std::vector<std::uint8_t>& body)
{
    ProtocolEnvelope envelope;
    envelope.kind = MessageKind::Snapshot;
    envelope.sessionId = _sessionId;
    envelope.sequence = _nextSendSequence++;
    appendUInt16(envelope.payload, kRecoveryWireVersion);
    envelope.payload.push_back(type);
    envelope.payload.push_back(0);
    envelope.payload.insert(envelope.payload.end(), body.begin(), body.end());
    return sendGameplayEnvelope(std::move(envelope));
}

void NetworkLobby::handlePacket(const Packet& packet)
{
    ProtocolDecodeResult decoded = decodeEnvelope(packet);
    if (!decoded
        || decoded.envelope.sessionId != _sessionId
        || decoded.envelope.sequence != _nextReceiveSequence) {
        fail(NetworkLobbyError::ProtocolError);
        return;
    }
    _nextReceiveSequence++;

    if (decoded.envelope.kind == MessageKind::Snapshot) {
        if (_state != NetworkLobbyState::Ready
            || !_startRequested
            || decoded.envelope.payload.size() < kRecoveryHeaderSize
            || readUInt16(decoded.envelope.payload, 0) != kRecoveryWireVersion
            || decoded.envelope.payload[3] != 0) {
            fail(NetworkLobbyError::ProtocolError);
            return;
        }

        RecoveryMessageType type = static_cast<RecoveryMessageType>(decoded.envelope.payload[2]);
        std::vector<std::uint8_t> body(
            decoded.envelope.payload.begin() + kRecoveryHeaderSize,
            decoded.envelope.payload.end());
        if (type == RecoveryMessageType::Request) {
            if (_mode != NetworkLaunchMode::Host || body.size() != sizeof(std::uint64_t)) {
                fail(NetworkLobbyError::UnexpectedMessage);
                return;
            }
            _recoveryRequests.push_back(EventSequence { readUInt64(body, 0) });
            return;
        }
        if (type == RecoveryMessageType::Snapshot) {
            if (_mode != NetworkLaunchMode::Join || !_recovering) {
                fail(NetworkLobbyError::UnexpectedMessage);
                return;
            }
            SnapshotDecodeResult snapshot = decodeSnapshot(body);
            if (!snapshot) {
                fail(NetworkLobbyError::ProtocolError);
                return;
            }
            if (snapshot.snapshot.lastIncludedEvent.value == std::numeric_limits<std::uint64_t>::max()) {
                fail(NetworkLobbyError::ProtocolError);
                return;
            }
            _nextExpectedEventSequence = snapshot.snapshot.lastIncludedEvent.value + 1;
            _peerSnapshots.push_back(std::move(snapshot.snapshot));
            return;
        }
        if (type == RecoveryMessageType::Complete) {
            if (_mode != NetworkLaunchMode::Join || !_recovering || body.size() != sizeof(std::uint64_t)) {
                fail(NetworkLobbyError::UnexpectedMessage);
                return;
            }
            EventSequence latest { readUInt64(body, 0) };
            if (_nextExpectedEventSequence == 0
                || latest.value == std::numeric_limits<std::uint64_t>::max()
                || latest.value + 1 != _nextExpectedEventSequence) {
                fail(NetworkLobbyError::ProtocolError);
                return;
            }
            _recovering = false;
            return;
        }
        fail(NetworkLobbyError::UnexpectedMessage);
        return;
    }

    if (decoded.envelope.kind == MessageKind::Combat) {
        SnapshotDecodeResult state = decodeSnapshot(decoded.envelope.payload);
        if (_mode != NetworkLaunchMode::Join
            || _state != NetworkLobbyState::Ready
            || !_startRequested
            || !state) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        if (_authoritativeStates.size() >= 2) {
            _authoritativeStates.pop_front();
        }
        _authoritativeStates.push_back(std::move(state.snapshot));
        return;
    }

    if (decoded.envelope.kind == MessageKind::Command) {
        GameCommandDecodeResult command = decodeGameCommand(decoded.envelope);
        if (_mode != NetworkLaunchMode::Host
            || _state != NetworkLobbyState::Ready
            || !_startRequested
            || !command
            || command.command.playerId != kGuestPlayerId
            || command.command.actorId != EntityId { kGuestPlayerId.value }) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        _peerCommands.push_back(std::move(command.command));
        return;
    }

    if (decoded.envelope.kind == MessageKind::CommandResult) {
        CommandResultDecodeResult result = decodeCommandResult(decoded.envelope);
        if (_mode != NetworkLaunchMode::Join
            || _state != NetworkLobbyState::Ready
            || !_startRequested
            || !result
            || _pendingCommandSequences.empty()
            || result.result.commandSequence != _pendingCommandSequences.front()) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        _pendingCommandSequences.pop_front();
        _commandResults.push_back(result.result);
        return;
    }

    if (decoded.envelope.kind == MessageKind::Event) {
        GameEventDecodeResult event = decodeGameEvent(decoded.envelope);
        EntityId actorId = event ? eventActorId(event.event.payload) : EntityId {};
        if (_mode != NetworkLaunchMode::Join
            || _state != NetworkLobbyState::Ready
            || !_startRequested
            || !event
            || !isSupportedLiveEvent(event.event.payload)
            || (actorId != EntityId { kHostPlayerId.value }
                && actorId != EntityId { kGuestPlayerId.value })) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        if (event.event.sequence.value != _nextExpectedEventSequence) {
            if (!_recovering
                && event.event.sequence.value > _nextExpectedEventSequence
                && _nextExpectedEventSequence > 0
                && requestRecovery(EventSequence { _nextExpectedEventSequence - 1 })) {
                return;
            }
            if (!_recovering) {
                fail(NetworkLobbyError::UnexpectedMessage);
            }
            return;
        }
        _nextExpectedEventSequence++;
        _peerEvents.push_back(std::move(event.event));
        return;
    }

    if (decoded.envelope.kind != MessageKind::Lobby
        || decoded.envelope.payload.size() < kLobbyHeaderSize
        || readUInt16(decoded.envelope.payload, 0) != kNetworkLobbyVersion
        || decoded.envelope.payload[3] != 0) {
        fail(NetworkLobbyError::ProtocolError);
        return;
    }

    MessageType type = static_cast<MessageType>(decoded.envelope.payload[2]);
    std::vector<std::uint8_t> body(decoded.envelope.payload.begin() + kLobbyHeaderSize, decoded.envelope.payload.end());
    switch (type) {
    case MessageType::CharacterSheet:
        handleCharacterSheet(body);
        return;
    case MessageType::Ready:
        if (_mode != NetworkLaunchMode::Join || !body.empty() || !_localSheet.has_value() || !_peerSheet.has_value()) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        _state = NetworkLobbyState::Ready;
        return;
    case MessageType::Rejected:
        if (body.size() != 2) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        _sheetError = static_cast<CharacterLobbyError>(readUInt16(body, 0));
        if (!isKnownCharacterLobbyError(_sheetError)) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        _error = NetworkLobbyError::PeerSheetRejected;
        _state = NetworkLobbyState::Rejected;
        return;
    case MessageType::Start:
        if (_mode != NetworkLaunchMode::Join
            || _state != NetworkLobbyState::Ready
            || !body.empty()
            || !_localSheet.has_value()
            || !_peerSheet.has_value()) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        _startRequested = true;
        return;
    case MessageType::Chat: {
        if (body.size() < kChatHeaderSize) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        PlayerId sender { readUInt32(body, 0) };
        PlayerId expectedSender = _mode == NetworkLaunchMode::Host ? kGuestPlayerId : kHostPlayerId;
        std::size_t textLength = readUInt16(body, 4);
        if (sender != expectedSender || body.size() != kChatHeaderSize + textLength) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        std::string text(body.begin() + kChatHeaderSize, body.end());
        if (!isValidChatText(text)) {
            fail(NetworkLobbyError::UnexpectedMessage);
            return;
        }
        if (_chatMessages.size() == kMaxQueuedChatMessages) {
            _chatMessages.pop_front();
        }
        _chatMessages.push_back(LobbyChatMessage { sender, std::move(text) });
        return;
    }
    }
    fail(NetworkLobbyError::UnexpectedMessage);
}

void NetworkLobby::handleCharacterSheet(const std::vector<std::uint8_t>& body)
{
    CharacterSheetDecodeResult decoded = decodeCharacterSheet(body);
    PlayerId expectedPlayer = _mode == NetworkLaunchMode::Host ? kGuestPlayerId : kHostPlayerId;
    CharacterLobbyError error = decoded.error;
    if (decoded && decoded.sheet.playerId != expectedPlayer) {
        error = CharacterLobbyError::InvalidPlayerId;
    }
    if (error != CharacterLobbyError::None) {
        reject(error);
        return;
    }
    if (_peerSheet.has_value()) {
        if (*_peerSheet == decoded.sheet) {
            return;
        }
        reject(CharacterLobbyError::WrongPhase);
        return;
    }

    _peerSheet = decoded.sheet;
    tryReadyHost();
}

void NetworkLobby::tryReadyHost()
{
    if (_mode != NetworkLaunchMode::Host
        || _state != NetworkLobbyState::Waiting
        || !_localSheet.has_value()
        || !_peerSheet.has_value()) {
        return;
    }
    if (sendMessage(MessageType::Ready)) {
        _state = NetworkLobbyState::Ready;
    }
}

void NetworkLobby::reject(CharacterLobbyError error)
{
    _sheetError = error;
    std::vector<std::uint8_t> body;
    appendUInt16(body, static_cast<std::uint16_t>(error));
    if (sendMessage(MessageType::Rejected, body)) {
        _error = NetworkLobbyError::PeerSheetRejected;
        _state = NetworkLobbyState::Rejected;
    }
}

void NetworkLobby::markDisconnected()
{
    if (_transport != nullptr) {
        _transport->close();
        _transport.reset();
    }
    _pendingCommandSequences.clear();
    _recoveryRequests.clear();
    _peerCommands.clear();
    _commandResults.clear();
    _peerEvents.clear();
    _peerSnapshots.clear();
    _recovering = false;
    _error = NetworkLobbyError::Disconnected;
    _state = NetworkLobbyState::Disconnected;
}

void NetworkLobby::fail(NetworkLobbyError error)
{
    _error = error;
    _state = NetworkLobbyState::Failed;
}

const char* networkLobbyErrorMessage(NetworkLobbyError error)
{
    switch (error) {
    case NetworkLobbyError::None:
        return "no error";
    case NetworkLobbyError::InvalidStart:
        return "the network lobby could not start";
    case NetworkLobbyError::EncodeFailed:
        return "the lobby message could not be encoded";
    case NetworkLobbyError::SendFailed:
        return "the lobby message could not be sent";
    case NetworkLobbyError::ProtocolError:
        return "the lobby received an invalid packet";
    case NetworkLobbyError::UnexpectedMessage:
        return "the lobby received an unexpected message";
    case NetworkLobbyError::PeerSheetRejected:
        return "a character sheet was rejected";
    case NetworkLobbyError::Disconnected:
        return "the multiplayer peer disconnected";
    }
    return "unknown lobby error";
}

const char* characterLobbyErrorMessage(CharacterLobbyError error)
{
    switch (error) {
    case CharacterLobbyError::None:
        return "no error";
    case CharacterLobbyError::SessionInactive:
        return "the multiplayer session is not active";
    case CharacterLobbyError::WrongPhase:
        return "the lobby is no longer accepting characters";
    case CharacterLobbyError::UnsupportedVersion:
        return "the character format version is unsupported";
    case CharacterLobbyError::InvalidPlayerId:
        return "the character belongs to the wrong player";
    case CharacterLobbyError::PlayerMissing:
        return "the character player is missing";
    case CharacterLobbyError::EmptyName:
        return "the character needs a name";
    case CharacterLobbyError::NameTooLong:
        return "the character name is too long";
    case CharacterLobbyError::InvalidName:
        return "the character name contains invalid text";
    case CharacterLobbyError::PrimaryStatOutOfRange:
        return "a SPECIAL stat is out of range";
    case CharacterLobbyError::InvalidPrimaryStatTotal:
        return "the SPECIAL total must be 40";
    case CharacterLobbyError::AgeOutOfRange:
        return "the character age is out of range";
    case CharacterLobbyError::InvalidGender:
        return "the character gender is invalid";
    case CharacterLobbyError::InvalidTaggedSkill:
        return "a tagged skill is invalid";
    case CharacterLobbyError::DuplicateTaggedSkill:
        return "tagged skills must be distinct";
    case CharacterLobbyError::InvalidTrait:
        return "a trait is invalid";
    case CharacterLobbyError::DuplicateTrait:
        return "traits must be distinct";
    case CharacterLobbyError::NonCanonicalTraits:
        return "the trait slots are malformed";
    case CharacterLobbyError::PacketTooShort:
        return "the character packet is too short";
    case CharacterLobbyError::TruncatedPacket:
        return "the character packet is truncated";
    case CharacterLobbyError::TrailingData:
        return "the character packet has trailing data";
    }
    return "unknown character error";
}

} // namespace multiplayer
} // namespace fallout
