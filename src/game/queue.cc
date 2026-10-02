#include "game/queue.h"

#include <cstdint>

#include "game/actions.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/gsound.h"
#include "game/item.h"
#include "game/map.h"
#include "game/message.h"
#include "game/object.h"
#include "game/perk.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/scripts.h"
#include "multiplayer/network_runtime.h"
#include "multiplayer/network_world.h"
#include "plib/gnw/memory.h"

namespace fallout {

typedef struct QueueListNode {
    // TODO: Make unsigned.
    int time;
    int type;
    Object* owner;
    // Native IDs survive serialization even when the guest is restored later.
    int savedOwnerId;
    // In-memory identity only; native saves and multiplayer snapshots serialize
    // explicit event fields, not this traversal handle.
    std::uint64_t generation;
    void* data;
    struct QueueListNode* next;
} QueueListNode;

static std::uint64_t nextQueueNodeGeneration = 1;

static int queue_destroy(Object* obj, void* data);
static int queue_explode(Object* obj, void* data);
static int queue_explode_exit(Object* obj, void* data);
static int queue_do_explosion(Object* obj, bool a2, int playerId = 0);
static int queue_player_explode(Object* obj, void* data);
static int queue_player_premature(Object* obj, void* data);
static int queue_player_explode_exit(Object* obj, void* data);
static int queue_player_explosion_load(DB_FILE* stream, void** data);
static int queue_player_explosion_save(DB_FILE* stream, void* data);
static int queue_premature(Object* obj, void* data);

static std::size_t queue_event_payload_count(int eventType)
{
    switch (eventType) {
    case EVENT_TYPE_PLAYER_EXPLOSION:
    case EVENT_TYPE_PLAYER_EXPLOSION_FAILURE:
        return 1;
    case EVENT_TYPE_DRUG:
        return 6;
    case EVENT_TYPE_WITHDRAWAL:
        return 3;
    case EVENT_TYPE_SCRIPT:
    case EVENT_TYPE_RADIATION:
        return 2;
    default:
        return 0;
    }
}

static void queue_free_list(QueueListNode* list)
{
    while (list != NULL) {
        QueueListNode* next = list->next;
        if (list->type >= 0 && list->type < EVENT_TYPE_COUNT) {
            EventTypeDescription* eventTypeDescription = &(q_func[list->type]);
            if (eventTypeDescription->freeProc != NULL) {
                eventTypeDescription->freeProc(list->data);
            }
        }
        mem_free(list);
        list = next;
    }
}

static void* queue_create_event_data(const QueueEventState& state)
{
    switch (state.eventType) {
    case EVENT_TYPE_PLAYER_EXPLOSION:
    case EVENT_TYPE_PLAYER_EXPLOSION_FAILURE: {
        auto* event = static_cast<PlayerExplosionEvent*>(mem_malloc(sizeof(PlayerExplosionEvent)));
        if (event != nullptr) event->playerId = state.payload[0];
        return event;
    }
    case EVENT_TYPE_DRUG: {
        DrugEffectEvent* event = (DrugEffectEvent*)mem_malloc(sizeof(*event));
        if (event != NULL) {
            event->drugPid = 0;
            for (int index = 0; index < 3; index++) {
                event->stats[index] = state.payload[index];
                event->modifiers[index] = state.payload[index + 3];
            }
        }
        return event;
    }
    case EVENT_TYPE_WITHDRAWAL: {
        WithdrawalEvent* event = (WithdrawalEvent*)mem_malloc(sizeof(*event));
        if (event != NULL) {
            event->field_0 = state.payload[0];
            event->pid = state.payload[1];
            event->perk = state.payload[2];
        }
        return event;
    }
    case EVENT_TYPE_SCRIPT: {
        ScriptEvent* event = (ScriptEvent*)mem_malloc(sizeof(*event));
        if (event != NULL) {
            event->sid = state.payload[0];
            event->fixedParam = state.payload[1];
        }
        return event;
    }
    case EVENT_TYPE_RADIATION: {
        RadiationEvent* event = (RadiationEvent*)mem_malloc(sizeof(*event));
        if (event != NULL) {
            event->radiationLevel = state.payload[0];
            event->isHealing = state.payload[1];
        }
        return event;
    }
    default:
        return NULL;
    }
}

// 0x5076FC
EventTypeDescription q_func[EVENT_TYPE_COUNT] = {
    { item_d_process, mem_free, item_d_load, item_d_save, true, item_d_clear },
    { critter_wake_up, NULL, NULL, NULL, true, critter_wake_clear },
    { item_wd_process, mem_free, item_wd_load, item_wd_save, true, item_wd_clear },
    { script_q_process, mem_free, script_q_load, script_q_save, true, NULL },
    { gtime_q_process, NULL, NULL, NULL, true, NULL },
    { critter_check_poison, NULL, NULL, NULL, false, NULL },
    { critter_process_rads, mem_free, critter_load_rads, critter_save_rads, false, NULL },
    { queue_destroy, NULL, NULL, NULL, true, queue_destroy },
    { queue_explode, NULL, NULL, NULL, true, queue_explode_exit },
    { item_m_trickle, NULL, NULL, NULL, true, item_m_turn_off_from_queue },
    { critter_sneak_check, NULL, NULL, NULL, true, critter_sneak_clear },
    { queue_premature, NULL, NULL, NULL, true, queue_explode_exit },
    { scr_map_q_process, NULL, NULL, NULL, true, NULL },
    { queue_player_explode, mem_free, queue_player_explosion_load, queue_player_explosion_save, true, queue_player_explode_exit },
    { queue_player_premature, mem_free, queue_player_explosion_load, queue_player_explosion_save, true, queue_player_explode_exit },
};

// 0x662F4C
static QueueListNode* queue;
static Object* mapExitPeerActor;
static int mapExitEventType;

static bool persistent_actor_event(int eventType)
{
    return eventType == EVENT_TYPE_DRUG || eventType == EVENT_TYPE_WITHDRAWAL
        || eventType == EVENT_TYPE_POISON || eventType == EVENT_TYPE_RADIATION;
}

static int clear_peer_map_exit_event(Object* owner, void* data)
{
    if (owner == nullptr || (owner != mapExitPeerActor && obj_top_environment(owner) != mapExitPeerActor)) return 0;
    if (owner == mapExitPeerActor && persistent_actor_event(mapExitEventType)) return 0;
    auto* handler = q_func[mapExitEventType].field_14;
    return handler != nullptr ? handler(owner, data) : 1;
}

DetachedActorQueueEvents::DetachedActorQueueEvents(Object* actor)
    : _actor(actor)
{
    if (actor == nullptr) return;
    Object* previousActor = mapExitPeerActor;
    int previousType = mapExitEventType;
    mapExitPeerActor = actor;
    // Run native exit effects while carried items still have a real inventory
    // owner. In particular destruction must remove an item before inventory is
    // detached, and wake-up must affect the copied peer state.
    for (int type = 0; type < EVENT_TYPE_COUNT; ++type) {
        if (!q_func[type].field_10) continue;
        mapExitEventType = type;
        queue_clear_type(type, clear_peer_map_exit_event);
    }
    mapExitPeerActor = previousActor;
    mapExitEventType = previousType;
    QueueListNode* detached = nullptr;
    QueueListNode** tail = &detached;
    QueueListNode** link = &queue;
    while (*link != nullptr) {
        QueueListNode* node = *link;
        bool persistent = persistent_actor_event(node->type) || node->type == EVENT_TYPE_KNOCKOUT;
        bool carried = node->owner != nullptr && node->owner != actor
            && obj_top_environment(node->owner) == actor;
        if ((node->owner == actor && persistent) || carried) {
            *link = node->next;
            node->next = nullptr;
            *tail = node;
            tail = &node->next;
        } else {
            link = &node->next;
        }
    }
    _events = detached;
}

DetachedActorQueueEvents::~DetachedActorQueueEvents()
{
    queue_free_list(static_cast<QueueListNode*>(_events));
}

void DetachedActorQueueEvents::restore(Object* replacement)
{
    if (replacement == nullptr) return;
    auto* detached = static_cast<QueueListNode*>(_events);
    _events = nullptr;
    while (detached != nullptr) {
        QueueListNode* node = detached;
        detached = node->next;
        if (node->owner == _actor) node->owner = replacement;
        node->savedOwnerId = -2;
        if (node->owner != nullptr) node->owner->flags |= OBJECT_USED;
        QueueListNode** link = &queue;
        while (*link != nullptr && (*link)->time <= node->time) link = &(*link)->next;
        node->next = *link;
        *link = node;
    }
}

// 0x490670
void queue_init()
{
    queue = NULL;
}

// 0x490680
int queue_reset()
{
    queue_clear();
    return 0;
}

// 0x490680
int queue_exit()
{
    queue_clear();
    return 0;
}

// 0x490688
int queue_load(DB_FILE* stream)
{
    int count;
    if (db_freadInt(stream, &count) == -1 || count < 0
        || count > (db_filelength(stream) - db_ftell(stream)) / 12) return -1;

    QueueListNode* loaded = nullptr;
    QueueListNode** next = &loaded;
    int previousTime = 0;
    for (int index = 0; index < count; ++index) {
        auto* node = static_cast<QueueListNode*>(mem_malloc(sizeof(QueueListNode)));
        if (node == nullptr) { queue_free_list(loaded); return -1; }
        node->generation = nextQueueNodeGeneration++;
        node->owner = nullptr;
        node->data = nullptr;
        node->next = nullptr;
        if (db_freadInt(stream, &node->time) == -1
            || db_freadInt(stream, &node->type) == -1
            || db_freadInt(stream, &node->savedOwnerId) == -1
            || node->type < 0 || node->type >= EVENT_TYPE_COUNT
            || node->time < 0 || node->time < previousTime) {
            mem_free(node); queue_free_list(loaded); return -1;
        }
        if (node->savedOwnerId != -2) {
            for (Object* root = obj_find_first(); root != nullptr; root = obj_find_next()) {
                node->owner = inven_find_id(root, node->savedOwnerId);
                if (node->owner != nullptr) break;
            }
        }
        auto* description = &q_func[node->type];
        if (description->readProc != nullptr
            && description->readProc(stream, &node->data) == -1) {
            queue_free_list(node); queue_free_list(loaded); return -1;
        }
        *next = node;
        next = &node->next;
        previousTime = node->time;
    }
    queue_free_list(queue);
    queue = loaded;
    return 0;
}

// Called after the independently saved guest and its inventory are attached.
// Map loading may have resolved a guest native ID to nothing or a new object.
void queue_bind_loaded_owner(int savedActorId, Object* actor)
{
    if (actor == nullptr) return;
    for (auto* node = queue; node != nullptr; node = node->next) {
        if (node->savedOwnerId == -2) continue;
        Object* owner = node->savedOwnerId == savedActorId
            ? actor : inven_find_id(actor, node->savedOwnerId);
        if (owner != nullptr) {
            node->owner = owner;
            node->savedOwnerId = -2;
            owner->flags |= OBJECT_USED;
        }
    }
}

// 0x4907F4
int queue_save(DB_FILE* stream)
{
    QueueListNode* queueListNode;

    int count = 0;

    queueListNode = queue;
    while (queueListNode != NULL) {
        count += 1;
        queueListNode = queueListNode->next;
    }

    if (db_fwriteInt(stream, count) == -1) {
        return -1;
    }

    queueListNode = queue;
    while (queueListNode != NULL) {
        Object* object = queueListNode->owner;
        int objectId = object != NULL ? object->id : queueListNode->savedOwnerId;

        if (db_fwriteInt(stream, queueListNode->time) == -1) {
            return -1;
        }

        if (db_fwriteInt(stream, queueListNode->type) == -1) {
            return -1;
        }

        if (db_fwriteInt(stream, objectId) == -1) {
            return -1;
        }

        EventTypeDescription* eventTypeDescription = &(q_func[queueListNode->type]);
        if (eventTypeDescription->writeProc != NULL) {
            if (eventTypeDescription->writeProc(stream, queueListNode->data) == -1) {
                return -1;
            }
        }

        queueListNode = queueListNode->next;
    }

    return 0;
}

// 0x4908A0
int queue_add(int delay, Object* obj, void* data, int eventType)
{
    QueueListNode* newQueueListNode = (QueueListNode*)mem_malloc(sizeof(QueueListNode));
    if (newQueueListNode == NULL) {
        return -1;
    }
    newQueueListNode->generation = nextQueueNodeGeneration++;

    int v1 = game_time();
    int v2 = v1 + delay;
    newQueueListNode->time = v2;
    newQueueListNode->type = eventType;
    newQueueListNode->owner = obj;
    newQueueListNode->savedOwnerId = -2;
    newQueueListNode->data = data;

    if (obj != NULL) {
        obj->flags |= OBJECT_USED;
    }

    QueueListNode** v3 = &queue;

    if (queue != NULL) {
        QueueListNode* v4;

        do {
            v4 = *v3;
            if (v2 < v4->time) {
                break;
            }
            v3 = &(v4->next);
        } while (v4->next != NULL);
    }

    newQueueListNode->next = *v3;
    *v3 = newQueueListNode;

    return 0;
}

// 0x490908
int queue_remove(Object* owner)
{
    QueueListNode* queueListNode = queue;
    QueueListNode** queueListNodePtr = &queue;

    while (queueListNode) {
        if (queueListNode->owner == owner) {
            QueueListNode* temp = queueListNode;

            queueListNode = queueListNode->next;
            *queueListNodePtr = queueListNode;

            EventTypeDescription* eventTypeDescription = &(q_func[temp->type]);
            if (eventTypeDescription->freeProc != NULL) {
                eventTypeDescription->freeProc(temp->data);
            }

            mem_free(temp);
        } else {
            queueListNodePtr = &(queueListNode->next);
            queueListNode = queueListNode->next;
        }
    }

    return 0;
}

// 0x490960
int queue_remove_this(Object* owner, int eventType)
{
    QueueListNode* queueListNode = queue;
    QueueListNode** queueListNodePtr = &queue;

    while (queueListNode) {
        if (queueListNode->owner == owner && queueListNode->type == eventType) {
            QueueListNode* temp = queueListNode;

            queueListNode = queueListNode->next;
            *queueListNodePtr = queueListNode;

            EventTypeDescription* eventTypeDescription = &(q_func[temp->type]);
            if (eventTypeDescription->freeProc != NULL) {
                eventTypeDescription->freeProc(temp->data);
            }

            mem_free(temp);
        } else {
            queueListNodePtr = &(queueListNode->next);
            queueListNode = queueListNode->next;
        }
    }

    return 0;
}

// Returns true if there is at least one event of given type scheduled.
//
// 0x4909BC
bool queue_find(Object* owner, int eventType)
{
    QueueListNode* queueListEvent = queue;
    while (queueListEvent != NULL) {
        if (owner == queueListEvent->owner && eventType == queueListEvent->type) {
            return true;
        }

        queueListEvent = queueListEvent->next;
    }

    return false;
}

// 0x4909E4
int queue_process()
{
    if (multiplayer::networkRuntimeIsGuestReplica() || multiplayer::networkRuntimeSimulationStopped()) {
        return 0;
    }

    int time = game_time();
    int v1 = 0;

    while (queue != NULL) {
        QueueListNode* queueListNode = queue;
        if (time < queueListNode->time || v1 != 0) {
            break;
        }

        queue = queueListNode->next;

        EventTypeDescription* eventTypeDescription = &(q_func[queueListNode->type]);
        v1 = eventTypeDescription->handlerProc(queueListNode->owner, queueListNode->data);

        if (eventTypeDescription->freeProc != NULL) {
            eventTypeDescription->freeProc(queueListNode->data);
        }

        mem_free(queueListNode);
    }

    return v1;
}

// 0x490A5C
void queue_clear()
{
    QueueListNode* queueListNode = queue;
    while (queueListNode != NULL) {
        QueueListNode* next = queueListNode->next;

        EventTypeDescription* eventTypeDescription = &(q_func[queueListNode->type]);
        if (eventTypeDescription->freeProc != NULL) {
            eventTypeDescription->freeProc(queueListNode->data);
        }

        mem_free(queueListNode);

        queueListNode = next;
    }

    queue = NULL;
}

// 0x490AA4
void queue_clear_type(int eventType, QueueEventHandler* fn)
{
    // Callbacks can destroy an owner, remove other events or reset the queue.
    // Hold identities rather than links into nodes that callbacks may free.
    std::vector<std::uint64_t> pending;
    for (QueueListNode* node = queue; node != nullptr; node = node->next) {
        if (node->type == eventType) pending.push_back(node->generation);
    }
    for (std::uint64_t generation : pending) {
        QueueListNode** link = &queue;
        while (*link != nullptr && (*link)->generation != generation) link = &(*link)->next;
        if (*link == nullptr) continue;
        QueueListNode* node = *link;
        *link = node->next;
        node->next = nullptr;

        if (fn != nullptr && fn(node->owner, node->data) != 1) {
            // Preserve the original order of equal-time events. Events newly
            // scheduled by a callback remain for a subsequent queue pass.
            QueueListNode** insertion = &queue;
            while (*insertion != nullptr && ((*insertion)->time < node->time
                || ((*insertion)->time == node->time && (*insertion)->generation < generation))) {
                insertion = &(*insertion)->next;
            }
            node->next = *insertion;
            *insertion = node;
        } else {
            EventTypeDescription* description = &q_func[node->type];
            if (description->freeProc != nullptr) description->freeProc(node->data);
            mem_free(node);
        }
    }
}

// TODO: Make unsigned.
//
// 0x490B1C
int queue_next_time()
{
    if (queue == NULL) {
        return 0;
    }

    return queue->time;
}

bool queue_capture_state(std::vector<QueueEventState>& state)
{
    state.clear();
    int previousTime = 0;
    for (QueueListNode* node = queue; node != NULL; node = node->next) {
        if (node->time <= 0
            || node->time < previousTime
            || node->type < 0
            || node->type >= EVENT_TYPE_COUNT) {
            state.clear();
            return false;
        }
        QueueEventState event;
        event.time = node->time;
        event.eventType = node->type;
        event.owner = node->owner;
        event.payloadCount = queue_event_payload_count(node->type);
        if (event.payloadCount != 0 && node->data == NULL) {
            state.clear();
            return false;
        }
        switch (node->type) {
        case EVENT_TYPE_PLAYER_EXPLOSION:
        case EVENT_TYPE_PLAYER_EXPLOSION_FAILURE:
            event.payload[0] = static_cast<PlayerExplosionEvent*>(node->data)->playerId;
            if (event.payload[0] <= 0) { state.clear(); return false; }
            break;
        case EVENT_TYPE_DRUG: {
            DrugEffectEvent* data = (DrugEffectEvent*)node->data;
            for (int index = 0; index < 3; index++) {
                event.payload[index] = data->stats[index];
                event.payload[index + 3] = data->modifiers[index];
            }
            break;
        }
        case EVENT_TYPE_WITHDRAWAL: {
            WithdrawalEvent* data = (WithdrawalEvent*)node->data;
            event.payload[0] = data->field_0;
            event.payload[1] = data->pid;
            event.payload[2] = data->perk;
            break;
        }
        case EVENT_TYPE_SCRIPT: {
            ScriptEvent* data = (ScriptEvent*)node->data;
            event.payload[0] = data->sid;
            event.payload[1] = data->fixedParam;
            break;
        }
        case EVENT_TYPE_RADIATION: {
            RadiationEvent* data = (RadiationEvent*)node->data;
            event.payload[0] = data->radiationLevel;
            event.payload[1] = data->isHealing;
            break;
        }
        default:
            if (node->data != NULL) {
                state.clear();
                return false;
            }
            break;
        }
        state.push_back(event);
        previousTime = node->time;
    }
    return true;
}

PreparedQueueEvents::~PreparedQueueEvents()
{
    queue_free_list(static_cast<QueueListNode*>(_events));
}

bool PreparedQueueEvents::prepare(const std::vector<QueueEventState>& state)
{
    QueueListNode* replacement = NULL;
    QueueListNode** next = &replacement;
    int previousTime = 0;
    for (const QueueEventState& event : state) {
        if (event.eventType < 0 || event.eventType >= EVENT_TYPE_COUNT) {
            queue_free_list(replacement);
            return false;
        }
        std::size_t expectedPayloadCount = queue_event_payload_count(event.eventType);
        bool ownerRequired = event.eventType != EVENT_TYPE_SCRIPT
            && event.eventType != EVENT_TYPE_GAME_TIME
            && event.eventType != EVENT_TYPE_MAP_UPDATE_EVENT;
        if (event.time <= 0
            || event.time < previousTime
            || ((event.eventType == EVENT_TYPE_PLAYER_EXPLOSION || event.eventType == EVENT_TYPE_PLAYER_EXPLOSION_FAILURE)
                && event.payload[0] <= 0)
            || expectedPayloadCount != event.payloadCount
            || (ownerRequired && event.owner == NULL)
            || ((event.eventType == EVENT_TYPE_GAME_TIME || event.eventType == EVENT_TYPE_MAP_UPDATE_EVENT)
                && event.owner != NULL)) {
            queue_free_list(replacement);
            return false;
        }
        for (std::size_t index = event.payloadCount; index < event.payload.size(); index++) {
            if (event.payload[index] != 0) {
                queue_free_list(replacement);
                return false;
            }
        }

        void* data = queue_create_event_data(event);
        if (expectedPayloadCount != 0 && data == NULL) {
            queue_free_list(replacement);
            return false;
        }
        QueueListNode* node = (QueueListNode*)mem_malloc(sizeof(*node));
        if (node == NULL) {
            if (data != NULL) {
                mem_free(data);
            }
            queue_free_list(replacement);
            return false;
        }
        node->generation = nextQueueNodeGeneration++;
        node->time = event.time;
        node->type = event.eventType;
        node->owner = event.owner;
        node->savedOwnerId = -2;
        node->data = data;
        node->next = NULL;
        *next = node;
        next = &(node->next);
        previousTime = event.time;
    }

    queue_free_list(static_cast<QueueListNode*>(_events));
    _events = replacement;
    _eventCount = state.size();
    _prepared = true;
    return true;
}

bool PreparedQueueEvents::rebindOwners(const std::vector<Object*>& owners)
{
    if (!_prepared || owners.size() != _eventCount) return false;
    std::size_t index = 0;
    for (auto* node = static_cast<QueueListNode*>(_events); node != nullptr; node = node->next, ++index) {
        bool ownerRequired = node->type != EVENT_TYPE_SCRIPT
            && node->type != EVENT_TYPE_GAME_TIME && node->type != EVENT_TYPE_MAP_UPDATE_EVENT;
        if ((ownerRequired && owners[index] == nullptr)
            || ((node->type == EVENT_TYPE_GAME_TIME || node->type == EVENT_TYPE_MAP_UPDATE_EVENT)
                && owners[index] != nullptr)) return false;
    }
    index = 0;
    for (auto* node = static_cast<QueueListNode*>(_events); node != nullptr; node = node->next, ++index) {
        node->owner = owners[index];
    }
    return true;
}

bool PreparedQueueEvents::commit()
{
    if (!_prepared) return false;
    auto* replacement = static_cast<QueueListNode*>(_events);
    // Only commit changes owner bookkeeping. Allocation and validation have
    // completed, and snapshot owners may have been rebound without allocation.
    for (auto* node = replacement; node != nullptr; node = node->next) {
        if (node->owner != nullptr) node->owner->flags |= OBJECT_USED;
    }
    queue_clear();
    queue = replacement;
    _events = nullptr;
    _eventCount = 0;
    _prepared = false;
    return true;
}

bool queue_replace_state(const std::vector<QueueEventState>& state)
{
    PreparedQueueEvents replacement;
    return replacement.prepare(state) && replacement.commit();
}

// 0x490B30
static int queue_destroy(Object* obj, void* data)
{
    obj_destroy(obj);
    return 1;
}

// 0x490B3C
static int queue_explode(Object* obj, void* data)
{
    return queue_do_explosion(obj, true);
}

// 0x490B44
static int queue_explode_exit(Object* obj, void* data)
{
    return queue_do_explosion(obj, false);
}

// 0x490B48
static int queue_do_explosion(Object* explosive, bool premature, int playerId)
{
    Object* owner;
    int tile;
    int elevation;
    int min_damage;
    int max_damage;

    owner = obj_top_environment(explosive);
    if (owner) {
        tile = owner->tile;
        elevation = owner->elevation;
    } else {
        tile = explosive->tile;
        elevation = explosive->elevation;
    }

    if (explosive->pid == PROTO_ID_DYNAMITE_I || explosive->pid == PROTO_ID_DYNAMITE_II) {
        // Dynamite
        min_damage = 30;
        max_damage = 50;
    } else {
        // Plastic explosive
        min_damage = 40;
        max_damage = 80;
    }

    Object* source = playerId != 0
        ? multiplayer::networkWorldPlayerActor(multiplayer::PlayerId { static_cast<std::uint32_t>(playerId) }) : obj_dude;
    if (action_explode(tile, elevation, min_damage, max_damage, source, premature) == -2) {
        if (playerId != 0) queue_add_player_explosion(50, explosive, playerId, false);
        else queue_add(50, explosive, NULL, EVENT_TYPE_EXPLOSION);
    } else {
        obj_destroy(explosive);
    }

    return 1;
}

int queue_add_player_explosion(int delay, Object* owner, int playerId, bool premature)
{
    if (playerId <= 0 || owner == nullptr || delay < 0) return -1;
    auto* event = static_cast<PlayerExplosionEvent*>(mem_malloc(sizeof(PlayerExplosionEvent)));
    if (event == nullptr) return -1;
    event->playerId = playerId;
    if (queue_add(delay, owner, event, premature ? EVENT_TYPE_PLAYER_EXPLOSION_FAILURE : EVENT_TYPE_PLAYER_EXPLOSION) != 0) {
        mem_free(event);
        return -1;
    }
    return 0;
}

static int queue_player_explosion_load(DB_FILE* stream, void** data)
{
    int id;
    if (db_freadInt32(stream, &id) != 0 || id <= 0) return -1;
    auto* event = static_cast<PlayerExplosionEvent*>(mem_malloc(sizeof(PlayerExplosionEvent)));
    if (event == nullptr) return -1;
    event->playerId = id;
    *data = event;
    return 0;
}

static int queue_player_explosion_save(DB_FILE* stream, void* data)
{
    auto* event = static_cast<PlayerExplosionEvent*>(data);
    return event != nullptr && event->playerId > 0 ? db_fwriteInt32(stream, event->playerId) : -1;
}

static int queue_player_explode(Object* obj, void* data)
{
    return queue_do_explosion(obj, true, static_cast<PlayerExplosionEvent*>(data)->playerId);
}

static int queue_player_explode_exit(Object* obj, void* data)
{
    return queue_do_explosion(obj, false, static_cast<PlayerExplosionEvent*>(data)->playerId);
}

static int queue_player_premature(Object* obj, void* data)
{
    int playerId = static_cast<PlayerExplosionEvent*>(data)->playerId;
    multiplayer::ScopedPlayerFeedback feedback(multiplayer::networkWorldPlayerActor(
        multiplayer::PlayerId { static_cast<std::uint32_t>(playerId) }));
    MessageListItem message;
    message.num = 4000;
    if (message_search(&misc_message_file, &message)) display_print(message.text);
    return queue_do_explosion(obj, true, playerId);
}

// 0x490BCC
static int queue_premature(Object* obj, void* data)
{
    MessageListItem msg;

    // Due to your inept handling, the explosive detonates prematurely.
    msg.num = 4000;
    if (message_search(&misc_message_file, &msg)) {
        display_print(msg.text);
    }

    return queue_do_explosion(obj, true);
}

// 0x490C08
void queue_leaving_map()
{
    int index;

    for (index = 0; index < EVENT_TYPE_COUNT; index++) {
        if (q_func[index].field_10) {
            queue_clear_type(index, q_func[index].field_14);
        }
    }
}

} // namespace fallout
