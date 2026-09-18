#ifndef FALLOUT_MULTIPLAYER_ENTITY_REGISTRY_H_
#define FALLOUT_MULTIPLAYER_ENTITY_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "multiplayer/types.h"

namespace fallout {

struct Object;

namespace multiplayer {

enum class EntityRegistryError {
    None,
    NullObject,
    InvalidEntityId,
    InvalidPlayerId,
    EntityIdInUse,
    ObjectAlreadyRegistered,
    EntityNotFound,
    EntityIdsExhausted,
};

struct EntityRegistrationResult {
    EntityRegistryError error = EntityRegistryError::None;
    EntityId entityId;

    explicit operator bool() const
    {
        return error == EntityRegistryError::None;
    }
};

class EntityRegistry {
public:
    EntityRegistrationResult registerObject(Object* object, std::optional<PlayerId> owner = std::nullopt);
    EntityRegistryError restoreObject(EntityId entityId, Object* object, std::optional<PlayerId> owner = std::nullopt);
    EntityRegistryError rebindObject(EntityId entityId, Object* replacement);
    EntityRegistryError unregisterEntity(EntityId entityId);

    Object* findObject(EntityId entityId) const;
    std::optional<EntityId> findEntity(const Object* object) const;

    EntityRegistryError setOwner(EntityId entityId, PlayerId owner);
    EntityRegistryError clearOwner(EntityId entityId);
    std::optional<PlayerId> ownerOf(EntityId entityId) const;
    bool isOwnedBy(EntityId entityId, PlayerId playerId) const;

    bool contains(EntityId entityId) const;
    std::size_t size() const;
    void removeUnowned();
    void clear();

private:
    struct Entry {
        Object* object;
        std::optional<PlayerId> owner;
    };

    EntityRegistryError validateRegistration(EntityId entityId, Object* object, std::optional<PlayerId> owner) const;

    std::unordered_map<EntityId, Entry, EntityIdHash> _entities;
    std::unordered_map<const Object*, EntityId> _objects;
    std::uint64_t _nextEntityId = 1;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_ENTITY_REGISTRY_H_ */
