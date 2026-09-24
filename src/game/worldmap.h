#ifndef FALLOUT_GAME_WORLDMAP_H_
#define FALLOUT_GAME_WORLDMAP_H_

#include <array>
#include <cstdint>

#include "plib/db/db.h"

namespace fallout {

typedef enum MapFlags {
    MAP_SAVED = 0x01,
    MAP_DEAD_BODIES_AGE = 0x02,
    MAP_PIPBOY_ACTIVE = 0x04,
    MAP_CAN_REST_ELEVATION_0 = 0x08,
    MAP_CAN_REST_ELEVATION_1 = 0x10,
    MAP_CAN_REST_ELEVATION_2 = 0x20,
} MapFlags;

typedef enum City {
    TOWN_VAULT_13 = 0,
    TOWN_VAULT_15 = 1,
    TOWN_SHADY_SANDS = 2,
    TOWN_JUNKTOWN = 3,
    TOWN_RAIDERS = 4,
    TOWN_NECROPOLIS = 5,
    TOWN_THE_HUB = 6,
    TOWN_BROTHERHOOD = 7,
    TOWN_MILITARY_BASE = 8,
    TOWN_THE_GLOW = 9,
    TOWN_BONEYARD = 10,
    TOWN_CATHEDRAL = 11,
    TOWN_COUNT = 12,
    TOWN_SPECIAL_12 = 12,
    TOWN_SPECIAL_13 = 13,
    TOWN_SPECIAL_14 = 14,
} City;

typedef enum Map {
    MAP_DESERT1,
    MAP_DESERT2,
    MAP_DESERT3,
    MAP_HALLDED,
    MAP_HOTEL,
    MAP_WATRSHD,
    MAP_VAULT13,
    MAP_VAULTENT,
    MAP_VAULTBUR,
    MAP_VAULTNEC,
    MAP_JUNKENT,
    MAP_JUNKCSNO,
    MAP_JUNKKILL,
    MAP_BROHDENT,
    MAP_BROHD12,
    MAP_BROHD34,
    MAP_CAVES,
    MAP_CHILDRN1,
    MAP_CHILDRN2,
    MAP_CITY1,
    MAP_COAST1,
    MAP_COAST2,
    MAP_COLATRUK,
    MAP_FSAUSER,
    MAP_RAIDERS,
    MAP_SHADYE,
    MAP_SHADYW,
    MAP_GLOWENT,
    MAP_LAADYTUM,
    MAP_LAFOLLWR,
    MAP_MBENT,
    MAP_MBSTRG12,
    MAP_MBVATS12,
    MAP_MSTRLR12,
    MAP_MSTRLR34,
    MAP_V13ENT,
    MAP_HUBENT,
    MAP_DETHCLAW,
    MAP_HUBDWNTN,
    MAP_HUBHEIGT,
    MAP_HUBOLDTN,
    MAP_HUBWATER,
    MAP_GLOW1,
    MAP_GLOW2,
    MAP_LABLADES,
    MAP_LARIPPER,
    MAP_LAGUNRUN,
    MAP_CHILDEAD,
    MAP_MBDEAD,
    MAP_MOUNTN1,
    MAP_MOUNTN2,
    MAP_FOOT,
    MAP_TARDIS,
    MAP_TALKCOW,
    MAP_USEDCAR,
    MAP_BRODEAD,
    MAP_DESCRVN1,
    MAP_DESCRVN2,
    MAP_MNTCRVN1,
    MAP_MNTCRVN2,
    MAP_VIPERS,
    MAP_DESCRVN3,
    MAP_MNTCRVN3,
    MAP_DESCRVN4,
    MAP_MNTCRVN4,
    MAP_HUBMIS1,
    MAP_COUNT,
} Map;

typedef enum TerrainType {
    TERRAIN_TYPE_DESERT,
    TERRAIN_TYPE_MOUNTAIN,
    TERRAIN_TYPE_CITY,
    TERRAIN_TYPE_COAST,
} TerrainType;

typedef enum WorldmapFrm {
    WORLDMAP_FRM_LITTLE_RED_BUTTON_NORMAL,
    WORLDMAP_FRM_LITTLE_RED_BUTTON_PRESSED,
    WORLDMAP_FRM_BOX,
    WORLDMAP_FRM_LABELS,
    WORLDMAP_FRM_LOCATION_MARKER,
    WORLDMAP_FRM_DESTINATION_MARKER_BRIGHT,
    WORLDMAP_FRM_DESTINATION_MARKER_DARK,
    WORLDMAP_FRM_RANDOM_ENCOUNTER_BRIGHT,
    WORLDMAP_FRM_RANDOM_ENCOUNTER_DARK,
    WORLDMAP_FRM_WORLDMAP,
    WORLDMAP_FRM_MONTHS,
    WORLDMAP_FRM_NUMBERS,
    WORLDMAP_FRM_HOTSPOT_NORMAL,
    WORLDMAP_FRM_HOTSPOT_PRESSED,
    WORLDMAP_FRM_COUNT,
} WorldmapFrm;

typedef enum TownmapFrm {
    TOWNMAP_FRM_BOX,
    TOWNMAP_FRM_LABELS,
    TOWNMAP_FRM_HOTSPOT_PRESSED,
    TOWNMAP_FRM_HOTSPOT_NORMAL,
    TOWNMAP_FRM_LITTLE_RED_BUTTON_NORMAL,
    TOWNMAP_FRM_LITTLE_RED_BUTTON_PRESSED,
    TOWNMAP_FRM_MONTHS,
    TOWNMAP_FRM_NUMBERS,
    TOWNMAP_FRM_COUNT,
} TownmapFrm;

typedef struct WorldMapContext {
    short state;
    short town;
    short section;
} WorldMapContext;

// Persistent world-map/encounter state. The arrays match the save-game data;
// transient UI buffers and travel-animation counters are deliberately absent.
struct WorldMapState {
    std::array<std::uint8_t, 31 * 29> grid {};
    std::array<std::uint8_t, 15 * 7> knownTownEntrances {};
    std::int32_t firstVisits = 0;
    std::int32_t specialEncounters = 0;
    std::int32_t town = 0;
    std::int32_t section = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
};

void worldmap_capture_state(WorldMapState& state);
bool worldmap_apply_state(const WorldMapState& state);
bool worldmap_multiplayer_choose_destination(bool encounter,
    int specialEncounter,
    bool enterCity,
    int* map,
    int* entranceIndex);

// A host-only, headless travel step. This deliberately stops before any map
// load or world-map movie. Multiplayer gameplay must use
// networkWorldAdvanceWorldMapTravel so remote player healing is also applied;
// callers must publish the resulting state and resolve interruptions.
enum class WorldMapTravelStepStatus {
    Invalid,
    Moving,
    Arrived,
    Blocked,
    QueueInterrupted,
    Encounter,
    WorldEventPending,
};

struct WorldMapTravelStepResult {
    WorldMapTravelStepStatus status = WorldMapTravelStepStatus::Invalid;
    int x = 0;
    int y = 0;
    int gameTime = 0;
    int specialEncounter = 0; // Zero denotes an ordinary encounter.
    bool dayElapsed = false;
};

struct WorldMapTravelProgress {
    bool active = false;
    std::int32_t targetX = -1;
    std::int32_t targetY = -1;
    std::int32_t deltaX = 0;
    std::int32_t deltaY = 0;
    std::int32_t lineError = 0;
    std::int32_t lineIndex = 0;
    std::int32_t xIncrement = 0;
    std::int32_t yIncrement = 0;
    std::int32_t moveCounter = 0;
    std::int32_t visualCounter = 0;
    std::int32_t miles = 0;
    std::int32_t dayLength = 0;
    std::int32_t timeAdder = 0;
};

bool worldmap_authoritative_travel_begin(int targetX, int targetY);
WorldMapTravelStepResult worldmap_authoritative_travel_step();
void worldmap_authoritative_travel_cancel();
void worldmap_capture_travel_progress(WorldMapTravelProgress& progress);
inline bool worldmap_validate_travel_progress(const WorldMapTravelProgress& progress, const WorldMapState& worldMap)
{
    if (!progress.active) {
        return progress.targetX == -1 && progress.targetY == -1
            && progress.deltaX == 0 && progress.deltaY == 0
            && progress.lineError == 0 && progress.lineIndex == 0
            && progress.xIncrement == 0 && progress.yIncrement == 0
            && progress.moveCounter == 0 && progress.visualCounter == 0
            && progress.miles == 0 && progress.dayLength == 0
            && progress.timeAdder == 0;
    }

    int pathLength = progress.deltaX > progress.deltaY ? progress.deltaX : progress.deltaY;
    return progress.targetX >= 0 && progress.targetX < 1400
        && progress.targetY >= 0 && progress.targetY < 1500
        && worldMap.x >= 0 && worldMap.x < 1400
        && worldMap.y >= 0 && worldMap.y < 1500
        && progress.deltaX >= 0 && progress.deltaX < 1400
        && progress.deltaY >= 0 && progress.deltaY < 1500
        && progress.lineIndex >= 0 && progress.lineIndex <= pathLength
        && progress.lineError >= -pathLength && progress.lineError <= pathLength
        && (progress.xIncrement == -1 || progress.xIncrement == 1)
        && (progress.yIncrement == -1 || progress.yIncrement == 1)
        && progress.moveCounter >= 0 && progress.moveCounter <= 4
        && progress.visualCounter >= 0 && progress.visualCounter <= 2
        && progress.dayLength >= 60 && progress.dayLength <= 120
        && progress.miles >= 0 && progress.miles < progress.dayLength
        && progress.timeAdder >= 0 && progress.timeAdder <= 14400;
}
bool worldmap_apply_travel_progress(const WorldMapTravelProgress& progress);

extern int world_win;
extern int our_section;
extern int our_town;

int init_world_map();
int save_world_map(DB_FILE* stream);
int load_world_map(DB_FILE* stream);
int world_map(WorldMapContext ctx);
void worldmap_multiplayer_open();
WorldMapContext town_map(WorldMapContext ctx);
void KillWorldWin();
int worldmap_script_jump(int city, int a2);
int xlate_mapidx_to_town(int map_idx);
int PlayCityMapMusic();

} // namespace fallout

#endif /* FALLOUT_GAME_WORLDMAP_H_ */
