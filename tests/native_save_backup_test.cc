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
    auto readValue = [](const char* path, int expected = 456) {
        DB_FILE* stream = db_fopen(path, "rb");
        int restored = 0;
        if (stream == nullptr) return false;
        bool intact = db_freadInt(stream, &restored) == 0 && restored == expected;
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
    bool transactionRecorded = RecordPendingSaveRollback();
    // Simulate process interruption after changing both native and metadata files.
    for (const char* path : originals) {
        DB_FILE* stream = db_fopen(path, "wb");
        if (stream == nullptr) { db_exit(); return 6; }
        bool written = db_fwriteInt(stream, 999) == 0;
        written = db_fclose(stream) == 0 && written;
        if (!written) { db_exit(); return 7; }
    }
    std::filesystem::path metadataCopy = std::filesystem::path(argv[1]) / "SAVEGAME/SLOT01/MULTI.RSV";
    std::filesystem::path retainedMetadataCopy = metadataCopy.string() + ".kept";
    std::filesystem::rename(metadataCopy, retainedMetadataCopy);
    bool missingMetadataRefused = !RecoverPendingSaveRollback();
    bool missingMetadataUnchanged = true;
    for (const char* path : originals) missingMetadataUnchanged = readValue(path, 999) && missingMetadataUnchanged;
    std::filesystem::rename(retainedMetadataCopy, metadataCopy);
    // A nonempty map destination refuses replacement during rollback. Every
    // backup must survive the failed restore so it can be retried safely.
    std::filesystem::path blockedRestore = std::filesystem::path(argv[1]) / "SAVEGAME/SLOT01/MAP.SAV";
    std::filesystem::remove(blockedRestore);
    std::filesystem::create_directory(blockedRestore);
    std::FILE* blocker = std::fopen((blockedRestore / "blocker").c_str(), "wb");
    if (blocker == nullptr) { db_exit(); return 8; }
    std::fclose(blocker);
    pid_t child = fork();
    if (child == -1) { db_exit(); return 11; }
    if (child == 0) {
        execl(argv[0], argv[0], argv[1], "--blocked-restart", static_cast<char*>(nullptr));
        _exit(12);
    }
    int childStatus = 0;
    bool freshProcessRetained = waitpid(child, &childStatus, 0) == child
        && WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0;
    int refusedRestore = RestoreSave();
    bool retryBackups = readValue("SAVEGAME\\SLOT01\\SAVE.BAK")
        && readValue("SAVEGAME\\SLOT01\\MAP.BAK")
        && readValue("SAVEGAME\\SLOT01\\AUTOMAP.BAK");
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
    std::fputs("FALLOUT-ROLLBACK-2 2 1\n", marker);
    std::fclose(marker);
    std::filesystem::remove_all(blockedRestore);
    int resumedBackup = SaveBackup();
    int restoreResult = RestoreSave();
    bool restoreIntact = true;
    for (const char* path : originals) restoreIntact = readValue(path) && restoreIntact;
    std::printf("NATIVE_SAVE_RESTORE_RETRY refused=%d backups_intact=%d blocked_save=%d retained_after_save=%d resumed_backup=%d\n",
        refusedRestore, retryBackups, saveRetryBlocked, retryBackupsAfterSave, resumedBackup);
    bool markerRemoved = !std::filesystem::exists(record);
    bool metadataBackupRemoved = !std::filesystem::exists(
        std::filesystem::path(argv[1]) / "SAVEGAME/SLOT01/MULTI.RSV");
    std::printf("NATIVE_SAVE_PERSISTENT_ROLLBACK marker=%d malformed_blocked=%d malformed_retained=%d removed=%d\n",
        persistentMarker, malformedBlocked, malformedRetained, markerRemoved);
    // An interrupted first save must remove newly published metadata as well
    // as its partial native files; there was no prior checkpoint to restore.
    slot_cursor = 1;
    std::filesystem::create_directories(std::filesystem::path(argv[1]) / "SAVEGAME/SLOT02");
    bool emptyPrepared = SaveBackup() == 0 && RecordPendingSaveRollback();
    for (const char* name : { "SAVE.DAT", "MAP.SAV", "MULTI.DAT" }) {
        char path[COMPAT_MAX_PATH];
        SaveDirectoryPath(path, true, true);
        strcat(path, name);
        DB_FILE* stream = db_fopen(path, "wb");
        if (stream == nullptr) { db_exit(); return 13; }
        bool written = db_fwriteInt(stream, 999) == 0;
        written = db_fclose(stream) == 0 && written;
        if (!written) { db_exit(); return 14; }
    }
    failedSaveRestorations.clear();
    std::filesystem::path emptyBlocker = std::filesystem::path(argv[1]) / "SAVEGAME/SLOT02/MAP.SAV";
    std::filesystem::remove(emptyBlocker);
    std::filesystem::create_directory(emptyBlocker);
    std::FILE* emptyFile = std::fopen((emptyBlocker / "blocker").c_str(), "wb");
    if (emptyFile == nullptr) { db_exit(); return 17; }
    std::fclose(emptyFile);
    bool emptyBlocked = GetSlotList() == 10 && LSstatus[1] == SLOT_STATE_ERROR
        && slot_cursor == 1 && !recovery_slot_active;
    bool emptyRecordRetained = std::filesystem::is_regular_file(
        std::filesystem::path(argv[1]) / "SAVEGAME/SLOT02/SAVE.RBK");
    std::filesystem::remove_all(emptyBlocker);
    bool emptyRecovered = GetSlotList() == 10 && LSstatus[1] == SLOT_STATE_EMPTY
        && slot_cursor == 1 && !recovery_slot_active;
    bool emptyRemoved = true;
    for (const char* name : { "SAVE.DAT", "MAP.SAV", "MULTI.DAT", "SAVE.RBK" }) {
        emptyRemoved = !std::filesystem::exists(
            std::filesystem::path(argv[1]) / "SAVEGAME/SLOT02" / name) && emptyRemoved;
    }
    // Removing the transaction record commits new coherent native/metadata
    // bytes. A later backup must preserve those new bytes, not roll them back.
    slot_cursor = 0;
    bool committed = SaveBackup() == 0 && RecordPendingSaveRollback();
    for (const char* path : originals) {
        DB_FILE* stream = db_fopen(path, "wb");
        if (stream == nullptr) { db_exit(); return 15; }
        bool written = db_fwriteInt(stream, 789) == 0;
        written = db_fclose(stream) == 0 && written;
        if (!written) { db_exit(); return 16; }
    }
    committed = ClearPendingSaveRollback() && committed;
    failedSaveRestorations.clear();
    committed = SaveBackup() == 0 && committed;
    for (const char* path : originals) committed = readValue(path, 789) && committed;
    committed = readValue("SAVEGAME\\SLOT01\\SAVE.BAK", 789)
        && readValue("SAVEGAME\\SLOT01\\MULTI.RSV", 789) && committed;
    std::printf("NATIVE_SAVE_TRANSACTION missing_metadata_refused=%d missing_metadata_unchanged=%d empty_prepared=%d empty_blocked=%d empty_record_retained=%d empty_recovered=%d empty_removed=%d commit_retained=%d\n",
        missingMetadataRefused, missingMetadataUnchanged, emptyPrepared, emptyBlocked, emptyRecordRetained, emptyRecovered, emptyRemoved, committed);
    db_exit();
    std::printf("NATIVE_SAVE_BACKUP failure=%d originals_intact=%d normal=%d backups_intact=%d originals_after_backup=%d restore=%d restore_intact=%d\n",
        result, intact, normal, backupIntact, originalsAfterBackup, restoreResult, restoreIntact);
    return result == -1 && intact && normal == 0 && backupIntact && originalsAfterBackup
        && emptyPrepared && emptyBlocked && emptyRecordRetained && emptyRecovered && emptyRemoved && committed
        && missingMetadataRefused && missingMetadataUnchanged
        && transactionRecorded && freshProcessRetained && persistentMarker && malformedBlocked == -1 && malformedRetained && markerRemoved && metadataBackupRemoved
        && refusedRestore == -1 && retryBackups && saveRetryBlocked == -1
        && retryBackupsAfterSave && resumedBackup == 0 && restoreResult == 0 && restoreIntact ? 0 : 1;
}
