#ifndef FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_
#define FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "multiplayer/character_lobby.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/protocol_diagnostics.h"
#include "multiplayer/session_recovery.h"
#include "multiplayer/snapshot.h"
#include "multiplayer/transport.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kNetworkLobbyVersion = 4;
constexpr std::size_t kMaxLobbyChatMessageLength = 64;

struct StoryPresentationCompletion {
    PlayerId playerId;
    std::uint64_t revision = 0;
};

struct LobbyChatMessage {
    PlayerId playerId;
    std::string text;
};

enum class NetworkLobbyState {
    Disabled,
    Waiting,
    Ready,
    Disconnected,
    Rejected,
    Failed,
    Stopped,
};

enum class NetworkLobbyError {
    None,
    InvalidStart,
    EncodeFailed,
    SendFailed,
    ProtocolError,
    UnexpectedMessage,
    PeerSheetRejected,
    Disconnected,
    SnapshotCapacityExceeded,
};

class NetworkLobby {
public:
    bool start(NetworkLaunchMode mode, SessionId sessionId, std::unique_ptr<Transport> transport);
    CharacterLobbyError submitLocalSheet(const CharacterCreationSheet& sheet);
    bool sendChatMessage(const std::string& text);
    std::optional<LobbyChatMessage> takeChatMessage();
    bool requestStart();
    bool sendLocalMove(int destinationTile,
        int elevation,
        bool running,
        int startingTile = -1,
        const std::vector<std::uint8_t>& path = {},
        std::uint32_t phaseRevision = 1);
    bool sendLocalFacing(int rotation, std::uint32_t phaseRevision = 1);
    bool sendLocalDoorUse(EntityId targetId, std::uint32_t phaseRevision = 1, std::uint64_t turnRevision = 0);
    bool sendLocalPickup(EntityId targetId, std::uint32_t phaseRevision = 1, std::uint64_t turnRevision = 0);
    bool sendLocalLoot(EntityId targetId, std::uint32_t phaseRevision = 1, std::uint64_t turnRevision = 0, bool targetChange = false);
    bool sendLocalSkillUse(EntityId targetId, ExplorationSkill skill, std::uint32_t phaseRevision = 1,
        std::uint64_t turnRevision = 0);
    bool sendLocalItemUse(EntityId itemId, EntityId targetId, std::uint32_t phaseRevision = 1);
    bool sendLocalElevator(std::int32_t elevatorType, std::int32_t destinationLevel, std::uint32_t phaseRevision = 1);
    bool sendLocalExitGrid(EntityId exitId, std::uint32_t phaseRevision = 1);
    bool sendLocalSceneryTransition(EntityId transitionId, std::uint32_t phaseRevision = 1);
    bool sendLocalRest(std::int32_t minutes, std::uint32_t phaseRevision = 1);
    bool sendLocalAttack(EntityId targetId, std::int32_t hitMode, std::int32_t hitLocation,
        std::uint64_t turnRevision, std::uint32_t phaseRevision = 1);
    bool sendLocalCombatMove(std::int32_t tile, std::int32_t elevation,
        bool running, std::uint64_t turnRevision, std::uint32_t phaseRevision);
    bool sendLocalCombatItem(EntityId itemId, EntityId targetId, std::uint64_t turnRevision,
        std::uint32_t phaseRevision, std::int32_t timerSeconds = 0);
    bool sendLocalCombatReload(EntityId weaponId, std::int32_t hitMode,
        std::uint64_t turnRevision, std::uint32_t phaseRevision);
    bool sendLocalInventoryAction(const InventoryActionCommand& command, std::uint32_t phaseRevision);
    bool sendLocalStartCombat(const StartCombatCommand& command, std::uint32_t phaseRevision);
    bool sendLocalCharacterAdvance(const CharacterAdvanceCommand& command, std::uint32_t phaseRevision);
    bool sendLocalEquipment(const EquipmentCommand& equipment, std::uint32_t phaseRevision);
    bool sendLocalCombatFace(std::int32_t rotation, std::uint64_t turnRevision,
        std::uint32_t phaseRevision);
    bool sendLocalEndTurn(std::uint64_t turnRevision, std::uint32_t phaseRevision);
    bool sendLocalSharedModal(SharedModalKind kind, bool open, SessionPhase currentPhase, std::uint32_t phaseRevision = 1);
    bool sendLocalTalk(EntityId targetId, std::uint32_t phaseRevision);
    bool sendLocalDialogueVote(std::uint64_t revision, std::uint8_t option, std::uint32_t phaseRevision);
    bool sendLocalNpcBarter(const NpcBarterCommand& command, std::uint32_t phaseRevision);
    bool sendLocalDirectTrade(const DirectTradeCommand& trade,
        std::uint32_t phaseRevision);
    bool sendLocalWorldMapRoute(const WorldMapRouteCommand& route, std::uint32_t phaseRevision = 1);
    bool sendLocalInventoryTransfer(EntityId sourceId,
        EntityId destinationId,
        EntityId itemId,
        std::uint32_t quantity,
        std::uint32_t sourceQuantity,
        std::uint32_t phaseRevision = 1,
        EntityId remainderItemId = {},
        ItemDescriptor itemDescriptor = {},
        std::uint64_t turnRevision = 0);
    bool sendLocalItemDrop(EntityId sourceId,
        EntityId itemId,
        std::uint32_t quantity,
        std::uint32_t sourceQuantity,
        std::uint32_t phaseRevision = 1,
        EntityId remainderItemId = {},
        std::int32_t tile = -1,
        std::int32_t elevation = -1,
        ItemDescriptor itemDescriptor = {});
    bool sendCommandOutcome(AuthoritativeCommandResult outcome);
    bool publishLocalCommandOutcome(AuthoritativeCommandResult outcome);
    bool publishDeferredEvent(GameEvent event);
    std::optional<GameCommand> takePeerCommand();
    std::optional<CommandResult> takeCommandResult();
    std::optional<GameEvent> takePeerEvent();
    bool confirmPeerEventApplied(EventSequence sequence);
    bool confirmSnapshotApplied(EventSequence sequence);
    EventReplay replayAfter(EventSequence lastApplied) const;
    bool requestRecovery(EventSequence lastApplied);
    bool beginReconnectRecovery(EventSequence lastApplied);
    std::optional<EventSequence> takeRecoveryRequest();
    bool sendRecovery(EventSequence lastApplied, const WorldSnapshot& snapshot);
    std::optional<WorldSnapshot> takePeerSnapshot();
    bool sendAuthoritativeState(const WorldSnapshot& snapshot);
    std::optional<WorldSnapshot> takeAuthoritativeState();
    bool confirmStoryPresentationCompleted(std::uint64_t revision);
    std::optional<StoryPresentationCompletion> takeStoryPresentationCompleted();
    bool confirmSessionEndingApplied(std::uint32_t phaseRevision);
    std::uint32_t acknowledgedEndingPhaseRevision() const;
    EventSequence latestAuthoritativeEvent() const;
    bool recoveryInProgress() const;
    void abortRecovery();
    void abortSnapshotCapacity();
    bool reattachTransport(std::unique_ptr<Transport> transport);
    bool queueRecovery(EventSequence lastApplied);
    EventSequence lastAppliedEvent() const;
    EventSequence acknowledgedEvent(PlayerId playerId) const;
    void poll();
    bool disconnectForReconnect();
    void stop();

    NetworkLobbyState state() const;
    NetworkLobbyError error() const;
    CharacterLobbyError sheetError() const;
    const CharacterCreationSheet* localSheet() const;
    const CharacterCreationSheet* peerSheet() const;
    bool startRequested() const;
    CommandSequence nextLocalCommandSequence() const;
    std::uint64_t nextSendSequence() const;
    std::uint64_t nextReceiveSequence() const;
    const ProtocolDiagnostics& diagnostics() const;
    std::unique_ptr<Transport> takeTransport();

private:
    enum class MessageType : std::uint8_t {
        CharacterSheet = 1,
        Ready = 2,
        Rejected = 3,
        Start = 4,
        Chat = 5,
    };

    bool sendLocalAction(GameEventPayload eventPayload, GameCommandPayload commandPayload, std::uint32_t phaseRevision, SessionPhase phaseOverride = SessionPhase::Lobby);
    bool sendAuthoritativeEvent(GameEvent event);
    bool sendRecoveryMessage(std::uint8_t type, const std::vector<std::uint8_t>& body = {});
    bool sendGameplayEnvelope(ProtocolEnvelope envelope);
    bool sendMessage(MessageType type, const std::vector<std::uint8_t>& body = {});
    void handlePacket(const Packet& packet);
    void handleCharacterSheet(const std::vector<std::uint8_t>& body);
    void tryReadyHost();
    void reject(CharacterLobbyError error);
    void markDisconnected();
    void fail(NetworkLobbyError error);

    NetworkLaunchMode _mode = NetworkLaunchMode::Disabled;
    NetworkLobbyState _state = NetworkLobbyState::Disabled;
    NetworkLobbyError _error = NetworkLobbyError::None;
    CharacterLobbyError _sheetError = CharacterLobbyError::None;
    SessionId _sessionId;
    std::uint64_t _nextSendSequence = 2;
    std::uint64_t _nextReceiveSequence = 2;
    std::optional<CharacterCreationSheet> _localSheet;
    std::optional<CharacterCreationSheet> _peerSheet;
    bool _startRequested = false;
    std::uint64_t _nextLocalCommandSequence = 1;
    std::uint64_t _nextEventSequence = 1;
    std::uint64_t _nextExpectedEventSequence = 1;
    EventSequence _lastAppliedEventSequence;
    EventSequence _lastDeliveredEventSequence;
    EventSequence _latestReceivedCheckpoint;
    std::uint32_t _acknowledgedEndingPhaseRevision = 0;
    std::deque<StoryPresentationCompletion> _storyCompletions;
    std::unordered_map<PlayerId, EventSequence, PlayerIdHash> _acknowledgedEvents;
    // Retain unconfirmed intents across stream replacement. Replaying their
    // original IDs/revisions lets authority consume every command sequence.
    std::deque<GameCommand> _pendingCommands;
    std::deque<EventSequence> _recoveryRequests;
    std::deque<GameCommand> _peerCommands;
    std::deque<CommandResult> _commandResults;
    std::deque<GameEvent> _peerEvents;
    std::deque<LobbyChatMessage> _chatMessages;
    std::deque<WorldSnapshot> _peerSnapshots;
    std::deque<WorldSnapshot> _authoritativeStates;
    bool _recovering = false;
    EventJournal _eventJournal;
    ProtocolDiagnostics _diagnostics;
    std::unique_ptr<Transport> _transport;
};

const char* networkLobbyErrorMessage(NetworkLobbyError error);
const char* characterLobbyErrorMessage(CharacterLobbyError error);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_ */
