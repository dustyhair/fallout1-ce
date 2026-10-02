#include "plib/db/db.h"

#include <cstdio>

// The standalone native-I/O probe replaces the desktop entry point.
namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
}

int main(int argc, char** argv)
{
    using namespace fallout;
    if (argc != 2 || db_init(nullptr, nullptr, argv[1], 0) == INVALID_DATABASE_HANDLE) return 2;
    DB_FILE* failed = db_fopen("FULL.TMP", "wb");
    if (failed == nullptr) { db_exit(); return 3; }
    // /dev/full accepts the buffered write, then rejects its flush on close.
    int writeResult = db_fwriteInt(failed, 123);
    int closeResult = db_fclose(failed);
    DB_FILE* normal = db_fopen("NORMAL.TMP", "wb");
    bool normalPassed = normal != nullptr;
    if (normal != nullptr) {
        normalPassed = db_fwriteInt(normal, 456) == 0;
        normalPassed = db_fclose(normal) == 0 && normalPassed;
    }
    normal = db_fopen("NORMAL.TMP", "rb");
    int restored = 0;
    if (normal != nullptr) {
        normalPassed = db_freadInt(normal, &restored) == 0 && restored == 456 && normalPassed;
        normalPassed = db_fclose(normal) == 0 && normalPassed;
    } else {
        normalPassed = false;
    }
    db_exit();
    std::printf("NATIVE_DB_CLOSE write=%d close=%d normal=%d\n", writeResult, closeResult, normalPassed);
    return writeResult == 0 && closeResult != 0 && normalPassed ? 0 : 1;
}
