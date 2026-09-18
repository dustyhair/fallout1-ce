#include "multiplayer/network_lobby.h"

#include <utility>

#include "multiplayer/protocol.h"

namespace fallout {
namespace multiplayer {
namespace {

constexpr std::size_t kLobbyHeaderSize = 4;

void appendUInt16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t readUInt16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

bool isKnownCharacterLobbyError(CharacterLobbyError error)
{
    return error > CharacterLobbyError::None && error <= CharacterLobbyError::TrailingData;
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

void NetworkLobby::poll()
{
    if (_transport == nullptr) {
        return;
    }
    if (_state == NetworkLobbyState::Ready || _state == NetworkLobbyState::Rejected) {
        _transport->poll();
        if (_state == NetworkLobbyState::Ready && !_transport->isConnected()) {
            fail(NetworkLobbyError::Disconnected);
        }
        return;
    }
    if (_state != NetworkLobbyState::Waiting) {
        return;
    }

    for (int packetCount = 0; packetCount < 16; packetCount++) {
        std::optional<Packet> packet = _transport->receive();
        if (!packet.has_value()) {
            if (!_transport->isConnected()) {
                fail(NetworkLobbyError::Disconnected);
            }
            return;
        }
        handlePacket(*packet);
        if (_state != NetworkLobbyState::Waiting) {
            return;
        }
    }
}

void NetworkLobby::stop()
{
    if (_transport != nullptr) {
        _transport->close();
        _transport.reset();
    }
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

void NetworkLobby::handlePacket(const Packet& packet)
{
    ProtocolDecodeResult decoded = decodeEnvelope(packet);
    if (!decoded
        || decoded.envelope.kind != MessageKind::Lobby
        || decoded.envelope.sessionId != _sessionId
        || decoded.envelope.sequence != _nextReceiveSequence
        || decoded.envelope.payload.size() < kLobbyHeaderSize
        || readUInt16(decoded.envelope.payload, 0) != kNetworkLobbyVersion
        || decoded.envelope.payload[3] != 0) {
        fail(NetworkLobbyError::ProtocolError);
        return;
    }
    _nextReceiveSequence++;

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
