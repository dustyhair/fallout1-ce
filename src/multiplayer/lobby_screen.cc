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
constexpr int kCommandPanelLeft = 84;
constexpr int kCommandPanelRight = 556;

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
        return sheet->name;
    }
    if (networkRuntimeConnected()) {
        return "< UNASSIGNED >";
    }
    return "< OFFLINE >";
}

std::string slotStatus(PlayerId playerId)
{
    if (sheetForPlayer(playerId) != nullptr) {
        return "STATE: READY";
    }
    if (networkRuntimeConnected()) {
        return "STATE: WAITING";
    }
    return "STATE: NO LINK";
}

std::string terminalStatus()
{
    std::string status = networkRuntimeStatus();
    constexpr const char* prefix = "MULTIPLAYER";
    if (status.rfind(prefix, 0) == 0) {
        status.erase(0, std::char_traits<char>::length(prefix));
    }
    while (!status.empty() && status.front() == ' ') {
        status.erase(0, 1);
    }
    if (!status.empty() && status.front() == ':') {
        status.erase(0, 1);
        if (!status.empty() && status.front() == ' ') {
            status.erase(0, 1);
        }
    }
    return status;
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

void drawLobby(int window,
    unsigned char* background,
    unsigned char* commandPanel,
    int commandPanelHeight,
    const std::string& notice)
{
    unsigned char* buffer = win_get_buf(window);
    buf_to_buf(background, kLobbyWidth, kLobbyHeight, kLobbyWidth, buffer, kLobbyWidth);
    int commandPanelY = kLobbyHeight - commandPanelHeight;
    buf_to_buf(commandPanel + kCommandPanelLeft,
        kCommandPanelRight - kCommandPanelLeft,
        commandPanelHeight,
        kLobbyWidth,
        buffer + kLobbyWidth * commandPanelY + kCommandPanelLeft,
        kLobbyWidth);

    const int green = colorTable[992];
    const int brightGreen = colorTable[992];
    const int dimGreen = colorTable[8804];

    int oldFont = text_curr();
    text_font(104);
    const char* title = "MULTIPLAYER NETWORK";
    int titleX = (kLobbyWidth - text_width(title)) / 2;
    text_to_buf(buffer + kLobbyWidth * 24 + titleX,
        title,
        kLobbyWidth - titleX,
        kLobbyWidth,
        brightGreen);

    draw_line(buffer, kLobbyWidth, 154, 57, 486, 57, dimGreen);
    draw_line(buffer, kLobbyWidth, 320, 68, 320, 142, dimGreen);

    text_font(101);
    std::string host = slotText(kHostPlayerId);
    std::string guest = slotText(kGuestPlayerId);
    std::string hostStatus = slotStatus(kHostPlayerId);
    std::string guestStatus = slotStatus(kGuestPlayerId);
    text_to_buf(buffer + kLobbyWidth * 76 + 166, "NODE 01 // HOST", 145, kLobbyWidth, green);
    text_to_buf(buffer + kLobbyWidth * 100 + 166, host.c_str(), 145, kLobbyWidth, brightGreen);
    text_to_buf(buffer + kLobbyWidth * 126 + 166, hostStatus.c_str(), 145, kLobbyWidth, green);
    text_to_buf(buffer + kLobbyWidth * 76 + 334, "NODE 02 // GUEST", 145, kLobbyWidth, green);
    text_to_buf(buffer + kLobbyWidth * 100 + 334, guest.c_str(), 145, kLobbyWidth, brightGreen);
    text_to_buf(buffer + kLobbyWidth * 126 + 334, guestStatus.c_str(), 145, kLobbyWidth, green);

    draw_line(buffer, kLobbyWidth, 154, 151, 486, 151, dimGreen);
    text_to_buf(buffer + kLobbyWidth * 162 + 166, "NETWORK LINK", 145, kLobbyWidth, brightGreen);
    std::string status = terminalStatus();
    text_to_buf(buffer + kLobbyWidth * 184 + 166, status.c_str(), 320, kLobbyWidth, green);
    if (!notice.empty()) {
        text_to_buf(buffer + kLobbyWidth * 204 + 166, notice.c_str(), 320, kLobbyWidth, brightGreen);
    }

    const char* commandsTitle = "COMMAND CONSOLE";
    int commandsTitleX = (kLobbyWidth - text_width(commandsTitle)) / 2;
    text_to_buf(buffer + kLobbyWidth * 347 + commandsTitleX,
        commandsTitle,
        kLobbyWidth - commandsTitleX,
        kLobbyWidth,
        brightGreen);
    draw_line(buffer, kLobbyWidth, 100, 366, 540, 366, dimGreen);
    text_to_buf(buffer + kLobbyWidth * 382 + 131, "HOST", 70, kLobbyWidth, green);
    text_to_buf(buffer + kLobbyWidth * 382 + 226, "JOIN", 70, kLobbyWidth, green);
    text_to_buf(buffer + kLobbyWidth * 382 + 321, "CHOOSE CHARACTER", 125, kLobbyWidth, green);
    text_to_buf(buffer + kLobbyWidth * 382 + 476,
        "START GAME",
        75,
        kLobbyWidth,
        canStartGame() ? brightGreen : dimGreen);
    text_to_buf(buffer + kLobbyWidth * 428 + 131, "DISCONNECT", 110, kLobbyWidth, green);
    text_to_buf(buffer + kLobbyWidth * 428 + 476, "BACK", 70, kLobbyWidth, green);

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
    CacheEntry* commandPanelKey = nullptr;
    CacheEntry* buttonUpKey = nullptr;
    CacheEntry* buttonDownKey = nullptr;
    unsigned char* background = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 103, 0, 0, 0), 0, 0, &backgroundKey);
    Art* commandPanelFrm = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, 99, 0, 0, 0), &commandPanelKey);
    unsigned char* commandPanel = commandPanelFrm != nullptr ? art_frame_data(commandPanelFrm, 0, 0) : nullptr;
    int commandPanelWidth = commandPanelFrm != nullptr ? art_frame_width(commandPanelFrm, 0, 0) : 0;
    int commandPanelHeight = commandPanelFrm != nullptr ? art_frame_length(commandPanelFrm, 0, 0) : 0;
    unsigned char* buttonUp = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 96, 0, 0, 0), 0, 0, &buttonUpKey);
    unsigned char* buttonDown = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 95, 0, 0, 0), 0, 0, &buttonDownKey);
    if (background == nullptr
        || commandPanel == nullptr
        || commandPanelWidth != kLobbyWidth
        || commandPanelHeight <= 0
        || commandPanelHeight >= kLobbyHeight
        || buttonUp == nullptr
        || buttonDown == nullptr) {
        if (buttonDown != nullptr) {
            art_ptr_unlock(buttonDownKey);
        }
        if (buttonUp != nullptr) {
            art_ptr_unlock(buttonUpKey);
        }
        if (background != nullptr) {
            art_ptr_unlock(backgroundKey);
        }
        if (commandPanelFrm != nullptr) {
            art_ptr_unlock(commandPanelKey);
        }
        win_delete(window);
        return MultiplayerLobbyScreenResult::Back;
    }

    registerActionButton(window, 110, 379, KEY_LOWERCASE_H, buttonUp, buttonDown);
    registerActionButton(window, 205, 379, KEY_LOWERCASE_J, buttonUp, buttonDown);
    registerActionButton(window, 300, 379, KEY_LOWERCASE_C, buttonUp, buttonDown);
    registerActionButton(window, 455, 379, KEY_LOWERCASE_S, buttonUp, buttonDown);
    registerActionButton(window, 110, 425, KEY_LOWERCASE_D, buttonUp, buttonDown);
    registerActionButton(window, 455, 425, KEY_ESCAPE, buttonUp, buttonDown);

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
            drawLobby(window, background, commandPanel, commandPanelHeight, notice);
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
    art_ptr_unlock(commandPanelKey);
    art_ptr_unlock(backgroundKey);
    return result;
}

} // namespace multiplayer
} // namespace fallout
