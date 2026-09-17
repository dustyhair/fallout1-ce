#ifndef FALLOUT_TTS_H_
#define FALLOUT_TTS_H_

namespace fallout {

bool ttsInit();
void ttsExit();
bool ttsIsAvailable();
bool ttsIsEnabled();
bool ttsIsSpeaking();
bool ttsShouldSpeakOptions();
void ttsSpeak(const char* text, bool interrupt = true);
void ttsSpeakDialog(const char* text, int speakerListId, bool playerVoice, int gender, bool interrupt = true);
void ttsStop();
void ttsRepeat();
void ttsToggle();

} // namespace fallout

#endif /* FALLOUT_TTS_H_ */
