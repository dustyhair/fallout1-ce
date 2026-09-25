#include "multiplayer/protocol_diagnostics.h"

#include <utility>

namespace fallout {
namespace multiplayer {
namespace {

std::size_t messageKindIndex(MessageKind kind)
{
    std::size_t index = static_cast<std::size_t>(kind);
    return index < 10 ? index : 0;
}

} // namespace

void ProtocolDiagnostics::recordEnvelope(ProtocolDiagnosticDirection direction,
    const ProtocolEnvelope& envelope)
{
    if (direction != ProtocolDiagnosticDirection::Sent
        && direction != ProtocolDiagnosticDirection::Received) {
        return;
    }
    auto& counters = direction == ProtocolDiagnosticDirection::Sent
        ? _counters.sentByKind
        : _counters.receivedByKind;
    counters[messageKindIndex(envelope.kind)]++;
    ProtocolDiagnosticRecord record;
    record.direction = direction;
    record.kind = envelope.kind;
    record.sequence = envelope.sequence;
    record.payloadSize = envelope.payload.size();
    append(std::move(record));
}

void ProtocolDiagnostics::recordRejected(ProtocolError error, std::size_t packetSize)
{
    _counters.rejectedPackets++;
    ProtocolDiagnosticRecord record;
    record.direction = ProtocolDiagnosticDirection::Rejected;
    record.payloadSize = packetSize;
    record.protocolError = error;
    append(std::move(record));
}

void ProtocolDiagnostics::recordStateChecksum(ProtocolDiagnosticDirection direction,
    EventSequence event,
    const SectionedStateDigest& digest)
{
    if (direction == ProtocolDiagnosticDirection::ChecksumSent) {
        _counters.sentChecksums++;
    } else if (direction == ProtocolDiagnosticDirection::ChecksumReceived) {
        _counters.receivedChecksums++;
    } else {
        return;
    }
    ProtocolDiagnosticRecord record;
    record.direction = direction;
    record.stateEvent = event;
    record.stateDigest = digest;
    append(std::move(record));
}

void ProtocolDiagnostics::reset()
{
    _counters = {};
    _records.clear();
}

const ProtocolDiagnosticCounters& ProtocolDiagnostics::counters() const
{
    return _counters;
}

const std::deque<ProtocolDiagnosticRecord>& ProtocolDiagnostics::records() const
{
    return _records;
}

void ProtocolDiagnostics::append(ProtocolDiagnosticRecord record)
{
    if (_records.size() == kProtocolDiagnosticCapacity) {
        _records.pop_front();
    }
    _records.push_back(std::move(record));
}

} // namespace multiplayer
} // namespace fallout
