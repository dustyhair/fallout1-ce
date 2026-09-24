#ifndef FALLOUT_MULTIPLAYER_DIALOGUE_VOTE_CONTROLLER_H_
#define FALLOUT_MULTIPLAYER_DIALOGUE_VOTE_CONTROLLER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "multiplayer/types.h"

namespace fallout {
namespace multiplayer {

enum class DialogueVotingPolicy : std::uint8_t {
    TalkerDecides = 1,
    MajorityHostTie = 2,
    HostDecides = 3,
    MajorityStatsRandomTie = 4,
};

struct DialogueBallot {
    PlayerId playerId;
    std::optional<std::uint8_t> option;
    bool connected = true;
};

// One host-owned choice round. The eligible roster is frozen when the options
// appear; disconnects count as abstentions rather than silently shrinking the
// denominator or letting a newly joined player change an in-flight decision.
class DialogueVoteController {
public:
    bool begin(std::uint64_t revision, PlayerId talker, PlayerId host,
        const std::vector<PlayerId>& eligible, std::uint8_t optionCount,
        DialogueVotingPolicy policy, std::uint64_t deadlineMilliseconds);
    bool vote(PlayerId playerId, std::uint64_t revision, std::uint8_t option);
    bool setConnected(PlayerId playerId, bool connected);
    bool setTieBreakStats(PlayerId playerId, int charisma, int intelligence);
    std::optional<std::uint8_t> resolve(std::uint64_t nowMilliseconds,
        std::optional<std::uint32_t> randomDraw = std::nullopt);
    void clear();

    bool active() const { return _active; }
    std::uint64_t revision() const { return _revision; }
    std::uint8_t optionCount() const { return _optionCount; }
    PlayerId talker() const { return _talker; }
    DialogueVotingPolicy policy() const { return _policy; }
    const std::vector<DialogueBallot>& ballots() const { return _ballots; }
    std::optional<std::uint8_t> selected() const { return _selected; }
    bool needsRandomTie() const { return _needsRandomTie; }
    std::size_t randomTieOptionCount() const { return _randomTieOptionCount; }

private:
    bool _active = false;
    std::uint64_t _revision = 0;
    std::uint64_t _deadlineMilliseconds = 0;
    std::uint8_t _optionCount = 0;
    PlayerId _talker;
    PlayerId _host;
    DialogueVotingPolicy _policy = DialogueVotingPolicy::TalkerDecides;
    std::vector<DialogueBallot> _ballots;
    std::vector<std::pair<int, int>> _tieBreakStats;
    std::optional<std::uint8_t> _selected;
    bool _needsRandomTie = false;
    std::size_t _randomTieOptionCount = 0;
};

} // namespace multiplayer
} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_DIALOGUE_VOTE_CONTROLLER_H_ */
