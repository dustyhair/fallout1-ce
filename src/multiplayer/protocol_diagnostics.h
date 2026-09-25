#ifndef FALLOUT_MULTIPLAYER_PROTOCOL_DIAGNOSTICS_H_
#define FALLOUT_MULTIPLAYER_PROTOCOL_DIAGNOSTICS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

#include "multiplayer/protocol.h"
#include "multiplayer/snapshot.h"

namespace fallout {
namespace multiplayer {

constexpr std::size_t kProtocolDiagnosticCapacity = 128;

enum class ProtocolDiagnosticDirection {
    Sent,
    Received,
    Rejected,
    ChecksumSent,
    ChecksumReceived,
};

struct ProtocolDiagnosticRecord {
    ProtocolDiagnosticDirection direction = ProtocolDiagnosticDirection::Rejected;
    MessageKind kind = MessageKind::Handshake;
    std::uint64_t sequence = 0;
    std::size_t payloadSize = 0;
    ProtocolError protocolError = ProtocolError::None;
    EventSequence stateEvent;
    SectionedStateDigest stateDigest;
};

struct ProtocolDiagnosticCounters {
    std::array<std::uint64_t, 10> sentByKind {};
    std::array<std::uint64_t, 10> receivedByKind {};
    std::uint64_t rejectedPackets = 0;
    std::uint64_t sentChecksums = 0;
    std::uint64_t receivedChecksums = 0;
};

class ProtocolDiagnostics {
public:
    void recordEnvelope(ProtocolDiagnosticDirection direction,
        const ProtocolEnvelope& envelope);
    void recordRejected(ProtocolError error, std::size_t packetSize);
    void recordStateChecksum(ProtocolDiagnosticDirection direction,
        EventSequence event,
        const SectionedStateDigest& digest);
    void reset();

    const ProtocolDiagnosticCounters& counters() const;
    const std::deque<ProtocolDiagnosticRecord>& records() const;

private:
    void append(ProtocolDiagnosticRecord record);

    ProtocolDiagnosticCounters _counters;
    std::deque<ProtocolDiagnosticRecord> _records;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_PROTOCOL_DIAGNOSTICS_H_ */
