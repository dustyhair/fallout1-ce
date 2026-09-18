#include "multiplayer/lobby_art.h"

#include <cstdint>
#include <cstring>

#include <SDL.h>

#include "lobby_hardware_asset.h"
#include "plib/color/color.h"

namespace fallout {
namespace multiplayer {
namespace {

constexpr int kLobbyArtWidth = 640;
constexpr int kLobbyArtHeight = 480;

std::uint32_t surfacePixel(const SDL_Surface* surface, int x, int y)
{
    const auto* pixel = static_cast<const std::uint8_t*>(surface->pixels)
        + y * surface->pitch
        + x * surface->format->BytesPerPixel;
    switch (surface->format->BytesPerPixel) {
    case 1:
        return *pixel;
    case 2: {
        std::uint16_t pixel16;
        std::memcpy(&pixel16, pixel, sizeof(pixel16));
        return pixel16;
    }
    case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        return pixel[0] << 16 | pixel[1] << 8 | pixel[2];
#else
        return pixel[0] | pixel[1] << 8 | pixel[2] << 16;
#endif
    case 4: {
        std::uint32_t pixel32;
        std::memcpy(&pixel32, pixel, sizeof(pixel32));
        return pixel32;
    }
    default:
        return 0;
    }
}

} // namespace

bool loadLobbyHardwareArt(std::vector<unsigned char>& pixels)
{
    SDL_RWops* stream = SDL_RWFromConstMem(kLobbyHardwareBmp, static_cast<int>(kLobbyHardwareBmpSize));
    if (stream == nullptr) {
        return false;
    }

    SDL_Surface* surface = SDL_LoadBMP_RW(stream, 1);
    if (surface == nullptr || surface->w != kLobbyArtWidth || surface->h != kLobbyArtHeight) {
        if (surface != nullptr) {
            SDL_FreeSurface(surface);
        }
        return false;
    }

    if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) != 0) {
        SDL_FreeSurface(surface);
        return false;
    }

    pixels.resize(kLobbyArtWidth * kLobbyArtHeight);
    for (int y = 0; y < kLobbyArtHeight; y++) {
        for (int x = 0; x < kLobbyArtWidth; x++) {
            std::uint8_t red;
            std::uint8_t green;
            std::uint8_t blue;
            SDL_GetRGB(surfacePixel(surface, x, y), surface->format, &red, &green, &blue);
            pixels[y * kLobbyArtWidth + x] = colorTable[
                (static_cast<int>(red) >> 3) << 10
                | (static_cast<int>(green) >> 3) << 5
                | (static_cast<int>(blue) >> 3)];
        }
    }

    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }
    SDL_FreeSurface(surface);
    return true;
}

} // namespace multiplayer
} // namespace fallout
