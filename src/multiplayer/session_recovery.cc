#include "multiplayer/session_recovery.h"

#include <limits>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include "multiplayer/gameplay_wire.h"
#include "multiplayer/protocol.h"

namespace fallout {
namespace multiplayer {
namespace {

std::optional<std::size_t> encodedEventSize(const GameEvent& event)
{
    ProtocolEnvelope envelope;
    envelope.sessionId.value = 1;
    envelope.sequence = 1;
    if (encodeGameEvent(event, envelope) != GameplayWireError::None) {
        return std::nullopt;
    }
    return kProtocolHeaderSize + envelope.payload.size();
}

bool isPlayerSlot(PlayerId playerId)
{
    return playerId == kHostPlayerId || playerId == kGuestPlayerId;
}

} // namespace

EventJournal::EventJournal(std::size_t maximumEvents, std::size_t maximumBytes)
    : _maximumEvents(maximumEvents)
    , _maximumBytes(maximumBytes)
{
}

EventJournalError EventJournal::append(const GameEvent& event)
{
    if (_maximumEvents == 0 || _maximumBytes == 0) {
        return EventJournalError::InvalidLimit;
    }

    std::optional<std::size_t> eventSize = encodedEventSize(event);
    if (!eventSize.has_value()) {
        return EventJournalError::InvalidEvent;
    }
    if (*eventSize > _maximumBytes) {
        return EventJournalError::EventTooLarge;
    }
    if (_latestSequence.value == std::numeric_limits<std::uint64_t>::max()
        || event.sequence.value != _latestSequence.value + 1) {
        return EventJournalError::SequenceGap;
    }

    _entries.push_back(Entry { event, *eventSize });
    _byteSize += *eventSize;
    _latestSequence = event.sequence;
    while (_entries.size() > _maximumEvents || _byteSize > _maximumBytes) {
        _byteSize -= _entries.front().byteSize;
        _entries.pop_front();
    }
    return EventJournalError::None;
}

EventReplay EventJournal::replayAfter(EventSequence lastApplied) const
{
    EventReplay replay;
    if (lastApplied.value > _latestSequence.value) {
        replay.status = EventReplayStatus::InvalidFutureSequence;
        return replay;
    }
    if (lastApplied == _latestSequence) {
        replay.status = EventReplayStatus::UpToDate;
        return replay;
    }
    if (_entries.empty()
        || lastApplied.value == std::numeric_limits<std::uint64_t>::max()
        || lastApplied.value + 1 < _entries.front().event.sequence.value) {
        replay.status = EventReplayStatus::SnapshotRequired;
        return replay;
    }

    replay.status = EventReplayStatus::Available;
    for (const Entry& entry : _entries) {
        if (entry.event.sequence.value > lastApplied.value) {
            replay.events.push_back(entry.event);
        }
    }
    return replay;
}

void EventJournal::clear()
{
    reset(EventSequence {});
}

void EventJournal::reset(EventSequence latestSequence)
{
    _entries.clear();
    _byteSize = 0;
    _latestSequence = latestSequence;
}

bool EventJournal::empty() const
{
    return _entries.empty();
}

std::size_t EventJournal::size() const
{
    return _entries.size();
}

std::size_t EventJournal::byteSize() const
{
    return _byteSize;
}

EventSequence EventJournal::oldestSequence() const
{
    return _entries.empty() ? EventSequence {} : _entries.front().event.sequence;
}

EventSequence EventJournal::latestSequence() const
{
    return _latestSequence;
}

bool isValid(const ReconnectToken& token)
{
    std::uint8_t combined = 0;
    for (std::uint8_t byte : token.bytes) {
        combined |= byte;
    }
    return combined != 0;
}

bool reconnectTokensEqual(const ReconnectToken& lhs, const ReconnectToken& rhs)
{
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < lhs.bytes.size(); index++) {
        difference |= lhs.bytes[index] ^ rhs.bytes[index];
    }
    return difference == 0;
}

bool generateReconnectToken(ReconnectToken& token)
{
    token = {};
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&random);
    static constexpr unsigned char personalization[] = "fallout-ce reconnect token";
    int result = mbedtls_ctr_drbg_seed(&random,
        mbedtls_entropy_func,
        &entropy,
        personalization,
        sizeof(personalization) - 1);
    if (result == 0) {
        result = mbedtls_ctr_drbg_random(&random, token.bytes.data(), token.bytes.size());
    }
    mbedtls_ctr_drbg_free(&random);
    mbedtls_entropy_free(&entropy);
    if (result != 0 || !isValid(token)) {
        token = {};
        return false;
    }
    return true;
}

ReconnectTokenRegistry::ReconnectTokenRegistry(SessionId sessionId)
    : _sessionId(sessionId)
{
}

void ReconnectTokenRegistry::reset(SessionId sessionId)
{
    invalidateAll();
    _sessionId = sessionId;
}

ReconnectTokenError ReconnectTokenRegistry::install(PlayerId playerId, const ReconnectToken& token)
{
    if (_sessionId.value == 0) {
        return ReconnectTokenError::InvalidSession;
    }
    if (!isPlayerSlot(playerId)) {
        return ReconnectTokenError::InvalidPlayer;
    }
    if (!isValid(token)) {
        return ReconnectTokenError::InvalidToken;
    }
    _tokens[playerId] = token;
    return ReconnectTokenError::None;
}

bool ReconnectTokenRegistry::validate(SessionId sessionId, PlayerId playerId, const ReconnectToken& token) const
{
    if (_sessionId.value == 0 || sessionId != _sessionId || !isPlayerSlot(playerId) || !isValid(token)) {
        return false;
    }
    auto entry = _tokens.find(playerId);
    return entry != _tokens.end() && reconnectTokensEqual(entry->second, token);
}

void ReconnectTokenRegistry::invalidate(PlayerId playerId)
{
    _tokens.erase(playerId);
}

void ReconnectTokenRegistry::invalidateAll()
{
    _tokens.clear();
}

SessionId ReconnectTokenRegistry::sessionId() const
{
    return _sessionId;
}

SnapshotRecoveryQueue::SnapshotRecoveryQueue(std::size_t maximumQueuedBytes)
    : _maximumQueuedBytes(maximumQueuedBytes)
{
}

SnapshotQueueError SnapshotRecoveryQueue::begin(const WorldSnapshot& snapshot)
{
    clear();
    if (_maximumQueuedBytes == 0) {
        return SnapshotQueueError::InvalidLimit;
    }
    if (validateSnapshot(snapshot) != SnapshotError::None) {
        return SnapshotQueueError::InvalidSnapshot;
    }
    _snapshot = snapshot;
    _active = true;
    return SnapshotQueueError::None;
}

SnapshotQueueError SnapshotRecoveryQueue::append(const GameEvent& event)
{
    if (!_active || _restartRequired) {
        return SnapshotQueueError::NotActive;
    }
    std::optional<std::size_t> eventSize = encodedEventSize(event);
    if (!eventSize.has_value()) {
        return SnapshotQueueError::InvalidEvent;
    }

    std::uint64_t previousSequence = _events.empty()
        ? _snapshot.lastIncludedEvent.value
        : _events.back().sequence.value;
    if (previousSequence == std::numeric_limits<std::uint64_t>::max()
        || event.sequence.value != previousSequence + 1) {
        return SnapshotQueueError::SequenceGap;
    }
    if (*eventSize > _maximumQueuedBytes - _byteSize) {
        _restartRequired = true;
        _events.clear();
        _byteSize = 0;
        return SnapshotQueueError::QueueOverflow;
    }

    _events.push_back(event);
    _byteSize += *eventSize;
    return SnapshotQueueError::None;
}

std::optional<SnapshotRecoveryBatch> SnapshotRecoveryQueue::finish()
{
    if (!_active || _restartRequired) {
        return std::nullopt;
    }

    SnapshotRecoveryBatch batch { _snapshot, std::move(_events) };
    clear();
    return batch;
}

void SnapshotRecoveryQueue::clear()
{
    _byteSize = 0;
    _active = false;
    _restartRequired = false;
    _snapshot = {};
    _events.clear();
}

bool SnapshotRecoveryQueue::active() const
{
    return _active;
}

bool SnapshotRecoveryQueue::restartRequired() const
{
    return _restartRequired;
}

std::size_t SnapshotRecoveryQueue::byteSize() const
{
    return _byteSize;
}

} // namespace multiplayer
} // namespace fallout
