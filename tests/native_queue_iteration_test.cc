#include "game/queue.cc"

#include <cstdio>

namespace fallout {
bool GNW95_isActive = false;
char GNW95_title[256];
}

static fallout::Object first;
static fallout::Object second;
static fallout::Object added;
static int calls;
static int mode;

static int clearOwner(fallout::Object* owner, void*)
{
    using namespace fallout;
    ++calls;
    if (mode == 1 && owner == &first) return 0;
    if (mode == 1) queue_remove(&first);
    if (mode == 2) {
        queue_remove(&second);
        if (queue_add(3, &added, nullptr, EVENT_TYPE_POISON) != 0) return 0;
    }
    if (mode == 3) return 0;
    queue_remove(owner);
    return 1;
}

int main()
{
    using namespace fallout;
    queue_init();
    // Removing another event owned by the current object invalidated the
    // native traversal's cached next pointer.
    if (queue_add(1, &first, nullptr, EVENT_TYPE_POISON) != 0
        || queue_add(2, &first, nullptr, EVENT_TYPE_POISON) != 0
        || queue_add(3, &second, nullptr, EVENT_TYPE_POISON) != 0) return 2;
    queue_clear_type(EVENT_TYPE_POISON, clearOwner);
    bool nextRemoved = calls == 2 && queue_next_time() == 0;

    // A later callback can remove a retained prior node, invalidating the
    // traversal's previous link as well.
    mode = 1; calls = 0;
    queue_add(1, &first, nullptr, EVENT_TYPE_POISON);
    queue_add(2, &second, nullptr, EVENT_TYPE_POISON);
    queue_clear_type(EVENT_TYPE_POISON, clearOwner);
    bool previousRemoved = calls == 2 && queue_next_time() == 0;

    // Newly scheduled events must survive this pass even if their allocation
    // reuses an address freed by the callback.
    mode = 2; calls = 0;
    queue_add(1, &first, nullptr, EVENT_TYPE_POISON);
    queue_add(2, &second, nullptr, EVENT_TYPE_POISON);
    queue_clear_type(EVENT_TYPE_POISON, clearOwner);
    std::vector<QueueEventState> state;
    bool rescheduled = calls == 1 && queue_capture_state(state)
        && state.size() == 1 && state.front().owner == &added;
    queue_clear();

    mode = 3; calls = 0;
    queue_add(1, &first, nullptr, EVENT_TYPE_POISON);
    queue_add(1, &second, nullptr, EVENT_TYPE_POISON);
    queue_clear_type(EVENT_TYPE_POISON, clearOwner);
    bool retainedOrder = calls == 2 && queue_capture_state(state)
        && state.size() == 2 && state[0].owner == &first && state[1].owner == &second;
    queue_exit();
    std::printf("NATIVE_QUEUE_ITERATION next=%d previous=%d rescheduled=%d retained_order=%d\n",
        nextRemoved, previousRemoved, rescheduled, retainedOrder);
    return nextRemoved && previousRemoved && rescheduled && retainedOrder ? 0 : 1;
}
