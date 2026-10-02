#include "game/loadsave.cc"
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
}

int main(int argc, char** argv)
{
    using namespace fallout;
    if ((argc != 2 && argc != 3) || db_init(nullptr, nullptr, argv[1], 0) == INVALID_DATABASE_HANDLE) return 2;
    patches = argv[1];
    if (argc == 3) {
        bool blocked = strcmp(argv[2], "--blocked-restart") == 0 && SaveBackup() == -1;
        for (const char* path : { "SAVEGAME\\SLOT01\\SAVE.BAK", "SAVEGAME\\SLOT01\\MAP.BAK", "SAVEGAME\\SLOT01\\AUTOMAP.BAK" }) {
            DB_FILE* stream = db_fopen(path, "rb");
            int value = 0;
            bool intact = stream != nullptr && db_freadInt(stream, &value) == 0 && value == 456;
            if (stream != nullptr) intact = db_fclose(stream) == 0 && intact;
            blocked = intact && blocked;
        }
        db_exit();
        std::printf("NATIVE_SAVE_FRESH_PROCESS blocked_and_retained=%d\n", blocked);
        return blocked ? 0 : 1;
    }
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
    pid_t child = fork();
    if (child == -1) { db_exit(); return 11; }
    if (child == 0) {
        execl(argv[0], argv[0], argv[1], "--blocked-restart", static_cast<char*>(nullptr));
        _exit(12);
    }
    int childStatus = 0;
    bool freshProcessRetained = waitpid(child, &childStatus, 0) == child
        && WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0;
    // Also discard the current process's in-memory retry guard.
    failedSaveRestorations.clear();
    map_backup_count = -1;
    bool persistentMarker = std::filesystem::is_regular_file(
        std::filesystem::path(argv[1]) / "SAVEGAME/SLOT01/SAVE.RBK");
    int saveRetryBlocked = SaveBackup();
    bool retryBackupsAfterSave = readValue("SAVEGAME\\SLOT01\\SAVE.BAK")
        && readValue("SAVEGAME\\SLOT01\\MAP.BAK")
        && readValue("SAVEGAME\\SLOT01\\AUTOMAP.BAK");
    // Corrupt records must refuse another save without touching the backups.
    std::filesystem::path record = std::filesystem::path(argv[1]) / "SAVEGAME/SLOT01/SAVE.RBK";
    std::FILE* marker = std::fopen(record.c_str(), "wb");
    if (marker == nullptr) { db_exit(); return 9; }
    std::fputs("invalid rollback record\n", marker);
    std::fclose(marker);
    failedSaveRestorations.clear();
    int malformedBlocked = SaveBackup();
    bool malformedRetained = readValue("SAVEGAME\\SLOT01\\SAVE.BAK")
        && readValue("SAVEGAME\\SLOT01\\MAP.BAK")
        && readValue("SAVEGAME\\SLOT01\\AUTOMAP.BAK");
    marker = std::fopen(record.c_str(), "wb");
    if (marker == nullptr) { db_exit(); return 10; }
    std::fputs("FALLOUT-ROLLBACK-1 2\n", marker);
    std::fclose(marker);
    std::filesystem::remove_all(blockedRestore);
    int resumedBackup = SaveBackup();
    int restoreResult = RestoreSave();
    bool restoreIntact = true;
    for (const char* path : originals) restoreIntact = readValue(path) && restoreIntact;
    std::printf("NATIVE_SAVE_RESTORE_RETRY refused=%d backups_intact=%d blocked_save=%d retained_after_save=%d resumed_backup=%d\n",
        refusedRestore, retryBackups, saveRetryBlocked, retryBackupsAfterSave, resumedBackup);
    bool markerRemoved = !std::filesystem::exists(record);
    std::printf("NATIVE_SAVE_PERSISTENT_ROLLBACK marker=%d malformed_blocked=%d malformed_retained=%d removed=%d\n",
        persistentMarker, malformedBlocked, malformedRetained, markerRemoved);
    db_exit();
    std::printf("NATIVE_SAVE_BACKUP failure=%d originals_intact=%d normal=%d backups_intact=%d originals_after_backup=%d restore=%d restore_intact=%d\n",
        result, intact, normal, backupIntact, originalsAfterBackup, restoreResult, restoreIntact);
    return result == -1 && intact && normal == 0 && backupIntact && originalsAfterBackup
        && freshProcessRetained && persistentMarker && malformedBlocked == -1 && malformedRetained && markerRemoved
        && refusedRestore == -1 && retryBackups && saveRetryBlocked == -1
        && retryBackupsAfterSave && resumedBackup == 0 && restoreResult == 0 && restoreIntact ? 0 : 1;
}
