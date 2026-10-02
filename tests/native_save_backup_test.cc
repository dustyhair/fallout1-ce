#include "game/loadsave.cc"
#include <filesystem>

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
}

int main(int argc, char** argv)
{
    using namespace fallout;
    if (argc != 2 || db_init(nullptr, nullptr, argv[1], 0) == INVALID_DATABASE_HANDLE) return 2;
    patches = argv[1];
    const char* originals[] = {
        "SAVEGAME\\SLOT01\\SAVE.DAT",
        "SAVEGAME\\SLOT01\\MAP.SAV",
        "SAVEGAME\\SLOT01\\AUTOMAP.SAV",
        "SAVEGAME\\SLOT01\\MULTI.DAT",
    };
    for (const char* path : originals) {
        DB_FILE* stream = db_fopen(path, "wb");
        if (stream == nullptr) { db_exit(); return 3; }
        bool written = db_fwriteInt(stream, 456) == 0;
        written = db_fclose(stream) == 0 && written;
        if (!written) { db_exit(); return 4; }
    }
    // MAP.BAK is a directory. Backup creation must fail without moving any
    // original slot files or orphaning the multiplayer metadata.
    int result = SaveBackup();
    auto readValue = [](const char* path) {
        DB_FILE* stream = db_fopen(path, "rb");
        int restored = 0;
        if (stream == nullptr) return false;
        bool intact = db_freadInt(stream, &restored) == 0 && restored == 456;
        return db_fclose(stream) == 0 && intact;
    };
    bool intact = true;
    for (const char* path : originals) {
        intact = readValue(path) && intact;
    }
    std::string blocked = std::string(argv[1]) + "/SAVEGAME/SLOT01/MAP.BAK";
    if (std::remove((blocked + "/blocker").c_str()) != 0) { db_exit(); return 5; }
    if (std::remove(blocked.c_str()) != 0) { db_exit(); return 5; }
    int normal = SaveBackup();
    bool backupIntact = readValue("SAVEGAME\\SLOT01\\SAVE.BAK")
        && readValue("SAVEGAME\\SLOT01\\MAP.BAK")
        && readValue("SAVEGAME\\SLOT01\\AUTOMAP.BAK");
    bool originalsAfterBackup = true;
    for (const char* path : originals) {
        originalsAfterBackup = readValue(path) && originalsAfterBackup;
    }
    // Simulate the partial native output that a later save failure must undo.
    for (const char* path : originals) {
        if (strcmp(path, "SAVEGAME\\SLOT01\\MULTI.DAT") == 0) continue;
        DB_FILE* stream = db_fopen(path, "wb");
        if (stream == nullptr) { db_exit(); return 6; }
        bool written = db_fwriteInt(stream, 999) == 0;
        written = db_fclose(stream) == 0 && written;
        if (!written) { db_exit(); return 7; }
    }
    // A nonempty map destination refuses replacement during rollback. Every
    // backup must survive the failed restore so it can be retried safely.
    std::filesystem::path blockedRestore = std::filesystem::path(argv[1]) / "SAVEGAME/SLOT01/MAP.SAV";
    std::filesystem::remove(blockedRestore);
    std::filesystem::create_directory(blockedRestore);
    std::FILE* blocker = std::fopen((blockedRestore / "blocker").c_str(), "wb");
    if (blocker == nullptr) { db_exit(); return 8; }
    std::fclose(blocker);
    int refusedRestore = RestoreSave();
    bool retryBackups = readValue("SAVEGAME\\SLOT01\\SAVE.BAK")
        && readValue("SAVEGAME\\SLOT01\\MAP.BAK")
        && readValue("SAVEGAME\\SLOT01\\AUTOMAP.BAK");
    std::filesystem::remove_all(blockedRestore);
    int restoreResult = RestoreSave();
    bool restoreIntact = true;
    for (const char* path : originals) restoreIntact = readValue(path) && restoreIntact;
    std::printf("NATIVE_SAVE_RESTORE_RETRY refused=%d backups_intact=%d\n", refusedRestore, retryBackups);
    db_exit();
    std::printf("NATIVE_SAVE_BACKUP failure=%d originals_intact=%d normal=%d backups_intact=%d originals_after_backup=%d restore=%d restore_intact=%d\n",
        result, intact, normal, backupIntact, originalsAfterBackup, restoreResult, restoreIntact);
    return result == -1 && intact && normal == 0 && backupIntact && originalsAfterBackup
        && refusedRestore == -1 && retryBackups && restoreResult == 0 && restoreIntact ? 0 : 1;
}
