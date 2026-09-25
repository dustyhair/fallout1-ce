#include "multiplayer/content_manifest.h"

namespace fallout {
namespace multiplayer {

ContentManifestResult buildContentManifest(const std::string&,
    const std::string&,
    const std::string&,
    const std::string&)
{
    return { ContentManifestError::HashFailed, 0 };
}

const char* contentManifestErrorMessage(ContentManifestError error)
{
    return error == ContentManifestError::None
        ? "no error"
        : "multiplayer support is disabled at build time";
}

} // namespace multiplayer
} // namespace fallout
