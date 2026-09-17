#ifndef FALLOUT_TTS_AUDIO_H_
#define FALLOUT_TTS_AUDIO_H_

#include <string>

namespace fallout {

bool ttsAudioIsAvailable();
bool ttsAudioPlay(const std::string& path);
void ttsAudioStop();

} // namespace fallout

#endif /* FALLOUT_TTS_AUDIO_H_ */
