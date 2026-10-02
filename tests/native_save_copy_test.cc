#include "game/loadsave.cc"

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
}

int main(int argc, char** argv)
{
    using namespace fallout;
    if (argc != 2 || db_init(nullptr, nullptr, argv[1], 0) == INVALID_DATABASE_HANDLE) return 2;
    DB_FILE* input = db_fopen("SOURCE.TMP", "wb");
    if (input == nullptr) { db_exit(); return 3; }
    bool written = db_fwriteInt(input, 456) == 0;
    written = db_fclose(input) == 0 && written;
    int failed = copy_file("SOURCE.TMP", "FULL.TMP");
    int normal = copy_file("SOURCE.TMP", "NORMAL.TMP");
    DB_FILE* output = db_fopen("NORMAL.TMP", "rb");
    int restored = 0;
    bool intact = output != nullptr;
    if (output != nullptr) {
        intact = db_freadInt(output, &restored) == 0 && restored == 456;
        intact = db_fclose(output) == 0 && intact;
    }
    db_exit();
    std::printf("NATIVE_SAVE_COPY failure=%d normal=%d intact=%d\n", failed, normal, intact);
    return written && failed == -1 && normal == 0 && intact ? 0 : 1;
}
