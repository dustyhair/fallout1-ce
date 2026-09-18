#include "multiplayer/entity_registry.h"

#include <limits>

namespace fallout {
namespace multiplayer {

EntityRegistrationResult EntityRegistry::registerObject(Object* object, std::optional<PlayerId> owner)
{
    EntityRegistrationResult result;

    if (object == nullptr) {
        result.error = EntityRegistryError::NullObject;
        return result;
    }

    if (owner.has_value() && !isValid(*owner)) {
        result.error = EntityRegistryError::InvalidPlayerId;
        return result;
    }

    if (_objects.find(object) != _objects.end()) {
        result.error = EntityRegistryError::ObjectAlreadyRegistered;
        return result;
    }

    if (_nextEntityId > std::numeric_limits<std::uint32_t>::max()) {
        result.error = EntityRegistryError::EntityIdsExhausted;
        return result;
    }

    result.entityId.value = static_cast<std::uint32_t>(_nextEntityId);
    _nextEntityId++;

    _entities.emplace(result.entityId, Entry { object, owner });
    _objects.emplace(object, result.entityId);
    return result;
}

EntityRegistryError EntityRegistry::restoreObject(EntityId entityId, Object* object, std::optional<PlayerId> owner)
{
    EntityRegistryError error = validateRegistration(entityId, object, owner);
    if (error != EntityRegistryError::None) {
        return error;
    }

    _entities.emplace(entityId, Entry { object, owner });
    _objects.emplace(object, entityId);

    if (entityId.value >= _nextEntityId) {
        _nextEntityId = static_cast<std::uint64_t>(entityId.value) + 1;
    }

    return EntityRegistryError::None;
}

EntityRegistryError EntityRegistry::rebindObject(EntityId entityId, Object* replacement)
{
    if (replacement == nullptr) {
        return EntityRegistryError::NullObject;
    }

    auto entity = _entities.find(entityId);
    if (entity == _entities.end()) {
        return EntityRegistryError::EntityNotFound;
    }

    auto object = _objects.find(replacement);
    if (object != _objects.end() && object->second != entityId) {
        return EntityRegistryError::ObjectAlreadyRegistered;
    }

    if (entity->second.object == replacement) {
        return EntityRegistryError::None;
    }

    _objects.erase(entity->second.object);
    entity->second.object = replacement;
    _objects[replacement] = entityId;
    return EntityRegistryError::None;
}

EntityRegistryError EntityRegistry::unregisterEntity(EntityId entityId)
{
    auto entity = _entities.find(entityId);
    if (entity == _entities.end()) {
        return EntityRegistryError::EntityNotFound;
    }

    _objects.erase(entity->second.object);
    _entities.erase(entity);
    return EntityRegistryError::None;
}

Object* EntityRegistry::findObject(EntityId entityId) const
{
    auto entity = _entities.find(entityId);
    return entity != _entities.end() ? entity->second.object : nullptr;
}

std::optional<EntityId> EntityRegistry::findEntity(const Object* object) const
{
    auto entity = _objects.find(object);
    if (entity == _objects.end()) {
        return std::nullopt;
    }

    return entity->second;
}

EntityRegistryError EntityRegistry::setOwner(EntityId entityId, PlayerId owner)
{
    if (!isValid(owner)) {
        return EntityRegistryError::InvalidPlayerId;
    }

    auto entity = _entities.find(entityId);
    if (entity == _entities.end()) {
        return EntityRegistryError::EntityNotFound;
    }

    entity->second.owner = owner;
    return EntityRegistryError::None;
}

EntityRegistryError EntityRegistry::clearOwner(EntityId entityId)
{
    auto entity = _entities.find(entityId);
    if (entity == _entities.end()) {
        return EntityRegistryError::EntityNotFound;
    }

    entity->second.owner = std::nullopt;
    return EntityRegistryError::None;
}

std::optional<PlayerId> EntityRegistry::ownerOf(EntityId entityId) const
{
    auto entity = _entities.find(entityId);
    if (entity == _entities.end()) {
        return std::nullopt;
    }

    return entity->second.owner;
}

bool EntityRegistry::isOwnedBy(EntityId entityId, PlayerId playerId) const
{
    std::optional<PlayerId> owner = ownerOf(entityId);
    return owner.has_value() && *owner == playerId;
}

bool EntityRegistry::contains(EntityId entityId) const
{
    return _entities.find(entityId) != _entities.end();
}

std::size_t EntityRegistry::size() const
{
    return _entities.size();
}

void EntityRegistry::removeUnowned()
{
    auto entity = _entities.begin();
    while (entity != _entities.end()) {
        if (entity->second.owner.has_value()) {
            entity++;
            continue;
        }

        _objects.erase(entity->second.object);
        entity = _entities.erase(entity);
    }
}

void EntityRegistry::clear()
{
    _entities.clear();
    _objects.clear();
    _nextEntityId = 1;
}

EntityRegistryError EntityRegistry::validateRegistration(EntityId entityId, Object* object, std::optional<PlayerId> owner) const
{
    if (!isValid(entityId)) {
        return EntityRegistryError::InvalidEntityId;
    }

    if (object == nullptr) {
        return EntityRegistryError::NullObject;
    }

    if (owner.has_value() && !isValid(*owner)) {
        return EntityRegistryError::InvalidPlayerId;
    }

    if (_entities.find(entityId) != _entities.end()) {
        return EntityRegistryError::EntityIdInUse;
    }

    if (_objects.find(object) != _objects.end()) {
        return EntityRegistryError::ObjectAlreadyRegistered;
    }

    return EntityRegistryError::None;
}

} // namespace multiplayer
} // namespace fallout
