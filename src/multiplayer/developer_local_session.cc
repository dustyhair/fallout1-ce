#include "multiplayer/developer_local_session.h"

#include <cstring>
#include <optional>

#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/critter.h"
#include "game/editor.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/map_defs.h"
#include "game/object.h"
#include "game/palette.h"
#include "game/protinst.h"
#include "game/stat.h"
#include "game/tile.h"
#include "multiplayer/acting_player_context.h"
#include "multiplayer/character_build_bridge.h"
#include "multiplayer/character_lobby.h"
#include "multiplayer/command_processor.h"
#include "multiplayer/local_player_context.h"
#include "multiplayer/local_session.h"
#include "multiplayer/presentation_bridge.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"

namespace fallout {
namespace multiplayer {
namespace {

bool enabled = false;
Object* guestActor = nullptr;
LocalSession session;
CommandProcessor commandProcessor;
std::uint64_t nextHostCommandSequence = 1;
std::uint64_t nextGuestCommandSequence = 1;
std::optional<MultiplayerSaveSidecar> pendingLoadedSave;
Object* pendingLoadedGuestObject = nullptr;
Inventory preservedGuestInventory {};
int preservedGuestFid = -1;

void eraseGuestActor();

void discardPendingGuestObject()
{
    if (pendingLoadedGuestObject != nullptr) {
        pendingLoadedGuestObject->flags &= ~OBJECT_NO_REMOVE;
        obj_erase_object(pendingLoadedGuestObject, nullptr);
        pendingLoadedGuestObject = nullptr;
    }
}

void discardPreservedGuestInventory()
{
    obj_inven_free(&preservedGuestInventory);
    preservedGuestFid = -1;
}

void detachGuestInventory()
{
    discardPreservedGuestInventory();
    if (guestActor == nullptr) {
        return;
    }

    preservedGuestInventory = guestActor->data.inventory;
    preservedGuestFid = guestActor->fid;
    guestActor->data.inventory = {};
}

void attachInventory(Object* actor, Inventory& inventory)
{
    actor->data.inventory = inventory;
    inventory = {};
    for (int index = 0; index < actor->data.inventory.length; index++) {
        actor->data.inventory.items[index].item->owner = actor;
    }
}

void attachPreservedGuestInventory(Object* actor)
{
    attachInventory(actor, preservedGuestInventory);
    if (preservedGuestFid != -1) {
        obj_change_fid(actor, preservedGuestFid, nullptr);
    }
    preservedGuestFid = -1;
}

bool applyPendingGuestObject(Object* actor)
{
    if (pendingLoadedGuestObject == nullptr) {
        return true;
    }

    Object* saved = pendingLoadedGuestObject;
    pendingLoadedGuestObject = nullptr;
    if (saved->pid != actor->pid) {
        saved->flags &= ~OBJECT_NO_REMOVE;
        obj_erase_object(saved, nullptr);
        return false;
    }

    discardPreservedGuestInventory();
    actor->data.critter = saved->data.critter;
    actor->data.critter.combat.whoHitMe = nullptr;
    attachInventory(actor, saved->data.inventory);
    saved->data.inventory = {};

    int savedFid = saved->fid;
    int savedTile = saved->tile;
    int savedElevation = saved->elevation;
    int savedRotation = saved->rotation;
    saved->flags &= ~OBJECT_NO_REMOVE;
    obj_erase_object(saved, nullptr);

    obj_change_fid(actor, savedFid, nullptr);
    if (hexGridTileIsValid(savedTile) && elevationIsValid(savedElevation)) {
        obj_attempt_placement(actor, savedTile, savedElevation, 2);
    }
    dude_stand(actor, savedRotation, -1);
    return true;
}

class EngineCommandExecutor : public CommandExecutor {
public:
    CommandExecutionStatus move(Object* actor, const MoveCommand& command) override
    {
        if (isInCombat()
            || !hexGridTileIsValid(command.destinationTile)
            || !elevationIsValid(command.elevation)
            || command.elevation != actor->elevation
            || command.destinationTile == actor->tile
            || make_path(actor, actor->tile, command.destinationTile, nullptr, 1) == 0) {
            return CommandExecutionStatus::InvalidAction;
        }

        int requestOptions = actor == obj_dude
            ? ANIMATION_REQUEST_RESERVED
            : ANIMATION_REQUEST_UNRESERVED;
        if (register_begin(requestOptions) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }

        int rc = command.running
            ? register_object_run_to_tile(actor, command.destinationTile, command.elevation, -1, 0)
            : register_object_move_to_tile(actor, command.destinationTile, command.elevation, -1, 0);
        if (rc == -1 || register_end() == -1) {
            return CommandExecutionStatus::InvalidAction;
        }

        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus face(Object* actor, const FaceCommand& command) override
    {
        if (command.rotation < 0 || command.rotation >= ROTATION_COUNT) {
            return CommandExecutionStatus::InvalidAction;
        }
        Rect dirtyRect;
        if (obj_set_rotation(actor, command.rotation, &dirtyRect) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }
        tile_refresh_rect(&dirtyRect, actor->elevation);
        return CommandExecutionStatus::Applied;
    }

    DoorUseExecution useDoor(Object* actor, Object* target) override
    {
        DoorUseExecution execution;
        if (isInCombat()
            || actor->elevation != target->elevation
            || !obj_is_a_portal(target)
            || action_use_an_object(actor, target) == -1) {
            return execution;
        }

        execution.status = CommandExecutionStatus::Applied;
        execution.open = obj_is_open(target) != 0;
        execution.locked = obj_is_locked(target);
        execution.frame = target->frame;
        return execution;
    }

    CommandExecutionStatus pickup(Object* actor, Object* target) override
    {
        if (isInCombat()
            || actor == target
            || actor->elevation != target->elevation
            || FID_TYPE(target->fid) != OBJ_TYPE_ITEM
            || target->owner != nullptr
            || action_get_an_object(actor, target) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }

        return CommandExecutionStatus::Applied;
    }

    CommandExecutionStatus loot(Object* actor, Object* target) override
    {
        if (isInCombat()
            || actor == target
            || actor->elevation != target->elevation
            || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER
            || action_loot_container(actor, target) == -1) {
            return CommandExecutionStatus::InvalidAction;
        }

        return CommandExecutionStatus::Applied;
    }

    InventoryTransferExecution transferInventory(Object*, Object*, Object*, Object*, const InventoryTransferCommand&) override
    {
        return {};
    }

    ItemDropExecution dropItem(Object*, Object*, Object*, const ItemDropCommand&) override
    {
        return {};
    }
};

EngineCommandExecutor commandExecutor;

std::uint64_t* nextCommandSequence(PlayerId playerId)
{
    if (playerId == kHostPlayerId) {
        return &nextHostCommandSequence;
    }
    if (playerId == kGuestPlayerId) {
        return &nextGuestCommandSequence;
    }
    return nullptr;
}

bool submitCommand(PlayerId playerId, GameCommandPayload payload)
{
    std::uint64_t* nextSequence = nextCommandSequence(playerId);
    EntityId actorId = session.playerActorId(playerId);
    if (!enabled || !session.isActive() || nextSequence == nullptr || !isValid(actorId)) {
        return false;
    }

    GameCommand command;
    command.sequence.value = (*nextSequence)++;
    command.playerId = playerId;
    command.actorId = actorId;
    command.expectedPhase = session.phase();
    command.expectedPhaseRevision = session.phaseRevision();
    command.payload = payload;

    AuthoritativeCommandResult authoritative = commandProcessor.process(command, session, commandExecutor);
    if (authoritative.result.status == CommandStatus::Rejected) {
        debug_printf("Multiplayer command %llu rejected with reason %d.\n",
            static_cast<unsigned long long>(command.sequence.value),
            static_cast<int>(authoritative.result.rejection));
        return false;
    }

    return true;
}

Object* createGuestActor()
{
    if (obj_dude == nullptr || obj_dude->tile == -1) {
        return nullptr;
    }

    Object* actor = nullptr;
    if (obj_pid_new(&actor, obj_dude->pid) == -1) {
        return nullptr;
    }

    actor->flags |= OBJECT_NO_SAVE;
    actor->flags &= ~OBJECT_NO_REMOVE;
    actor->data.critter.combat.aiPacket = 0;
    actor->data.critter.combat.team = obj_dude->data.critter.combat.team;

    if (obj_change_fid(actor, obj_dude->fid, nullptr) == -1
        || obj_attempt_placement(actor, obj_dude->tile, obj_dude->elevation, 2) == -1) {
        obj_erase_object(actor, nullptr);
        return nullptr;
    }

    dude_stand(actor, obj_dude->rotation, -1);
    return actor;
}

bool isNewCharacterBuild(const CharacterBuild& build)
{
    if (build.level != 1 || build.experience != 0 || build.unspentSkillPoints != 0) {
        return false;
    }
    for (std::int32_t points : build.skillPoints) {
        if (points != 0) {
            return false;
        }
    }
    for (std::int32_t rank : build.perkRanks) {
        if (rank != 0) {
            return false;
        }
    }
    return true;
}

bool refreshPlayerBuild(PlayerId playerId, bool healToFull = true)
{
    PlayerCharacterState* player = session.players().find(playerId);
    Object* actor = player != nullptr ? session.entities().findObject(player->actorId) : nullptr;
    if (player == nullptr || actor == nullptr) {
        return false;
    }

    ScopedActingPlayerContext actingPlayer(*player, actor);
    stat_recalc_derived(actor);
    if (healToFull) {
        int hitPoints = critter_get_hits(actor);
        int maximumHitPoints = stat_level(actor, STAT_MAXIMUM_HIT_POINTS);
        critter_adjust_hits(actor, maximumHitPoints - hitPoints);
    }
    return updatePlayerGenderAppearance(actor) == 0;
}

bool submitCharacterSheet(const CharacterCreationSheet& sheet)
{
    std::vector<std::uint8_t> packet;
    CharacterLobbyError error = encodeCharacterSheet(sheet, packet);
    if (error == CharacterLobbyError::None) {
        CharacterSheetDecodeResult decoded = decodeCharacterSheet(packet);
        error = decoded.error;
        if (decoded) {
            error = session.submitCharacterSheet(decoded.sheet);
        }
    }
    if (error != CharacterLobbyError::None) {
        debug_printf("Multiplayer character sheet for player %u rejected with reason %d.\n",
            sheet.playerId.value,
            static_cast<int>(error));
        return false;
    }
    return refreshPlayerBuild(sheet.playerId);
}

void cancelLobby()
{
    session.stop();
    eraseGuestActor();
    enabled = false;
    debug_printf("Multiplayer developer lobby cancelled; continuing in single-player mode.\n");
}

void eraseGuestActor()
{
    if (guestActor == nullptr) {
        return;
    }

    register_clear(guestActor);
    obj_erase_object(guestActor, nullptr);
    guestActor = nullptr;
}

bool beginSession()
{
    CharacterBuild hostBuild;
    if (!captureLegacyCharacterBuild(obj_dude, hostBuild) || !isNewCharacterBuild(hostBuild)) {
        enabled = false;
        debug_printf("Multiplayer developer lobby requires a new level-one character; existing-save conversion is not available yet.\n");
        return false;
    }

    guestActor = createGuestActor();
    if (guestActor == nullptr) {
        return false;
    }

    if (session.start(obj_dude, guestActor) != LocalSessionError::None) {
        session.stop();
        eraseGuestActor();
        return false;
    }

    CharacterCreationSheet hostSheet = characterSheetFromBuild(kHostPlayerId, critter_name(obj_dude), hostBuild);
    if (!submitCharacterSheet(hostSheet)) {
        cancelLobby();
        return false;
    }

    CharacterCreationSheet guestDraft;
    guestDraft.playerId = kGuestPlayerId;
    guestDraft.name = "Guest";
    PlayerCharacterState* guest = session.players().find(kGuestPlayerId);
    if (guest == nullptr
        || session.players().setName(kGuestPlayerId, guestDraft.name) != PlayerStateError::None
        || session.players().setBuild(kGuestPlayerId, characterBuildFromSheet(guestDraft)) != PlayerStateError::None
        || bindLocalPlayer(session, kGuestPlayerId) != LocalPlayerError::None
        || !refreshPlayerBuild(kGuestPlayerId)) {
        cancelLobby();
        return false;
    }

    CharEditInit();
    int editorResult = editor_design(true);
    palette_fade_to(cmap);
    if (editorResult != 0) {
        cancelLobby();
        return false;
    }

    guest = session.players().find(kGuestPlayerId);
    CharacterCreationSheet guestSheet = characterSheetFromBuild(kGuestPlayerId, guest->name, guest->build);
    if (!submitCharacterSheet(guestSheet)
        || !session.characterLobbyReady()
        || bindLocalPlayer(session, kHostPlayerId) != LocalPlayerError::None
        || session.transitionTo(SessionPhase::Loading) != LocalSessionError::None
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None) {
        cancelLobby();
        return false;
    }
    intface_redraw();

    commandProcessor.reset();
    nextHostCommandSequence = 1;
    nextGuestCommandSequence = 1;

    debug_printf("Multiplayer developer session started with validated host and guest characters.\n");
    return true;
}

bool beginRestoredSession()
{
    if (!pendingLoadedSave.has_value()) {
        return false;
    }

    guestActor = createGuestActor();
    if (guestActor == nullptr) {
        return false;
    }
    if (!applyPendingGuestObject(guestActor)
        || session.start(obj_dude, guestActor) != LocalSessionError::None
        || session.restorePlayerCharacters(*pendingLoadedSave) != LocalSessionError::None
        || bindLocalPlayer(session, kHostPlayerId) != LocalPlayerError::None
        || session.transitionTo(SessionPhase::Loading) != LocalSessionError::None
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None
        || !refreshPlayerBuild(kHostPlayerId, false)
        || !refreshPlayerBuild(kGuestPlayerId, false)) {
        session.stop();
        eraseGuestActor();
        return false;
    }

    pendingLoadedSave.reset();
    commandProcessor.reset();
    nextHostCommandSequence = 1;
    nextGuestCommandSequence = 1;
    intface_redraw();
    debug_printf("Multiplayer developer session restored from slot sidecar.\n");
    return true;
}

bool finishMapTransition()
{
    Object* replacement = createGuestActor();
    if (replacement == nullptr) {
        return false;
    }

    if (pendingLoadedGuestObject != nullptr) {
        if (!applyPendingGuestObject(replacement)) {
            obj_erase_object(replacement, nullptr);
            return false;
        }
    } else {
        attachPreservedGuestInventory(replacement);
    }

    if (session.rebindPlayerActor(kGuestPlayerId, replacement) != LocalSessionError::None) {
        obj_erase_object(replacement, nullptr);
        return false;
    }

    guestActor = replacement;
    if ((pendingLoadedSave.has_value()
            && session.restorePlayerCharacters(*pendingLoadedSave) != LocalSessionError::None)
        || session.transitionTo(SessionPhase::Exploration) != LocalSessionError::None
        || !refreshPlayerBuild(kGuestPlayerId, false)
        || (pendingLoadedSave.has_value() && !refreshPlayerBuild(kHostPlayerId, false))) {
        eraseGuestActor();
        session.stop();
        return false;
    }

    pendingLoadedSave.reset();
    commandProcessor.reset();
    nextHostCommandSequence = 1;
    nextGuestCommandSequence = 1;
    intface_redraw();
    return true;
}

} // namespace

void developerLocalSessionConfigure(int argc, char** argv)
{
    enabled = false;
    pendingLoadedSave.reset();
    discardPendingGuestObject();
    discardPreservedGuestInventory();
    for (int index = 1; index < argc; index++) {
        if (std::strcmp(argv[index], "--multiplayer-dev") == 0) {
            enabled = true;
            break;
        }
    }
}

bool developerLocalSessionIsEnabled()
{
    return enabled;
}

bool developerLocalSessionIsActive()
{
    return enabled && session.isActive();
}

bool developerLocalSessionEnsureStarted()
{
    if (!enabled) {
        return true;
    }

    if (!session.isActive()) {
        if (pendingLoadedSave.has_value()) {
            return beginRestoredSession();
        }
        return beginSession();
    }

    if (guestActor == nullptr && session.phase() == SessionPhase::Transition) {
        return finishMapTransition();
    }

    return guestActor != nullptr;
}

MultiplayerSaveError developerLocalSessionCaptureSave(std::uint64_t generation,
    std::uint64_t saveDatDigest,
    MultiplayerSaveSidecar& sidecar)
{
    if (!enabled || !session.isActive()) {
        return MultiplayerSaveError::PlayerMissing;
    }
    return captureMultiplayerSave(session.players(), generation, saveDatDigest, sidecar);
}

bool developerLocalSessionStageLoadedSave(const MultiplayerSaveSidecar& sidecar)
{
    if (!enabled
        || validateMultiplayerSave(sidecar) != MultiplayerSaveError::None
        || (!sidecar.guestObjectData.empty() && pendingLoadedGuestObject == nullptr)) {
        return false;
    }
    if (sidecar.guestObjectData.empty()) {
        discardPendingGuestObject();
        discardPreservedGuestInventory();
    }
    pendingLoadedSave = sidecar;
    return true;
}

bool developerLocalSessionWriteGuestObject(const char* relativePath)
{
    if (!enabled || !session.isActive() || guestActor == nullptr || relativePath == nullptr) {
        return false;
    }

    DB_FILE* stream = db_fopen(relativePath, "wb");
    if (stream == nullptr) {
        return false;
    }

    int savedFlags = guestActor->flags;
    int savedSid = guestActor->sid;
    guestActor->flags &= ~OBJECT_NO_SAVE;
    guestActor->sid = -1;
    int rc = obj_save_obj(stream, guestActor);
    guestActor->flags = savedFlags;
    guestActor->sid = savedSid;
    return db_fclose(stream) == 0 && rc == 0;
}

bool developerLocalSessionStageLoadedGuestObject(const char* relativePath)
{
    if (!enabled || relativePath == nullptr) {
        return false;
    }

    DB_FILE* stream = db_fopen(relativePath, "rb");
    if (stream == nullptr) {
        return false;
    }

    Object* loaded = nullptr;
    int rc = obj_load_obj(stream, &loaded, -1, nullptr);
    bool consumed = rc == 0 && db_ftell(stream) == db_filelength(stream);
    db_fclose(stream);
    if (!consumed || loaded == nullptr || PID_TYPE(loaded->pid) != OBJ_TYPE_CRITTER) {
        if (loaded != nullptr) {
            loaded->flags &= ~OBJECT_NO_REMOVE;
            obj_erase_object(loaded, nullptr);
        }
        return false;
    }

    discardPendingGuestObject();
    loaded->flags |= OBJECT_NO_SAVE;
    loaded->flags &= ~OBJECT_NO_REMOVE;
    pendingLoadedGuestObject = loaded;
    return true;
}

void developerLocalSessionRejectLoadedSave()
{
    pendingLoadedSave.reset();
    discardPendingGuestObject();
    discardPreservedGuestInventory();
    if (session.isActive()) {
        eraseGuestActor();
        session.stop();
        commandProcessor.reset();
    }
}

bool developerLocalSessionOpenGuestInventory()
{
    if (!enabled
        || !session.isActive()
        || guestActor == nullptr
        || session.phase() != SessionPhase::Exploration) {
        return false;
    }

    {
        ScopedLocalPlayerBinding guestBinding(session, kGuestPlayerId);
        if (!guestBinding) {
            return false;
        }
        handle_inventory();
    }

    intface_redraw();
    return true;
}

bool developerLocalSessionSubmitMove(PlayerId playerId, int destinationTile, int elevation, bool running)
{
    return submitCommand(playerId, MoveCommand { destinationTile, elevation, running });
}

bool developerLocalSessionSubmitDoorUse(PlayerId playerId, Object* target)
{
    if (!enabled || !session.isActive() || target == nullptr || !obj_is_a_portal(target)) {
        return false;
    }

    EntityRegistrationResult registered = session.registerWorldObject(target);
    if (!registered) {
        return false;
    }

    return submitCommand(playerId, InteractCommand { registered.entityId });
}

bool developerLocalSessionSubmitPickup(PlayerId playerId, Object* target)
{
    if (!enabled || !session.isActive() || target == nullptr || FID_TYPE(target->fid) != OBJ_TYPE_ITEM) {
        return false;
    }

    EntityRegistrationResult registered = session.registerWorldObject(target);
    if (!registered) {
        return false;
    }

    return submitCommand(playerId, PickupCommand { registered.entityId });
}

bool developerLocalSessionSubmitLoot(PlayerId playerId, Object* target)
{
    if (!enabled || !session.isActive() || target == nullptr || FID_TYPE(target->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    EntityRegistrationResult registered = session.registerWorldObject(target);
    if (!registered) {
        return false;
    }

    return submitCommand(playerId, LootCommand { registered.entityId });
}

void developerLocalSessionPrepareForWorldReset()
{
    if (!enabled || !session.isActive()) {
        return;
    }

    session.clearWorldEntities();

    if (session.phase() != SessionPhase::Transition
        && session.transitionTo(SessionPhase::Transition) != LocalSessionError::None) {
        debug_printf("Multiplayer developer session stopped during an unexpected world reset.\n");
        eraseGuestActor();
        session.stop();
        commandProcessor.reset();
        return;
    }

    detachGuestInventory();
    eraseGuestActor();
}

void developerLocalSessionStop()
{
    pendingLoadedSave.reset();
    discardPendingGuestObject();
    discardPreservedGuestInventory();
    eraseGuestActor();
    session.stop();
    commandProcessor.reset();
}

} // namespace multiplayer
} // namespace fallout
