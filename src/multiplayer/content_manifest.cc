#include "multiplayer/content_manifest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <mbedtls/sha256.h>

namespace fallout {
namespace multiplayer {
namespace {

constexpr std::size_t kReadBufferSize = 1024 * 1024;

struct ManifestHasher {
    mbedtls_sha256_context context;
    bool active = false;

    ManifestHasher()
    {
        mbedtls_sha256_init(&context);
        active = mbedtls_sha256_starts(&context, 0) == 0;
    }

    ~ManifestHasher()
    {
        mbedtls_sha256_free(&context);
    }

    bool update(const void* data, std::size_t size)
    {
        return active
            && mbedtls_sha256_update(&context,
                   static_cast<const unsigned char*>(data),
                   size)
                == 0;
    }

    bool string(std::string_view value)
    {
        std::array<unsigned char, 8> sizeBytes;
        std::uint64_t size = value.size();
        for (int index = 7; index >= 0; index--) {
            sizeBytes[index] = static_cast<unsigned char>(size & 0xFF);
            size >>= 8;
        }
        return update(sizeBytes.data(), sizeBytes.size())
            && update(value.data(), value.size());
    }

    bool finish(std::array<unsigned char, 32>& digest)
    {
        if (!active || mbedtls_sha256_finish(&context, digest.data()) != 0) {
            return false;
        }
        active = false;
        return true;
    }
};

std::string normalizedRelativePath(const std::filesystem::path& path)
{
    std::string value = path.generic_string();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isGameplayPatchPath(const std::filesystem::path& relative)
{
    auto component = relative.begin();
    if (component == relative.end()) {
        return false;
    }
    std::string top = normalizedRelativePath(*component);
    if (top == "maps") {
        std::string extension = normalizedRelativePath(relative.extension());
        // Fallout writes per-session map state and generated edge caches into
        // the same directory as immutable .MAP patches. They are save data,
        // not compatibility inputs, and can legitimately differ per peer.
        return extension != ".sav"
            && extension != ".bak"
            && extension != ".edg"
            && extension != ".tmp"
            && normalizedRelativePath(relative.filename()) != "automap.db";
    }
    return top == "scripts"
        || top == "proto"
        || top == "text";
}

ContentManifestError hashFile(ManifestHasher& hasher,
    std::string_view label,
    const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return ContentManifestError::InvalidPath;
    }
    std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return ContentManifestError::ReadFailed;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return ContentManifestError::ReadFailed;
    }
    if (!hasher.string(label)) {
        return ContentManifestError::HashFailed;
    }
    std::array<unsigned char, 8> sizeBytes;
    std::uint64_t remaining = size;
    for (int index = 7; index >= 0; index--) {
        sizeBytes[index] = static_cast<unsigned char>(remaining & 0xFF);
        remaining >>= 8;
    }
    if (!hasher.update(sizeBytes.data(), sizeBytes.size())) {
        return ContentManifestError::HashFailed;
    }

    std::vector<char> buffer(kReadBufferSize);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize count = stream.gcount();
        if (count > 0
            && !hasher.update(buffer.data(), static_cast<std::size_t>(count))) {
            return ContentManifestError::HashFailed;
        }
    }
    return stream.eof() ? ContentManifestError::None : ContentManifestError::ReadFailed;
}

ContentManifestError hashPatchTree(ManifestHasher& hasher,
    std::string_view label,
    const std::string& configuredPath)
{
    if (!hasher.string(label)) {
        return ContentManifestError::HashFailed;
    }
    std::filesystem::path root(configuredPath);
    std::error_code error;
    bool present = !configuredPath.empty() && std::filesystem::is_directory(root, error) && !error;
    const unsigned char marker = present ? 1 : 0;
    if (!hasher.update(&marker, sizeof(marker))) {
        return ContentManifestError::HashFailed;
    }
    if (!present) {
        return ContentManifestError::None;
    }

    std::vector<std::pair<std::string, std::filesystem::path>> files;
    std::unordered_set<std::string> normalizedPaths;
    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(error) && !error) {
            std::filesystem::path relative = entry.path().lexically_relative(root);
            if (isGameplayPatchPath(relative)) {
                std::string normalized = normalizedRelativePath(relative);
                if (!normalizedPaths.insert(normalized).second) {
                    return ContentManifestError::DuplicatePath;
                }
                files.emplace_back(std::move(normalized), entry.path());
            }
        }
        iterator.increment(error);
    }
    if (error) {
        return ContentManifestError::ReadFailed;
    }

    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    for (const auto& file : files) {
        ContentManifestError fileError = hashFile(hasher, file.first, file.second);
        if (fileError != ContentManifestError::None) {
            return fileError;
        }
    }
    return ContentManifestError::None;
}

} // namespace

ContentManifestResult buildContentManifest(const std::string& masterArchive,
    const std::string& critterArchive,
    const std::string& masterPatches,
    const std::string& critterPatches)
{
    ManifestHasher hasher;
    if (!hasher.active || !hasher.string("fallout-ce-content-manifest-v1")) {
        return { ContentManifestError::HashFailed, 0 };
    }
    ContentManifestError error = hashFile(hasher, "master.dat", masterArchive);
    if (error == ContentManifestError::None) {
        error = hashFile(hasher, "critter.dat", critterArchive);
    }
    if (error == ContentManifestError::None) {
        error = hashPatchTree(hasher, "master-patches", masterPatches);
    }
    if (error == ContentManifestError::None) {
        error = hashPatchTree(hasher, "critter-patches", critterPatches);
    }
    if (error != ContentManifestError::None) {
        return { error, 0 };
    }

    std::array<unsigned char, 32> digestBytes;
    if (!hasher.finish(digestBytes)) {
        return { ContentManifestError::HashFailed, 0 };
    }
    std::uint64_t digest = 0;
    for (int index = 0; index < 8; index++) {
        digest = (digest << 8) | digestBytes[index];
    }
    return { ContentManifestError::None, digest != 0 ? digest : 1 };
}

const char* contentManifestErrorMessage(ContentManifestError error)
{
    switch (error) {
    case ContentManifestError::None:
        return "no error";
    case ContentManifestError::InvalidPath:
        return "invalid archive path";
    case ContentManifestError::ReadFailed:
        return "content file could not be read";
    case ContentManifestError::DuplicatePath:
        return "content tree contains duplicate case-insensitive paths";
    case ContentManifestError::HashFailed:
        return "content hash failed";
    }
    return "unknown content manifest error";
}

} // namespace multiplayer
} // namespace fallout
