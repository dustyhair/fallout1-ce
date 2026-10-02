#ifndef FALLOUT_GAME_ENDGAME_H_
#define FALLOUT_GAME_ENDGAME_H_

#include <vector>

namespace fallout {

std::vector<int> endgame_select_slides();
void endgame_play_slides(const std::vector<int>& slides);
void endgame_play_finale(int movie);
void endgame_slideshow();
void endgame_movie();

} // namespace fallout

#endif /* FALLOUT_GAME_ENDGAME_H_ */
