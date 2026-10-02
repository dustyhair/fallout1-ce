// Inject rename I/O failure after moving the previous sidecar. The first rename
// uses the real filesystem; later publication and restoration attempts fail.
#define compat_rename testSidecarRename
#include "game/loadsave.cc"
#undef compat_rename
#include <cerrno>
#include <filesystem>
#include <cstdio>

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
int compat_rename(const char*, const char*);
static int faultMode = 0;
static int renameCalls = 0;
int testSidecarRename(const char* from, const char* to)
{
    ++renameCalls;
    if ((faultMode == 1 && renameCalls >= 2) || faultMode == 2) {
        errno = EIO;
        return -1;
    }
    return compat_rename(from, to);
}
}

int main(int argc, char** argv)
{
    using namespace fallout;
    if (argc != 2 || db_init(nullptr, nullptr, argv[1], 0) == INVALID_DATABASE_HANDLE) return 2;
    patches = argv[1];
    std::filesystem::path slot = std::filesystem::path(argv[1]) / "SAVEGAME/SLOT01";
    std::filesystem::create_directories(slot);
    const std::vector<std::uint8_t> original { 1, 2, 3, 4 };
    const std::vector<std::uint8_t> replacement { 8, 8, 7 };
    auto equals = [](const char* name, const std::vector<std::uint8_t>& expected) {
        std::vector<std::uint8_t> actual;
        std::string path = std::string("SAVEGAME\\SLOT01\\") + name;
        return ReadSaveFile(path.c_str(), 32, actual) && actual == expected;
    };
    if (!WriteSaveFile("SAVEGAME\\SLOT01\\MULTI.DAT", original)) return 3;
    faultMode = 1;
    bool firstFailed = !PublishMultiplayerSidecar({ 9, 9 });
    bool firstBackup = equals("MULTI.OLD", original);
    faultMode = 2; renameCalls = 0;
    bool retryFailed = !PublishMultiplayerSidecar(replacement);
    bool retryBackup = equals("MULTI.OLD", original);
    faultMode = 0; renameCalls = 0;
    bool unblocked = PublishMultiplayerSidecar(replacement) && equals("MULTI.DAT", replacement)
        && !std::filesystem::exists(slot / "MULTI.OLD");

    // An older interrupted publisher leaves MULTI.BAK. Native backup creation
    // must recover that metadata before enumerating or clearing map backups.
    const std::vector<std::uint8_t> nativeData { 4, 5, 6 };
    if (!WriteSaveFile("SAVEGAME\\SLOT01\\SAVE.DAT", nativeData)
        || !WriteSaveFile("SAVEGAME\\SLOT01\\MAP.SAV", nativeData)
        || compat_rename((slot / "MULTI.DAT").c_str(), (slot / "MULTI.BAK").c_str()) != 0) return 4;
    faultMode = 2; renameCalls = 0;
    bool legacyRefused = SaveBackup() == -1 && equals("MULTI.BAK", replacement)
        && equals("SAVE.DAT", nativeData) && equals("MAP.SAV", nativeData);
    faultMode = 0; renameCalls = 0;
    bool legacyRecovered = SaveBackup() == 0 && equals("MULTI.DAT", replacement)
        && equals("SAVE.BAK", nativeData) && equals("MAP.BAK", nativeData)
        && !std::filesystem::exists(slot / "MULTI.BAK");

    // A destination directory is not evidence that the previous metadata was
    // published. Preserve the recoverable backup until that obstruction clears.
    if (compat_rename((slot / "MULTI.DAT").c_str(), (slot / "MULTI.OLD").c_str()) != 0) return 5;
    std::filesystem::create_directory(slot / "MULTI.DAT");
    std::FILE* blocker = std::fopen((slot / "MULTI.DAT/blocker").c_str(), "wb");
    if (blocker == nullptr) return 6;
    std::fclose(blocker);
    bool directoryRefused = !PublishMultiplayerSidecar({ 7 }) && equals("MULTI.OLD", replacement);
    std::filesystem::remove_all(slot / "MULTI.DAT");
    bool directoryRecovered = PublishMultiplayerSidecar({ 7 }) && equals("MULTI.DAT", { 7 });
    std::printf("NATIVE_SIDECAR_PUBLICATION_RETRY first_failed=%d first_backup=%d retry_failed=%d retry_backup=%d unblocked=%d legacy_refused=%d legacy_recovered=%d directory_refused=%d directory_recovered=%d\n",
        firstFailed, firstBackup, retryFailed, retryBackup, unblocked, legacyRefused,
        legacyRecovered, directoryRefused, directoryRecovered);
    db_exit();
    return firstFailed && firstBackup && retryFailed && retryBackup && unblocked
        && legacyRefused && legacyRecovered && directoryRefused && directoryRecovered ? 0 : 1;
}
