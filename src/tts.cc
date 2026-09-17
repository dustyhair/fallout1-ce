#include "tts.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iconv.h>
#include <sstream>
#include <string>
#include <vector>

#include "game/config.h"
#include "game/critter.h"
#include "game/gconfig.h"
#include "game/object.h"
#include "plib/gnw/debug.h"
#include "tts_audio.h"

#if defined(FALLOUT_HAVE_SPEECHD)
#include <libspeechd.h>
#endif

namespace fallout {

static bool tts_enabled = true;
static bool tts_speak_options = true;
static bool tts_available = false;
static std::string tts_encoding = "WINDOWS-1252";
static std::string tts_cache_path = "TTS_CACHE";
static std::string tts_last_text;
static std::string tts_last_cache_relative_path;

#if defined(FALLOUT_HAVE_SPEECHD)
static SPDConnection* tts_connection = nullptr;
#endif

static std::string getTextHash(const std::string& text)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }

    std::ostringstream value;
    value << std::hex << std::setfill('0') << std::setw(16) << hash;
    return value.str();
}

static bool playCachedPath(const std::string& relativePath)
{
    if (!ttsAudioIsAvailable() || tts_cache_path.empty() || relativePath.empty()) {
        return false;
    }

    std::string path = tts_cache_path + '/' + relativePath;
    std::FILE* stream = std::fopen(path.c_str(), "rb");
    if (stream == nullptr) {
        return false;
    }
    std::fclose(stream);

    return ttsAudioPlay(path);
}

static bool speakCached(const std::string& text, const std::string& relativePath)
{
    if (playCachedPath(relativePath)) {
        return true;
    }

    if (relativePath.size() > 7) {
        std::string neutralPath = relativePath;
        std::size_t suffix = neutralPath.size() - 7;
        if (neutralPath.compare(suffix, 7, "-m.opus") == 0
            || neutralPath.compare(suffix, 7, "-f.opus") == 0) {
            neutralPath.replace(suffix, 7, "-n.opus");
            if (playCachedPath(neutralPath)) {
                return true;
            }
        }
    }

    return playCachedPath(getTextHash(text) + ".opus");
}

static void beginSpeech(bool interrupt)
{
    if (interrupt) {
        ttsAudioStop();
#if defined(FALLOUT_HAVE_SPEECHD)
        if (tts_connection != nullptr) {
            spd_cancel(tts_connection);
        }
#endif
    }
}

static void speakFallback(const std::string& text)
{
#if defined(FALLOUT_HAVE_SPEECHD)
    if (tts_connection != nullptr) {
        spd_say(tts_connection, SPD_MESSAGE, text.c_str());
    }
#endif
}

static void speakUtf8(const std::string& text, const std::string& relativePath, bool interrupt)
{
    beginSpeech(interrupt);
    if (!speakCached(text, relativePath)) {
        speakFallback(text);
    }
}

static int clampSpeechSetting(int value)
{
    return std::max(-100, std::min(100, value));
}

static std::string convertToUtf8(const char* text)
{
    if (text == nullptr || *text == '\0') {
        return {};
    }

    if (tts_encoding.empty() || tts_encoding == "UTF-8" || tts_encoding == "utf-8") {
        return text;
    }

    iconv_t converter = iconv_open("UTF-8", tts_encoding.c_str());
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        return text;
    }

    size_t inputBytesLeft = strlen(text);
    char* input = const_cast<char*>(text);
    std::vector<char> output(inputBytesLeft * 4 + 16);
    char* outputCursor = output.data();
    size_t outputBytesLeft = output.size() - 1;

    while (inputBytesLeft > 0) {
        size_t result = iconv(converter, &input, &inputBytesLeft, &outputCursor, &outputBytesLeft);
        if (result != static_cast<size_t>(-1)) {
            break;
        }

        if (errno == E2BIG) {
            size_t written = static_cast<size_t>(outputCursor - output.data());
            output.resize(output.size() * 2);
            outputCursor = output.data() + written;
            outputBytesLeft = output.size() - written - 1;
            continue;
        }

        if (errno == EILSEQ || errno == EINVAL) {
            input++;
            inputBytesLeft--;
            if (outputBytesLeft >= 3) {
                *outputCursor++ = static_cast<char>(0xEF);
                *outputCursor++ = static_cast<char>(0xBF);
                *outputCursor++ = static_cast<char>(0xBD);
                outputBytesLeft -= 3;
            }
            continue;
        }

        break;
    }

    iconv_close(converter);
    *outputCursor = '\0';
    return output.data();
}

static bool isNameCharacter(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z')
        || (ch >= '0' && ch <= '9')
        || ch == '_'
        || ch >= 0x80;
}

static std::string replacePlayerName(const std::string& text)
{
    if (obj_dude == nullptr) {
        return text;
    }

    std::string playerName = convertToUtf8(critter_name(obj_dude));
    if (playerName.empty() || playerName == "None" || playerName == "Vault Dweller") {
        return text;
    }

    std::string result = text;
    std::size_t offset = 0;
    while ((offset = result.find(playerName, offset)) != std::string::npos) {
        bool startsAtBoundary = offset == 0
            || !isNameCharacter(static_cast<unsigned char>(result[offset - 1]));
        std::size_t ending = offset + playerName.size();
        bool endsAtBoundary = ending == result.size()
            || !isNameCharacter(static_cast<unsigned char>(result[ending]));
        if (startsAtBoundary && endsAtBoundary) {
            result.replace(offset, playerName.size(), "Vault Dweller");
            offset += strlen("Vault Dweller");
        } else {
            offset = ending;
        }
    }
    return result;
}

static std::string dialogCachePath(const std::string& text, int speakerListId, bool playerVoice, int gender)
{
    char genderCode = 'n';
    if (gender == 0) {
        genderCode = 'm';
    } else if (gender == 1) {
        genderCode = 'f';
    }

    std::ostringstream relativePath;
    relativePath << "dialog/" << (playerVoice ? "player" : "npc") << '/';
    if (!playerVoice) {
        relativePath << speakerListId << '/';
    }
    relativePath << getTextHash(text) << '-' << genderCode << ".opus";
    return relativePath.str();
}

bool ttsInit()
{
    configGetBool(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_ENABLED_KEY, &tts_enabled);
    configGetBool(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_SPEAK_OPTIONS_KEY, &tts_speak_options);

    char* stringValue = nullptr;
    if (config_get_string(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_ENCODING_KEY, &stringValue)) {
        tts_encoding = stringValue;
    }
    if (config_get_string(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_CACHE_PATH_KEY, &stringValue)) {
        tts_cache_path = stringValue;
    }

    bool speechDispatcherAvailable = false;

#if defined(FALLOUT_HAVE_SPEECHD)
    tts_connection = spd_open("fallout-ce-tts", "main", nullptr, SPD_MODE_SINGLE);
    if (tts_connection == nullptr) {
        debug_printf("Text-to-speech: could not connect to Speech Dispatcher.\n");
    } else {
        int value = 0;
        if (config_get_value(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_RATE_KEY, &value)) {
            spd_set_voice_rate(tts_connection, clampSpeechSetting(value));
        }
        if (config_get_value(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_PITCH_KEY, &value)) {
            spd_set_voice_pitch(tts_connection, clampSpeechSetting(value));
        }
        if (config_get_value(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_VOLUME_KEY, &value)) {
            spd_set_volume(tts_connection, clampSpeechSetting(value));
        }

        if (config_get_string(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_LANGUAGE_KEY, &stringValue)
            && stringValue[0] != '\0') {
            spd_set_language(tts_connection, stringValue);
        }
        if (config_get_string(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_OUTPUT_MODULE_KEY, &stringValue)
            && stringValue[0] != '\0') {
            spd_set_output_module(tts_connection, stringValue);
        }
        if (config_get_string(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_VOICE_KEY, &stringValue)
            && stringValue[0] != '\0') {
            spd_set_synthesis_voice(tts_connection, stringValue);
        }

        spd_set_punctuation(tts_connection, SPD_PUNCT_SOME);
        speechDispatcherAvailable = true;
        debug_printf("Text-to-speech: connected to Speech Dispatcher.\n");
    }
#else
    debug_printf("Text-to-speech: this build has no Speech Dispatcher support.\n");
#endif

    tts_available = ttsAudioIsAvailable() || speechDispatcherAvailable;
    if (ttsAudioIsAvailable()) {
        debug_printf("Text-to-speech: cached Opus playback enabled.\n");
    }
    return tts_available;
}

void ttsExit()
{
    ttsAudioStop();
#if defined(FALLOUT_HAVE_SPEECHD)
    if (tts_connection != nullptr) {
        spd_cancel(tts_connection);
        spd_close(tts_connection);
        tts_connection = nullptr;
    }
#endif
    tts_available = false;
    tts_last_text.clear();
    tts_last_cache_relative_path.clear();
}

bool ttsIsAvailable()
{
    return tts_available;
}

bool ttsIsEnabled()
{
    return tts_available && tts_enabled;
}

bool ttsShouldSpeakOptions()
{
    return ttsIsEnabled() && tts_speak_options;
}

void ttsSpeak(const char* text, bool interrupt)
{
    if (!ttsIsEnabled() || text == nullptr || *text == '\0') {
        return;
    }

    std::string utf8 = convertToUtf8(text);
    if (utf8.empty()) {
        return;
    }

    tts_last_text = utf8;
    tts_last_cache_relative_path.clear();

    speakUtf8(utf8, tts_last_cache_relative_path, interrupt);
}

void ttsSpeakDialog(const char* text, int speakerListId, bool playerVoice, int gender, bool interrupt)
{
    if (!ttsIsEnabled() || text == nullptr || *text == '\0') {
        return;
    }

    if (playerVoice && static_cast<unsigned char>(*text) == 0x95) {
        text++;
        while (*text == ' ') {
            text++;
        }
    }

    std::string utf8 = convertToUtf8(text);
    if (utf8.empty()) {
        return;
    }

    std::string relativePath = dialogCachePath(utf8, speakerListId, playerVoice, gender);
    std::string canonicalText = replacePlayerName(utf8);
    std::string canonicalPath = dialogCachePath(canonicalText, speakerListId, playerVoice, gender);

    beginSpeech(interrupt);
    if (speakCached(utf8, relativePath)) {
        tts_last_text = utf8;
        tts_last_cache_relative_path = relativePath;
        return;
    }
    if (canonicalText != utf8 && speakCached(canonicalText, canonicalPath)) {
        tts_last_text = canonicalText;
        tts_last_cache_relative_path = canonicalPath;
        return;
    }

    tts_last_text = utf8;
    tts_last_cache_relative_path = relativePath;
    speakFallback(utf8);
}

void ttsStop()
{
    ttsAudioStop();
#if defined(FALLOUT_HAVE_SPEECHD)
    if (tts_connection != nullptr) {
        spd_cancel(tts_connection);
    }
#endif
}

void ttsRepeat()
{
    if (!ttsIsEnabled() || tts_last_text.empty()) {
        return;
    }

    speakUtf8(tts_last_text, tts_last_cache_relative_path, true);
}

void ttsToggle()
{
    if (!tts_available) {
        return;
    }

    tts_enabled = !tts_enabled;
    configSetBool(&game_config, GAME_CONFIG_TTS_KEY, GAME_CONFIG_TTS_ENABLED_KEY, tts_enabled);

    if (tts_enabled) {
        ttsSpeak("Text to speech enabled.");
    } else {
        ttsStop();
    }
}

} // namespace fallout
