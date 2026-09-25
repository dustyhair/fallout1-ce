#include "multiplayer/loot_policy.h"

#include <algorithm>

namespace fallout {
namespace multiplayer {

bool LootPolicy::reset(std::vector<PlayerId> roster)
{
    if (roster.size() < 2 || roster.size() > 8
        || std::any_of(roster.begin(), roster.end(), [](PlayerId id) { return !isValid(id); })) {
        return false;
    }
    std::sort(roster.begin(), roster.end(), [](PlayerId a, PlayerId b) {
        return a.value < b.value;
    });
    if (std::adjacent_find(roster.begin(), roster.end()) != roster.end()) return false;
    _roster = std::move(roster);
    _extraCapCursor = 0;
    _itemCursor = 0;
    return true;
}

bool LootPolicy::restore(std::vector<PlayerId> roster, PlayerId nextExtraCapPlayer,
    PlayerId nextItemPriorityPlayer)
{
    if (!reset(std::move(roster))) return false;
    auto extra = std::find(_roster.begin(), _roster.end(), nextExtraCapPlayer);
    auto item = std::find(_roster.begin(), _roster.end(), nextItemPriorityPlayer);
    if (extra == _roster.end() || item == _roster.end()) return false;
    _extraCapCursor = static_cast<std::size_t>(extra - _roster.begin());
    _itemCursor = static_cast<std::size_t>(item - _roster.begin());
    return true;
}

std::vector<LootCapShare> LootPolicy::splitCaps(std::uint32_t quantity) const
{
    if (_roster.empty()) return {};
    std::vector<LootCapShare> shares;
    shares.reserve(_roster.size());
    std::uint32_t common = quantity / static_cast<std::uint32_t>(_roster.size());
    std::uint32_t remainder = quantity % static_cast<std::uint32_t>(_roster.size());
    for (std::size_t index = 0; index < _roster.size(); index++) {
        std::size_t distance = (index + _roster.size() - _extraCapCursor) % _roster.size();
        shares.push_back({ _roster[index], common + (distance < remainder ? 1U : 0U) });
    }
    return shares;
}

void LootPolicy::advance(std::size_t& cursor)
{
    if (!_roster.empty()) cursor = (cursor + 1) % _roster.size();
}

void LootPolicy::advanceCaps(std::uint32_t quantity)
{
    if (!_roster.empty()) {
        _extraCapCursor = (_extraCapCursor
            + quantity % static_cast<std::uint32_t>(_roster.size())) % _roster.size();
    }
}

PlayerId LootPolicy::itemPriority() const
{
    return nextItemPriorityPlayer();
}

void LootPolicy::advanceItemPriority()
{
    advance(_itemCursor);
}

PlayerId LootPolicy::nextExtraCapPlayer() const
{
    return _roster.empty() ? PlayerId {} : _roster[_extraCapCursor];
}

PlayerId LootPolicy::nextItemPriorityPlayer() const
{
    return _roster.empty() ? PlayerId {} : _roster[_itemCursor];
}

} // namespace multiplayer
} // namespace fallout
