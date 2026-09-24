#include "multiplayer/dialogue_vote_controller.h"

#include <algorithm>
#include <array>

namespace fallout {
namespace multiplayer {

bool DialogueVoteController::begin(std::uint64_t revision, PlayerId talker,
    PlayerId host, const std::vector<PlayerId>& eligible,
    std::uint8_t optionCount, DialogueVotingPolicy policy,
    std::uint64_t deadlineMilliseconds)
{
    if (revision == 0 || !isValid(talker) || !isValid(host)
        || eligible.empty() || eligible.size() > kMaximumTransitionPlayers
        || optionCount == 0 || optionCount > kMaximumDialogueOptions
        || deadlineMilliseconds == 0
        || (policy != DialogueVotingPolicy::TalkerDecides
            && policy != DialogueVotingPolicy::MajorityHostTie
            && policy != DialogueVotingPolicy::HostDecides
            && policy != DialogueVotingPolicy::MajorityStatsRandomTie)) {
        return false;
    }
    std::vector<PlayerId> ordered = eligible;
    std::sort(ordered.begin(), ordered.end(), [](PlayerId a, PlayerId b) {
        return a.value < b.value;
    });
    if (std::adjacent_find(ordered.begin(), ordered.end()) != ordered.end()
        || std::find(ordered.begin(), ordered.end(), talker) == ordered.end()
        || std::find(ordered.begin(), ordered.end(), host) == ordered.end()) {
        return false;
    }
    clear();
    _active = true;
    _revision = revision;
    _deadlineMilliseconds = deadlineMilliseconds;
    _optionCount = optionCount;
    _talker = talker;
    _host = host;
    _policy = policy;
    for (PlayerId playerId : ordered) {
        _ballots.push_back(DialogueBallot { playerId, std::nullopt, true });
        _tieBreakStats.emplace_back(0, 0);
    }
    return true;
}

bool DialogueVoteController::vote(PlayerId playerId, std::uint64_t revision,
    std::uint8_t option)
{
    if (!_active || _selected.has_value() || revision != _revision
        || option >= _optionCount) return false;
    auto found = std::find_if(_ballots.begin(), _ballots.end(),
        [playerId](const DialogueBallot& ballot) {
            return ballot.playerId == playerId;
        });
    if (found == _ballots.end() || !found->connected) return false;
    found->option = option;
    _needsRandomTie = false;
    _randomTieOptionCount = 0;
    return true;
}

bool DialogueVoteController::setConnected(PlayerId playerId, bool connected)
{
    auto found = std::find_if(_ballots.begin(), _ballots.end(),
        [playerId](const DialogueBallot& ballot) {
            return ballot.playerId == playerId;
        });
    if (!_active || _selected.has_value() || found == _ballots.end()) return false;
    found->connected = connected;
    if (!connected) found->option.reset();
    _needsRandomTie = false;
    _randomTieOptionCount = 0;
    return true;
}

bool DialogueVoteController::setTieBreakStats(PlayerId playerId,
    int charisma, int intelligence)
{
    auto found = std::find_if(_ballots.begin(), _ballots.end(),
        [playerId](const DialogueBallot& ballot) {
            return ballot.playerId == playerId;
        });
    if (!_active || _selected.has_value() || found == _ballots.end()
        || charisma < 0 || intelligence < 0) return false;
    _tieBreakStats[static_cast<std::size_t>(found - _ballots.begin())] =
        { charisma, intelligence };
    return true;
}

std::optional<std::uint8_t> DialogueVoteController::resolve(
    std::uint64_t nowMilliseconds, std::optional<std::uint32_t> randomDraw)
{
    if (!_active || _selected.has_value()) return _selected;
    auto ballotFor = [this](PlayerId id) -> const DialogueBallot* {
        auto found = std::find_if(_ballots.begin(), _ballots.end(),
            [id](const DialogueBallot& ballot) { return ballot.playerId == id; });
        return found != _ballots.end() ? &*found : nullptr;
    };
    bool expired = nowMilliseconds >= _deadlineMilliseconds;
    bool allSettled = std::all_of(_ballots.begin(), _ballots.end(),
        [](const DialogueBallot& ballot) {
            return !ballot.connected || ballot.option.has_value();
        });
    const DialogueBallot* host = ballotFor(_host);
    const DialogueBallot* talker = ballotFor(_talker);
    if (_policy == DialogueVotingPolicy::HostDecides) {
        if (host->option.has_value()) _selected = host->option;
        else if (expired) _selected = talker->option.value_or(0);
        return _selected;
    }
    if (_policy == DialogueVotingPolicy::TalkerDecides) {
        // The talker gets to see every connected player's ballot first.
        if (!allSettled && !expired) return std::nullopt;
        if (talker->option.has_value()) _selected = talker->option;
        else if (host->option.has_value()) _selected = host->option;
        else _selected = 0;
        return _selected;
    }
    std::array<std::size_t, kMaximumDialogueOptions> counts {};
    for (const DialogueBallot& ballot : _ballots) {
        if (ballot.option.has_value()) counts[*ballot.option]++;
    }
    for (std::uint8_t option = 0; option < _optionCount; option++) {
        if (counts[option] > _ballots.size() / 2) {
            _selected = option;
            return _selected;
        }
    }
    if (!allSettled && !expired) return std::nullopt;
    std::size_t highest = *std::max_element(counts.begin(),
        counts.begin() + _optionCount);
    if (_policy == DialogueVotingPolicy::MajorityStatsRandomTie) {
        std::pair<int, int> bestStats { -1, -1 };
        std::vector<std::uint8_t> finalists;
        for (std::uint8_t option = 0; option < _optionCount; option++) {
            if (counts[option] != highest) continue;
            std::pair<int, int> optionStats { -1, -1 };
            for (std::size_t index = 0; index < _ballots.size(); index++) {
                if (_ballots[index].option == option) {
                    optionStats = std::max(optionStats, _tieBreakStats[index]);
                }
            }
            if (optionStats > bestStats) {
                bestStats = optionStats;
                finalists.clear();
            }
            if (optionStats == bestStats) finalists.push_back(option);
        }
        if (finalists.size() == 1) {
            _selected = finalists.front();
            return _selected;
        }
        _needsRandomTie = true;
        _randomTieOptionCount = finalists.size();
        if (randomDraw.has_value()) {
            _selected = finalists[*randomDraw % finalists.size()];
            _needsRandomTie = false;
            _randomTieOptionCount = 0;
        }
        return _selected;
    }
    if (host->option.has_value() && counts[*host->option] == highest) {
        _selected = host->option;
    } else if (talker->option.has_value()
        && counts[*talker->option] == highest) {
        _selected = talker->option;
    } else {
        for (std::uint8_t option = 0; option < _optionCount; option++) {
            if (counts[option] == highest) {
                _selected = option;
                break;
            }
        }
    }
    return _selected;
}

void DialogueVoteController::clear()
{
    _active = false;
    _revision = 0;
    _deadlineMilliseconds = 0;
    _optionCount = 0;
    _talker = {};
    _host = {};
    _ballots.clear();
    _tieBreakStats.clear();
    _selected.reset();
    _needsRandomTie = false;
    _randomTieOptionCount = 0;
}

} // namespace multiplayer
} // namespace fallout
