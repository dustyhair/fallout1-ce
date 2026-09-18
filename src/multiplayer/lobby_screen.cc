#include "multiplayer/lobby_screen.h"

#include <string>

#include "game/art.h"
#include "game/game.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/object.h"
#include "game/options.h"
#include "game/palette.h"
#include "game/select.h"
#include "multiplayer/network_runtime.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/intrface.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {
namespace multiplayer {
namespace {

constexpr int kLobbyWidth = 640;
constexpr int kLobbyHeight = 480;

const CharacterCreationSheet* sheetForPlayer(PlayerId playerId)
{
    const CharacterCreationSheet* local = networkRuntimeLocalSheet();
    if (local != nullptr && local->playerId == playerId) {
        return local;
    }
    const CharacterCreationSheet* peer = networkRuntimePeerSheet();
    if (peer != nullptr && peer->playerId == playerId) {
        return peer;
    }
    return nullptr;
}

std::string slotText(PlayerId playerId)
{
    const CharacterCreationSheet* sheet = sheetForPlayer(playerId);
    if (sheet != nullptr) {
        return sheet->name + "  -  READY";
    }
    if (networkRuntimeConnected()) {
        return "WAITING FOR CHARACTER";
    }
    return "NOT CONNECTED";
}

std::string screenSignature(const std::string& notice)
{
    return std::string(networkRuntimeStatus()) + "\n" + slotText(kHostPlayerId) + "\n" + slotText(kGuestPlayerId) + "\n" + notice;
}

bool canStartGame()
{
    return networkRuntimeMode() != NetworkLaunchMode::Disabled
        && networkRuntimeLocalSheet() != nullptr
        && networkRuntimePeerSheet() != nullptr
        && networkRuntimeLobbyReady();
}

void drawLobby(int window, unsigned char* background, const std::string& notice)
{
    unsigned char* buffer = win_get_buf(window);
    buf_to_buf(background, kLobbyWidth, kLobbyHeight, kLobbyWidth, buffer, kLobbyWidth);

    int oldFont = text_curr();
    text_font(104);
    win_print(window, "MULTIPLAYER", 0, 232, 25, colorTable[21091]);

    text_font(103);
    win_print(window, "HOST", 240, 76, 105, colorTable[21091]);
    win_print(window, "GUEST", 240, 365, 105, colorTable[21091]);

    text_font(101);
    std::string host = slotText(kHostPlayerId);
    std::string guest = slotText(kGuestPlayerId);
    win_print(window, host.c_str(), 240, 76, 144, colorTable[21204]);
    win_print(window, guest.c_str(), 240, 365, 144, colorTable[21204]);

    win_print(window, "CONNECTION STATUS", 0, 76, 245, colorTable[21091]);
    win_print(window, networkRuntimeStatus(), 488, 76, 269, colorTable[21204]);
    if (!notice.empty()) {
        win_print(window, notice.c_str(), 488, 76, 298, colorTable[21091]);
    }

    win_print(window, "HOST", 80, 76, 382, colorTable[21204]);
    win_print(window, "JOIN", 80, 186, 382, colorTable[21204]);
    win_print(window, "CHOOSE CHARACTER", 150, 296, 382, colorTable[21204]);
    win_print(window, "START GAME", 110, 473, 382, canStartGame() ? colorTable[21091] : colorTable[21204]);
    win_print(window, "DISCONNECT", 110, 76, 428, colorTable[21204]);
    win_print(window, "BACK", 80, 523, 428, colorTable[21204]);

    text_font(oldFont);
    win_draw(window);
}

int registerActionButton(int window, int x, int y, int key, unsigned char* up, unsigned char* down)
{
    int button = win_register_button(window,
        x,
        y,
        14,
        14,
        -1,
        -1,
        -1,
        key,
        up,
        down,
        nullptr,
        BUTTON_FLAG_TRANSPARENT);
    if (button != -1) {
        win_register_button_sound_func(button, gsound_med_butt_press, gsound_med_butt_release);
    }
    return button;
}

} // namespace

MultiplayerLobbyScreenResult multiplayerLobbyScreen()
{
    const int windowX = (screenGetWidth() - kLobbyWidth) / 2;
    const int windowY = (screenGetHeight() - kLobbyHeight) / 2;
    int window = win_add(windowX,
        windowY,
        kLobbyWidth,
        kLobbyHeight,
        256,
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (window == -1) {
        return MultiplayerLobbyScreenResult::Back;
    }

    CacheEntry* backgroundKey = nullptr;
    CacheEntry* buttonUpKey = nullptr;
    CacheEntry* buttonDownKey = nullptr;
    unsigned char* background = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 103, 0, 0, 0), 0, 0, &backgroundKey);
    unsigned char* buttonUp = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 96, 0, 0, 0), 0, 0, &buttonUpKey);
    unsigned char* buttonDown = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 95, 0, 0, 0), 0, 0, &buttonDownKey);
    if (background == nullptr || buttonUp == nullptr || buttonDown == nullptr) {
        if (buttonDown != nullptr) {
            art_ptr_unlock(buttonDownKey);
        }
        if (buttonUp != nullptr) {
            art_ptr_unlock(buttonUpKey);
        }
        if (background != nullptr) {
            art_ptr_unlock(backgroundKey);
        }
        win_delete(window);
        return MultiplayerLobbyScreenResult::Back;
    }

    registerActionButton(window, 55, 379, KEY_LOWERCASE_H, buttonUp, buttonDown);
    registerActionButton(window, 165, 379, KEY_LOWERCASE_J, buttonUp, buttonDown);
    registerActionButton(window, 275, 379, KEY_LOWERCASE_C, buttonUp, buttonDown);
    registerActionButton(window, 452, 379, KEY_LOWERCASE_S, buttonUp, buttonDown);
    registerActionButton(window, 55, 425, KEY_LOWERCASE_D, buttonUp, buttonDown);
    registerActionButton(window, 502, 425, KEY_ESCAPE, buttonUp, buttonDown);

    bool cursorWasHidden = mouse_hidden();
    if (cursorWasHidden) {
        mouse_show();
    }
    loadColorTable("color.pal");
    palette_fade_to(cmap);

    static char joinEndpoint[128] = "127.0.0.1:42424";
    static char hostPort[7] = "42424";
    std::string notice;
    std::string drawnSignature;
    MultiplayerLobbyScreenResult result = MultiplayerLobbyScreenResult::Back;
    bool done = false;
    while (!done && game_user_wants_to_quit == 0) {
        sharedFpsLimiter.mark();

        std::string signature = screenSignature(notice);
        if (signature != drawnSignature) {
            drawnSignature = signature;
            drawLobby(window, background, notice);
        }

        int keyCode = get_input();
        switch (keyCode) {
        case KEY_UPPERCASE_H:
        case KEY_LOWERCASE_H:
            notice.clear();
            if (win_get_str(hostPort,
                    sizeof(hostPort) - 2,
                    "Enter host port:",
                    windowX + 220,
                    windowY + 170)
                == 0) {
                std::string address;
                std::uint16_t port;
                if (!parseNetworkJoinEndpoint(std::string("127.0.0.1:") + hostPort, address, port)) {
                    notice = "ENTER A PORT BETWEEN 1 AND 65535.";
                } else if (!networkRuntimeHost(port)) {
                    notice = "COULD NOT OPEN THE HOST PORT.";
                }
            }
            break;
        case KEY_UPPERCASE_J:
        case KEY_LOWERCASE_J:
            notice.clear();
            if (win_get_str(joinEndpoint,
                    sizeof(joinEndpoint) - 2,
                    "Enter host address (address:port):",
                    windowX + 150,
                    windowY + 170)
                    == 0
                && joinEndpoint[0] != '\0'
                && !networkRuntimeJoin(joinEndpoint)) {
                notice = "CHECK THE HOST ADDRESS AND TRY AGAIN.";
            }
            break;
        case KEY_UPPERCASE_C:
        case KEY_LOWERCASE_C:
            notice.clear();
            if (!networkRuntimeConnected()) {
                notice = "HOST OR JOIN A GAME FIRST.";
            } else if (networkRuntimeLocalSheet() != nullptr) {
                notice = "YOUR CHARACTER IS ALREADY READY.";
            } else {
                win_hide(window);
                if (select_character() == 2 && !networkRuntimeSubmitLocalCharacter(obj_dude)) {
                    notice = "THE CHARACTER COULD NOT JOIN THIS LOBBY.";
                }
                loadColorTable("color.pal");
                win_show(window);
                palette_fade_to(cmap);
                drawnSignature.clear();
            }
            break;
        case KEY_UPPERCASE_S:
        case KEY_LOWERCASE_S:
            if (canStartGame()) {
                result = MultiplayerLobbyScreenResult::StartGame;
                done = true;
            } else {
                notice = "BOTH PLAYERS MUST CHOOSE A CHARACTER.";
            }
            break;
        case KEY_UPPERCASE_D:
        case KEY_LOWERCASE_D:
            networkRuntimeDisconnect();
            notice.clear();
            break;
        case KEY_UPPERCASE_B:
        case KEY_LOWERCASE_B:
        case KEY_ESCAPE:
            done = true;
            break;
        case KEY_PLUS:
        case KEY_EQUAL:
            IncGamma();
            break;
        case KEY_MINUS:
        case KEY_UNDERSCORE:
            DecGamma();
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (cursorWasHidden) {
        mouse_hide();
    }
    win_delete(window);
    art_ptr_unlock(buttonDownKey);
    art_ptr_unlock(buttonUpKey);
    art_ptr_unlock(backgroundKey);
    return result;
}

} // namespace multiplayer
} // namespace fallout
