#ifndef FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_
#define FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "multiplayer/character_lobby.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/session_recovery.h"
#include "multiplayer/snapshot.h"
#include "multiplayer/transport.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::uint16_t kNetworkLobbyVersion = 1;

enum class NetworkLobbyState {
    Disabled,
    Waiting,
    Ready,
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
};

class NetworkLobby {
public:
    bool start(NetworkLaunchMode mode, SessionId sessionId, std::unique_ptr<Transport> transport);
    CharacterLobbyError submitLocalSheet(const CharacterCreationSheet& sheet);
    bool requestStart();
    bool sendLocalMove(int destinationTile,
        int elevation,
        bool running,
        int startingTile = -1,
        const std::vector<std::uint8_t>& path = {},
        std::uint32_t phaseRevision = 1);
    bool sendLocalFacing(int rotation, std::uint32_t phaseRevision = 1);
    bool sendLocalDoorUse(EntityId targetId, std::uint32_t phaseRevision = 1);
    bool sendCommandOutcome(AuthoritativeCommandResult outcome);
    std::optional<GameCommand> takePeerCommand();
    std::optional<CommandResult> takeCommandResult();
    std::optional<GameEvent> takePeerEvent();
    EventReplay replayAfter(EventSequence lastApplied) const;
    bool requestRecovery(EventSequence lastApplied);
    std::optional<EventSequence> takeRecoveryRequest();
    bool sendRecovery(EventSequence lastApplied, const WorldSnapshot& snapshot);
    std::optional<WorldSnapshot> takePeerSnapshot();
    EventSequence latestAuthoritativeEvent() const;
    bool recoveryInProgress() const;
    void abortRecovery();
    void poll();
    void stop();

    NetworkLobbyState state() const;
    NetworkLobbyError error() const;
    CharacterLobbyError sheetError() const;
    const CharacterCreationSheet* localSheet() const;
    const CharacterCreationSheet* peerSheet() const;
    bool startRequested() const;
    std::uint64_t nextSendSequence() const;
    std::uint64_t nextReceiveSequence() const;
    std::unique_ptr<Transport> takeTransport();

private:
    enum class MessageType : std::uint8_t {
        CharacterSheet = 1,
        Ready = 2,
        Rejected = 3,
        Start = 4,
    };

    bool sendLocalAction(GameEventPayload eventPayload, GameCommandPayload commandPayload, std::uint32_t phaseRevision);
    bool sendAuthoritativeEvent(GameEvent event);
    bool sendRecoveryMessage(std::uint8_t type, const std::vector<std::uint8_t>& body = {});
    bool sendGameplayEnvelope(ProtocolEnvelope envelope);
    bool sendMessage(MessageType type, const std::vector<std::uint8_t>& body = {});
    void handlePacket(const Packet& packet);
    void handleCharacterSheet(const std::vector<std::uint8_t>& body);
    void tryReadyHost();
    void reject(CharacterLobbyError error);
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
    std::deque<CommandSequence> _pendingCommandSequences;
    std::deque<EventSequence> _recoveryRequests;
    std::deque<GameCommand> _peerCommands;
    std::deque<CommandResult> _commandResults;
    std::deque<GameEvent> _peerEvents;
    std::deque<WorldSnapshot> _peerSnapshots;
    bool _recovering = false;
    EventJournal _eventJournal;
    std::unique_ptr<Transport> _transport;
};

const char* networkLobbyErrorMessage(NetworkLobbyError error);
const char* characterLobbyErrorMessage(CharacterLobbyError error);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_NETWORK_LOBBY_H_ */
