#include "multiplayer/network_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#include "game/gconfig.h"
#include "multiplayer/network_bootstrap.h"
#include "multiplayer/protocol.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/input.h"

namespace fallout {
namespace multiplayer {
namespace {

NetworkLaunchOptions launchOptions;
NetworkBootstrap bootstrap;
NetworkBootstrapState reportedState = NetworkBootstrapState::Disabled;
bool backgroundProcessRegistered = false;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kArchiveSampleSize = 64 * 1024;

void hashBytes(std::uint64_t& digest, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; index++) {
        digest ^= bytes[index];
        digest *= kFnvPrime;
    }
}

void hashString(std::uint64_t& digest, const char* value)
{
    if (value != nullptr) {
        hashBytes(digest, value, std::char_traits<char>::length(value));
    }
    const std::uint8_t separator = 0;
    hashBytes(digest, &separator, sizeof(separator));
}

void hashUInt64(std::uint64_t& digest, std::uint64_t value)
{
    std::array<std::uint8_t, 8> bytes;
    for (int index = 7; index >= 0; index--) {
        bytes[index] = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
    }
    hashBytes(digest, bytes.data(), bytes.size());
}

bool hashArchiveSample(std::uint64_t& digest, const char* path)
{
    if (path == nullptr || *path == '\0') {
        return false;
    }

    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    std::streamoff size = stream.tellg();
    if (size < 0) {
        return false;
    }

    std::uint64_t archiveSize = static_cast<std::uint64_t>(size);
    hashUInt64(digest, archiveSize);

    std::array<char, kArchiveSampleSize> buffer;
    std::streamsize sampleSize = static_cast<std::streamsize>(
        std::min<std::uint64_t>(archiveSize, buffer.size()));
    stream.seekg(0);
    stream.read(buffer.data(), sampleSize);
    if (stream.gcount() != sampleSize) {
        return false;
    }
    hashBytes(digest, buffer.data(), static_cast<std::size_t>(sampleSize));

    if (archiveSize > buffer.size()) {
        stream.clear();
        stream.seekg(size - sampleSize);
        stream.read(buffer.data(), sampleSize);
        if (stream.gcount() != sampleSize) {
            return false;
        }
        hashBytes(digest, buffer.data(), static_cast<std::size_t>(sampleSize));
    }
    return true;
}

std::uint64_t compatibilityDigest()
{
    char* masterDat = nullptr;
    char* critterDat = nullptr;
    char* language = nullptr;
    if (!config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_DAT_KEY, &masterDat)
        || !config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_CRITTER_DAT_KEY, &critterDat)) {
        return 0;
    }
    config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_LANGUAGE_KEY, &language);

    std::uint64_t digest = kFnvOffsetBasis;
    hashString(digest, "fallout-ce-multiplayer-compatibility-v1");
    hashUInt64(digest, kProtocolVersion);
    hashUInt64(digest, kConnectionHandshakeVersion);
    hashString(digest, language);
    if (!hashArchiveSample(digest, masterDat) || !hashArchiveSample(digest, critterDat)) {
        return 0;
    }
    return digest != 0 ? digest : 1;
}

SessionId createSessionId()
{
    std::uint64_t value = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    value ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&bootstrap));
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    return SessionId { value != 0 ? value : 1 };
}

void reportState()
{
    NetworkBootstrapState state = bootstrap.state();
    if (state == reportedState) {
        return;
    }
    reportedState = state;

    switch (state) {
    case NetworkBootstrapState::Connected:
        std::fprintf(stderr, "Multiplayer connection established: session %llu, local player %u.\n",
            static_cast<unsigned long long>(bootstrap.sessionId().value),
            bootstrap.localPlayerId().value);
        debug_printf("Multiplayer connection established: session %llu, local player %u.\n",
            static_cast<unsigned long long>(bootstrap.sessionId().value),
            bootstrap.localPlayerId().value);
        break;
    case NetworkBootstrapState::Rejected:
        std::fprintf(stderr, "Multiplayer connection rejected: %s.\n", handshakeRejectionMessage(bootstrap.rejection()));
        debug_printf("Multiplayer connection rejected: %s.\n", handshakeRejectionMessage(bootstrap.rejection()));
        break;
    case NetworkBootstrapState::Failed:
        std::fprintf(stderr, "Multiplayer connection failed: %s.\n", networkBootstrapErrorMessage(bootstrap.error()));
        debug_printf("Multiplayer connection failed: %s.\n", networkBootstrapErrorMessage(bootstrap.error()));
        break;
    default:
        break;
    }
}

void networkRuntimeBackgroundProcess()
{
    bootstrap.poll();
    reportState();
}

} // namespace

bool networkRuntimeConfigure(int argc, char** argv)
{
    NetworkLaunchParseResult result = parseNetworkLaunchOptions(argc, argv);
    if (!result) {
        std::fprintf(stderr, "Invalid multiplayer launch options: %s.\n", networkLaunchParseErrorMessage(result.error));
        return false;
    }
    launchOptions = result.options;
    return true;
}

bool networkRuntimeStart()
{
    if (launchOptions.mode == NetworkLaunchMode::Disabled) {
        return true;
    }

    std::uint64_t digest = compatibilityDigest();
    if (digest == 0) {
        std::fprintf(stderr, "Multiplayer launch failed: could not fingerprint master.dat and critter.dat.\n");
        debug_printf("Multiplayer launch failed: could not fingerprint master.dat and critter.dat.\n");
        return false;
    }

    SessionId sessionId = launchOptions.mode == NetworkLaunchMode::Host ? createSessionId() : SessionId {};
    if (!bootstrap.start(launchOptions, digest, sessionId)) {
        std::fprintf(stderr, "Multiplayer launch failed: %s.\n", networkBootstrapErrorMessage(bootstrap.error()));
        debug_printf("Multiplayer launch failed: %s.\n", networkBootstrapErrorMessage(bootstrap.error()));
        return false;
    }

    reportedState = bootstrap.state();
    if (launchOptions.mode == NetworkLaunchMode::Host) {
        std::fprintf(stderr, "Multiplayer host listening on TCP port %u.\n", bootstrap.port());
        debug_printf("Multiplayer host listening on TCP port %u.\n", bootstrap.port());
    } else {
        std::fprintf(stderr, "Multiplayer guest connected to %s:%u; awaiting host handshake.\n",
            launchOptions.address.c_str(),
            launchOptions.port);
        debug_printf("Multiplayer guest connected to %s:%u; awaiting host handshake.\n",
            launchOptions.address.c_str(),
            launchOptions.port);
    }

    add_bk_process(networkRuntimeBackgroundProcess);
    backgroundProcessRegistered = true;
    return true;
}

void networkRuntimeStop()
{
    if (backgroundProcessRegistered) {
        remove_bk_process(networkRuntimeBackgroundProcess);
        backgroundProcessRegistered = false;
    }
    bootstrap.stop();
}

} // namespace multiplayer
} // namespace fallout
