#ifndef FALLOUT_MULTIPLAYER_SESSION_RECOVERY_H_
#define FALLOUT_MULTIPLAYER_SESSION_RECOVERY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "multiplayer/snapshot.h"
#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

constexpr std::size_t kDefaultEventJournalMaximumEvents = 256;
constexpr std::size_t kDefaultEventJournalMaximumBytes = 256 * 1024;
constexpr std::size_t kReconnectTokenSize = 32;

enum class EventJournalError {
    None,
    InvalidLimit,
    InvalidEvent,
    SequenceGap,
    EventTooLarge,
};

enum class EventReplayStatus {
    UpToDate,
    Available,
    SnapshotRequired,
    InvalidFutureSequence,
};

struct EventReplay {
    EventReplayStatus status = EventReplayStatus::UpToDate;
    std::vector<GameEvent> events;
};

class EventJournal {
public:
    explicit EventJournal(
        std::size_t maximumEvents = kDefaultEventJournalMaximumEvents,
        std::size_t maximumBytes = kDefaultEventJournalMaximumBytes);

    EventJournalError append(const GameEvent& event);
    EventReplay replayAfter(EventSequence lastApplied) const;
    void clear();
    void reset(EventSequence latestSequence);

    bool empty() const;
    std::size_t size() const;
    std::size_t byteSize() const;
    EventSequence oldestSequence() const;
    EventSequence latestSequence() const;

private:
    struct Entry {
        GameEvent event;
        std::size_t byteSize = 0;
    };

    std::size_t _maximumEvents;
    std::size_t _maximumBytes;
    std::size_t _byteSize = 0;
    EventSequence _latestSequence;
    std::deque<Entry> _entries;
};

struct ReconnectToken {
    std::array<std::uint8_t, kReconnectTokenSize> bytes {};
};

bool isValid(const ReconnectToken& token);
bool reconnectTokensEqual(const ReconnectToken& lhs, const ReconnectToken& rhs);

enum class ReconnectTokenError {
    None,
    InvalidSession,
    InvalidPlayer,
    InvalidToken,
};

class ReconnectTokenRegistry {
public:
    explicit ReconnectTokenRegistry(SessionId sessionId = {});

    void reset(SessionId sessionId);
    ReconnectTokenError install(PlayerId playerId, const ReconnectToken& token);
    bool validate(SessionId sessionId, PlayerId playerId, const ReconnectToken& token) const;
    void invalidate(PlayerId playerId);
    void invalidateAll();
    SessionId sessionId() const;

private:
    SessionId _sessionId;
    std::unordered_map<PlayerId, ReconnectToken, PlayerIdHash> _tokens;
};

enum class SnapshotQueueError {
    None,
    InvalidLimit,
    InvalidSnapshot,
    NotActive,
    InvalidEvent,
    SequenceGap,
    QueueOverflow,
};

struct SnapshotRecoveryBatch {
    WorldSnapshot snapshot;
    std::vector<GameEvent> followingEvents;
};

class SnapshotRecoveryQueue {
public:
    explicit SnapshotRecoveryQueue(std::size_t maximumQueuedBytes = kDefaultEventJournalMaximumBytes);

    SnapshotQueueError begin(const WorldSnapshot& snapshot);
    SnapshotQueueError append(const GameEvent& event);
    std::optional<SnapshotRecoveryBatch> finish();
    void clear();

    bool active() const;
    bool restartRequired() const;
    std::size_t byteSize() const;

private:
    std::size_t _maximumQueuedBytes;
    std::size_t _byteSize = 0;
    bool _active = false;
    bool _restartRequired = false;
    WorldSnapshot _snapshot;
    std::vector<GameEvent> _events;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_SESSION_RECOVERY_H_ */
