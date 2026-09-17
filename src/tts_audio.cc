#include "tts_audio.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "audio_engine.h"
#include "game/gsound.h"
#include "int/sound.h"

#if defined(FALLOUT_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#endif

namespace fallout {

static int ttsSoundBuffer = -1;

#if defined(FALLOUT_HAVE_FFMPEG)
static bool appendFrame(SwrContext* resampler, AVCodecContext* codecContext, AVFrame* frame, std::vector<std::uint8_t>& pcm)
{
    int outputSamples = av_rescale_rnd(
        swr_get_delay(resampler, codecContext->sample_rate) + frame->nb_samples,
        codecContext->sample_rate,
        codecContext->sample_rate,
        AV_ROUND_UP);
    if (outputSamples <= 0) {
        return false;
    }

    std::size_t oldSize = pcm.size();
    pcm.resize(oldSize + static_cast<std::size_t>(outputSamples) * sizeof(std::int16_t));
    std::uint8_t* output[] = { pcm.data() + oldSize };

    int converted = swr_convert(
        resampler,
        output,
        outputSamples,
        const_cast<const std::uint8_t**>(frame->extended_data),
        frame->nb_samples);
    if (converted < 0) {
        pcm.resize(oldSize);
        return false;
    }

    pcm.resize(oldSize + static_cast<std::size_t>(converted) * sizeof(std::int16_t));
    return true;
}

static bool decodeAudio(const std::string& path, std::vector<std::uint8_t>& pcm, int& sampleRate)
{
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVStream* stream = nullptr;
    const AVCodec* codec = nullptr;
    AVChannelLayout mono = {};
    int streamIndex = -1;
    bool monoInitialized = false;
    bool success = false;

    if (avformat_open_input(&formatContext, path.c_str(), nullptr, nullptr) < 0
        || avformat_find_stream_info(formatContext, nullptr) < 0) {
        goto cleanup;
    }

    streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        goto cleanup;
    }

    stream = formatContext->streams[streamIndex];
    codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        goto cleanup;
    }

    codecContext = avcodec_alloc_context3(codec);
    if (codecContext == nullptr
        || avcodec_parameters_to_context(codecContext, stream->codecpar) < 0
        || avcodec_open2(codecContext, codec, nullptr) < 0) {
        goto cleanup;
    }

    sampleRate = codecContext->sample_rate;
    av_channel_layout_default(&mono, 1);
    monoInitialized = true;
    if (swr_alloc_set_opts2(
            &resampler,
            &mono,
            AV_SAMPLE_FMT_S16,
            sampleRate,
            &codecContext->ch_layout,
            codecContext->sample_fmt,
            codecContext->sample_rate,
            0,
            nullptr)
            < 0
        || swr_init(resampler) < 0) {
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        goto cleanup;
    }

    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == streamIndex && avcodec_send_packet(codecContext, packet) >= 0) {
            while (avcodec_receive_frame(codecContext, frame) >= 0) {
                if (!appendFrame(resampler, codecContext, frame, pcm)) {
                    goto cleanup;
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    avcodec_send_packet(codecContext, nullptr);
    while (avcodec_receive_frame(codecContext, frame) >= 0) {
        if (!appendFrame(resampler, codecContext, frame, pcm)) {
            goto cleanup;
        }
        av_frame_unref(frame);
    }

    success = !pcm.empty();

cleanup:
    if (monoInitialized) {
        av_channel_layout_uninit(&mono);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resampler);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);
    return success;
}
#endif

bool ttsAudioIsAvailable()
{
#if defined(FALLOUT_HAVE_FFMPEG)
    return true;
#else
    return false;
#endif
}

bool ttsAudioIsPlaying()
{
    if (ttsSoundBuffer == -1) {
        return false;
    }

    unsigned int status = 0;
    return audioEngineSoundBufferGetStatus(ttsSoundBuffer, &status)
        && (status & AUDIO_ENGINE_SOUND_BUFFER_STATUS_PLAYING) != 0;
}

bool ttsAudioPlay(const std::string& path)
{
#if defined(FALLOUT_HAVE_FFMPEG)
    std::vector<std::uint8_t> pcm;
    int sampleRate = 0;
    if (!decodeAudio(path, pcm, sampleRate)) {
        return false;
    }

    ttsAudioStop();
    ttsSoundBuffer = audioEngineCreateSoundBuffer(pcm.size(), 16, 1, sampleRate);
    if (ttsSoundBuffer == -1) {
        return false;
    }
    int volume = gsound_get_master_volume() * gsound_speech_volume_get() / VOLUME_MAX;
    audioEngineSoundBufferSetVolume(ttsSoundBuffer, soundVolumeHMItoDirectSound(volume));

    void* audio1 = nullptr;
    void* audio2 = nullptr;
    unsigned int bytes1 = 0;
    unsigned int bytes2 = 0;
    if (!audioEngineSoundBufferLock(
            ttsSoundBuffer,
            0,
            pcm.size(),
            &audio1,
            &bytes1,
            &audio2,
            &bytes2,
            AUDIO_ENGINE_SOUND_BUFFER_LOCK_ENTIRE_BUFFER)) {
        ttsAudioStop();
        return false;
    }

    std::memcpy(audio1, pcm.data(), bytes1);
    if (audio2 != nullptr) {
        std::memcpy(audio2, pcm.data() + bytes1, bytes2);
    }
    audioEngineSoundBufferUnlock(ttsSoundBuffer, audio1, bytes1, audio2, bytes2);

    if (!audioEngineSoundBufferPlay(ttsSoundBuffer, 0)) {
        ttsAudioStop();
        return false;
    }

    return true;
#else
    return false;
#endif
}

void ttsAudioStop()
{
    if (ttsSoundBuffer != -1) {
        audioEngineSoundBufferStop(ttsSoundBuffer);
        audioEngineSoundBufferRelease(ttsSoundBuffer);
        ttsSoundBuffer = -1;
    }
}

} // namespace fallout
