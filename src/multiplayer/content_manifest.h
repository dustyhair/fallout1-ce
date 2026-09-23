#ifndef FALLOUT_MULTIPLAYER_CONTENT_MANIFEST_H_
#define FALLOUT_MULTIPLAYER_CONTENT_MANIFEST_H_

#include <cstdint>
#include <string>

namespace fallout {
namespace multiplayer {

enum class ContentManifestError {
    None,
    InvalidPath,
    ReadFailed,
    DuplicatePath,
    HashFailed,
};

struct ContentManifestResult {
    ContentManifestError error = ContentManifestError::None;
    std::uint64_t digest = 0;

    explicit operator bool() const { return error == ContentManifestError::None; }
};

ContentManifestResult buildContentManifest(const std::string& masterArchive,
    const std::string& critterArchive,
    const std::string& masterPatches,
    const std::string& critterPatches);

const char* contentManifestErrorMessage(ContentManifestError error);

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_CONTENT_MANIFEST_H_ */
