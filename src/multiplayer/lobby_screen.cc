#include "multiplayer/lobby_screen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <string>
#include <vector>

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
constexpr int kControlPanelTop = 222;
constexpr int kControlPanelBottom = 319;
constexpr int kControlMouseEnterEventBase = 1200;
constexpr int kControlMouseExitEventBase = 1300;
constexpr int kJoinEndpointInputLength = 48;
constexpr int kChatLeft = 110;
constexpr int kChatTop = 334;
constexpr int kChatRight = 530;
constexpr int kChatBottom = 461;
constexpr int kChatSidebarRight = 174;
constexpr int kChatTextLeft = 184;
constexpr int kChatTextRight = 520;
constexpr int kChatInputY = 441;
constexpr std::size_t kMaxChatHistory = 16;
constexpr int kVisibleChatLines = 6;

enum class LobbyOption {
    Host,
    Join,
    ChooseCharacter,
    StartGame,
    Disconnect,
    Back,
    Count,
};

struct ControlDefinition {
    LobbyOption option;
    const char* label;
    int centerX;
    int width;
};

enum class ChatEntryResult {
    Cancelled,
    Sent,
    Failed,
};

constexpr std::array<ControlDefinition, static_cast<std::size_t>(LobbyOption::Count)> kControls = { {
    { LobbyOption::Host, "HOST", 116, 48 },
    { LobbyOption::Join, "JOIN", 181, 48 },
    { LobbyOption::ChooseCharacter, "CHARACTER", 250, 70 },
    { LobbyOption::StartGame, "START", 320, 54 },
    { LobbyOption::Disconnect, "DISCONNECT", 456, 72 },
    { LobbyOption::Back, "BACK", 524, 46 },
} };

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

std::string playerChatLabel(PlayerId playerId)
{
    const CharacterCreationSheet* sheet = sheetForPlayer(playerId);
    if (sheet != nullptr && !sheet->name.empty()) {
        return sheet->name;
    }
    return playerId == kHostPlayerId ? "HOST" : "GUEST";
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

bool canStartGame()
{
    return networkRuntimeMode() == NetworkLaunchMode::Host
        && networkRuntimeLocalSheet() != nullptr
        && networkRuntimePeerSheet() != nullptr
        && networkRuntimeLobbyReady();
}

bool controlEnabled(LobbyOption option)
{
    switch (option) {
    case LobbyOption::ChooseCharacter:
        return networkRuntimeConnected() && networkRuntimeLocalSheet() == nullptr;
    case LobbyOption::StartGame:
        return canStartGame();
    case LobbyOption::Disconnect:
        return networkRuntimeMode() != NetworkLaunchMode::Disabled;
    case LobbyOption::Host:
    case LobbyOption::Join:
    case LobbyOption::Back:
        return true;
    case LobbyOption::Count:
        break;
    }
    return false;
}

int paletteColor(int red, int green, int blue)
{
    return colorTable[(red << 10) | (green << 5) | blue];
}

void drawFilledCircle(unsigned char* buffer, int centerX, int centerY, int radius, int color)
{
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                buffer[(centerY + y) * kLobbyWidth + centerX + x] = color;
            }
        }
    }
}

void drawScrew(unsigned char* buffer, int centerX, int centerY)
{
    int shadow = paletteColor(3, 3, 2);
    int steel = paletteColor(15, 14, 10);
    int highlight = paletteColor(23, 21, 15);
    drawFilledCircle(buffer, centerX, centerY, 4, shadow);
    drawFilledCircle(buffer, centerX, centerY, 3, steel);
    draw_line(buffer, kLobbyWidth, centerX - 2, centerY + 1, centerX + 2, centerY - 1, highlight);
}

void drawLamp(unsigned char* buffer, int centerX, int centerY, bool lit, bool green)
{
    int dark = paletteColor(4, 3, 2);
    int rim = paletteColor(18, 15, 8);
    int lamp = green
        ? paletteColor(lit ? 3 : 2, lit ? 31 : 10, lit ? 4 : 2)
        : paletteColor(lit ? 31 : 10, lit ? 6 : 3, 2);
    drawFilledCircle(buffer, centerX, centerY, 5, dark);
    drawFilledCircle(buffer, centerX, centerY, 4, rim);
    drawFilledCircle(buffer, centerX, centerY, 3, lamp);
    if (lit) {
        buffer[(centerY - 1) * kLobbyWidth + centerX - 1] = paletteColor(31, 31, 20);
    }
}

void drawCenteredText(unsigned char* buffer, const char* text, int centerX, int y, int color)
{
    int x = centerX - text_width(text) / 2;
    text_to_buf(buffer + kLobbyWidth * y + x,
        text,
        kLobbyWidth - x,
        kLobbyWidth,
        color);
}

void drawAnalogButton(unsigned char* buffer, const ControlDefinition& control, bool enabled, bool highlighted)
{
    const int dimText = colorTable[8804];
    const int brightText = colorTable[992];
    int labelColor = enabled ? brightText : dimText;
    if (enabled && highlighted) {
        labelColor = colorTable[32747];
    }

    drawCenteredText(buffer, control.label, control.centerX, 237, labelColor);
    drawLamp(buffer, control.centerX, 255, enabled && highlighted, true);

    int outer = paletteColor(3, 3, 2);
    int rim = paletteColor(enabled ? 18 : 9, enabled ? 16 : 8, enabled ? 10 : 5);
    int face = paletteColor(enabled ? 9 : 5, enabled ? 8 : 5, enabled ? 5 : 3);
    if (highlighted && enabled) {
        rim = paletteColor(31, 20, 4);
    }
    drawFilledCircle(buffer, control.centerX, 280, 12, outer);
    drawFilledCircle(buffer, control.centerX, 279, 10, rim);
    drawFilledCircle(buffer, control.centerX, 279, 8, face);
    draw_line(buffer, kLobbyWidth, control.centerX - 4, 274, control.centerX + 3, 272, paletteColor(20, 18, 11));

    char keyLabel[4] = { '[', static_cast<char>('1' + static_cast<int>(control.option)), ']', '\0' };
    drawCenteredText(buffer, keyLabel, control.centerX, 301, enabled ? dimText : paletteColor(5, 7, 4));
}

void drawStartControl(unsigned char* buffer, bool enabled, bool highlighted)
{
    const int dimText = colorTable[8804];
    const int brightText = colorTable[992];
    int labelColor = enabled ? brightText : dimText;
    if (enabled && highlighted) {
        labelColor = colorTable[32747];
    }

    int metalDark = paletteColor(5, 5, 3);
    int metalLight = paletteColor(18, 16, 9);
    int hazard = paletteColor(29, 20, 2);
    drawCenteredText(buffer, "START", 320, 252, labelColor);
    draw_shaded_box(buffer, kLobbyWidth, 298, 264, 342, 312, metalLight, metalDark);
    draw_shaded_box(buffer, kLobbyWidth, 301, 267, 339, 309, metalDark, metalLight);
    for (int x = 304; x < 338; x += 8) {
        draw_line(buffer, kLobbyWidth, x, 269, std::min(x + 6, 337), 269, hazard);
        draw_line(buffer, kLobbyWidth, x + 1, 270, std::min(x + 7, 337), 270, hazard);
    }
    drawFilledCircle(buffer, 320, 289, 13, paletteColor(3, 2, 2));
    drawFilledCircle(buffer, 320, 288, 11, paletteColor(enabled ? 26 : 9, enabled ? 4 : 3, 2));
    drawFilledCircle(buffer, 317, 285, 4, paletteColor(enabled ? 31 : 12, enabled ? 11 : 5, 4));
    if (highlighted && enabled) {
        draw_box(buffer, kLobbyWidth, 296, 262, 344, 314, colorTable[32747]);
    }
}

void drawReadyControl(unsigned char* buffer)
{
    bool ready = networkRuntimeLocalSheet() != nullptr;
    int labelColor = ready ? colorTable[992] : colorTable[8804];
    drawCenteredText(buffer, "READY", 384, 237, labelColor);
    drawLamp(buffer, 370, 258, !ready, false);
    drawLamp(buffer, 399, 258, ready, true);

    int base = paletteColor(4, 4, 3);
    int steel = paletteColor(18, 17, 12);
    drawFilledCircle(buffer, 384, 282, 7, base);
    drawFilledCircle(buffer, 384, 282, 5, steel);
    int leverTopX = ready ? 394 : 374;
    draw_line(buffer, kLobbyWidth, 383, 279, leverTopX, 266, paletteColor(24, 23, 18));
    draw_line(buffer, kLobbyWidth, 384, 280, leverTopX + 1, 266, paletteColor(10, 9, 6));
    drawFilledCircle(buffer, leverTopX, 266, 3, steel);
    drawCenteredText(buffer, ready ? "ON" : "OFF", 384, 302, labelColor);
}

void drawConnectionMeter(unsigned char* buffer)
{
    int frameDark = paletteColor(4, 4, 3);
    int frameLight = paletteColor(17, 15, 9);
    int paper = paletteColor(23, 21, 13);
    int ink = paletteColor(5, 5, 3);
    draw_shaded_box(buffer, kLobbyWidth, 287, 225, 353, 248, frameLight, frameDark);
    buf_fill(buffer + kLobbyWidth * 228 + 290, 60, 17, kLobbyWidth, paper);
    drawCenteredText(buffer, "LINK", 320, 228, ink);
    draw_line(buffer, kLobbyWidth, 296, 242, 344, 242, ink);
    for (int x = 296; x <= 344; x += 8) {
        draw_line(buffer, kLobbyWidth, x, 239, x, 242, ink);
    }
    int level = networkRuntimeConnected() ? (networkRuntimeLobbyReady() ? 2 : 1) : 0;
    int needleX = 298 + level * 22;
    draw_line(buffer, kLobbyWidth, 320, 242, needleX, 234, paletteColor(26, 3, 2));
}

void drawControlPanel(unsigned char* buffer, int highlightedOption)
{
    int dark = paletteColor(4, 4, 3);
    int base = paletteColor(9, 8, 5);
    int light = paletteColor(18, 16, 10);
    int scratch = paletteColor(12, 11, 7);
    buf_fill(buffer + kLobbyWidth * kControlPanelTop + kCommandPanelLeft,
        kCommandPanelRight - kCommandPanelLeft,
        kControlPanelBottom - kControlPanelTop,
        kLobbyWidth,
        base);
    draw_shaded_box(buffer,
        kLobbyWidth,
        kCommandPanelLeft,
        kControlPanelTop,
        kCommandPanelRight,
        kControlPanelBottom,
        light,
        dark);
    draw_shaded_box(buffer,
        kLobbyWidth,
        kCommandPanelLeft + 3,
        kControlPanelTop + 3,
        kCommandPanelRight - 3,
        kControlPanelBottom - 3,
        scratch,
        dark);
    for (int y = kControlPanelTop + 8; y < kControlPanelBottom - 4; y += 9) {
        draw_line(buffer, kLobbyWidth, kCommandPanelLeft + 8, y, kCommandPanelRight - 8, y, paletteColor(8, 7, 4));
    }
    drawScrew(buffer, kCommandPanelLeft + 9, kControlPanelTop + 9);
    drawScrew(buffer, kCommandPanelRight - 9, kControlPanelTop + 9);
    drawScrew(buffer, kCommandPanelLeft + 9, kControlPanelBottom - 9);
    drawScrew(buffer, kCommandPanelRight - 9, kControlPanelBottom - 9);

    int oldFont = text_curr();
    text_font(101);
    drawConnectionMeter(buffer);
    for (const ControlDefinition& control : kControls) {
        if (control.option == LobbyOption::StartGame) {
            continue;
        }
        bool highlighted = highlightedOption == static_cast<int>(control.option);
        drawAnalogButton(buffer, control, controlEnabled(control.option), highlighted);
    }
    drawStartControl(buffer,
        controlEnabled(LobbyOption::StartGame),
        highlightedOption == static_cast<int>(LobbyOption::StartGame));
    drawReadyControl(buffer);
    text_font(oldFont);
}

std::vector<std::string> wrappedChatLines(const std::deque<LobbyChatMessage>& messages)
{
    std::vector<std::string> lines;
    const int maxWidth = kChatTextRight - kChatTextLeft - 4;
    for (const LobbyChatMessage& message : messages) {
        std::string prefix = playerChatLabel(message.playerId) + ": ";
        std::string line = prefix;
        for (char ch : message.text) {
            std::string candidate = line + ch;
            if (text_width(candidate.c_str()) > maxWidth && line.size() > prefix.size()) {
                lines.push_back(line);
                line = "  ";
            }
            line.push_back(ch);
        }
        lines.push_back(line);
    }
    return lines;
}

void drawChatTerminal(unsigned char* buffer, const std::deque<LobbyChatMessage>& chatMessages)
{
    int black = paletteColor(0, 0, 0);
    int screen = paletteColor(1, 3, 1);
    int green = colorTable[992];
    int dimGreen = colorTable[8804];
    int amber = paletteColor(31, 18, 3);

    buf_fill(buffer + kLobbyWidth * kChatTop + kChatLeft,
        kChatRight - kChatLeft,
        kChatBottom - kChatTop,
        kLobbyWidth,
        black);
    draw_box(buffer, kLobbyWidth, kChatLeft, kChatTop, kChatRight, kChatBottom, dimGreen);
    draw_box(buffer, kLobbyWidth, kChatLeft + 2, kChatTop + 2, kChatRight - 2, kChatBottom - 2, screen);
    draw_line(buffer, kLobbyWidth, kChatSidebarRight, kChatTop + 3, kChatSidebarRight, kChatBottom - 3, dimGreen);
    draw_line(buffer, kLobbyWidth, kChatTextLeft - 5, 432, kChatTextRight, 432, dimGreen);

    int oldFont = text_curr();
    text_font(101);
    drawLamp(buffer, 126, 355, networkRuntimeMode() == NetworkLaunchMode::Host && networkRuntimeConnected(), false);
    drawLamp(buffer, 126, 389, networkRuntimeMode() == NetworkLaunchMode::Join && networkRuntimeConnected(), true);
    text_to_buf(buffer + kLobbyWidth * 349 + 138, "HOST", 33, kLobbyWidth, amber);
    text_to_buf(buffer + kLobbyWidth * 383 + 138, "GUEST", 33, kLobbyWidth, amber);
    text_to_buf(buffer + kLobbyWidth * 416 + 120, "CHAT", 46, kLobbyWidth, dimGreen);

    std::vector<std::string> lines = wrappedChatLines(chatMessages);
    std::size_t firstLine = lines.size() > kVisibleChatLines ? lines.size() - kVisibleChatLines : 0;
    int y = 344;
    if (lines.empty()) {
        text_to_buf(buffer + kLobbyWidth * y + kChatTextLeft,
            networkRuntimeConnected() ? "CHANNEL OPEN. NO MESSAGES." : "CHANNEL CLOSED.",
            kChatTextRight - kChatTextLeft,
            kLobbyWidth,
            dimGreen);
    } else {
        for (std::size_t index = firstLine; index < lines.size(); index++) {
            text_to_buf(buffer + kLobbyWidth * y + kChatTextLeft,
                lines[index].c_str(),
                kChatTextRight - kChatTextLeft,
                kLobbyWidth,
                green);
            y += text_height();
        }
    }

    const char* prompt = networkRuntimeConnected()
        ? "> PRESS ENTER TO TRANSMIT..."
        : "> CONNECT TO OPEN CHANNEL";
    text_to_buf(buffer + kLobbyWidth * kChatInputY + kChatTextLeft,
        prompt,
        kChatTextRight - kChatTextLeft,
        kLobbyWidth,
        networkRuntimeConnected() ? green : dimGreen);
    text_font(oldFont);
}

std::string screenSignature(
    const std::string& notice,
    int highlightedOption,
    const std::deque<LobbyChatMessage>& chatMessages)
{
    std::string signature = std::string(networkRuntimeStatus())
        + "\n" + slotText(kHostPlayerId)
        + "\n" + slotText(kGuestPlayerId)
        + "\n" + notice
        + "\n" + std::to_string(highlightedOption);
    for (const LobbyChatMessage& message : chatMessages) {
        signature += "\n" + std::to_string(message.playerId.value) + ":" + message.text;
    }
    return signature;
}

void drawLobby(int window,
    unsigned char* background,
    unsigned char* commandPanel,
    int commandPanelHeight,
    const std::string& notice,
    int highlightedOption,
    const std::deque<LobbyChatMessage>& chatMessages)
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
    text_font(oldFont);

    drawControlPanel(buffer, highlightedOption);
    drawChatTerminal(buffer, chatMessages);
    win_draw(window);
}

int registerControlHotspot(int window, const ControlDefinition& control)
{
    int optionIndex = static_cast<int>(control.option);
    int button = win_register_button(window,
        control.centerX - control.width / 2,
        kControlPanelTop + 8,
        control.width,
        kControlPanelBottom - kControlPanelTop - 12,
        kControlMouseEnterEventBase + optionIndex,
        kControlMouseExitEventBase + optionIndex,
        -1,
        KEY_1 + optionIndex,
        nullptr,
        nullptr,
        nullptr,
        0);
    if (button != -1) {
        win_register_button_sound_func(button, gsound_red_butt_press, gsound_red_butt_release);
    }
    return button;
}

void appendChatMessage(std::deque<LobbyChatMessage>& messages, LobbyChatMessage message)
{
    if (messages.size() == kMaxChatHistory) {
        messages.pop_front();
    }
    messages.push_back(std::move(message));
}

std::string trimmedChatText(const char* text)
{
    std::string value = text != nullptr ? text : "";
    auto isWhitespace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
        return !isWhitespace(static_cast<unsigned char>(ch));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
        return !isWhitespace(static_cast<unsigned char>(ch));
    }).base(), value.end());
    return value;
}

ChatEntryResult enterChatMessage(int window, std::deque<LobbyChatMessage>& messages)
{
    unsigned char* buffer = win_get_buf(window);
    int black = paletteColor(0, 0, 0);
    int green = colorTable[992];
    int oldFont = text_curr();
    text_font(101);
    buf_fill(buffer + kLobbyWidth * kChatInputY + kChatTextLeft,
        kChatTextRight - kChatTextLeft,
        text_height(),
        kLobbyWidth,
        black);
    text_to_buf(buffer + kLobbyWidth * kChatInputY + kChatTextLeft,
        ">",
        10,
        kLobbyWidth,
        green);
    win_draw(window);

    char input[kMaxLobbyChatMessageLength + 2] = {};
    int inputResult = win_input_str(window,
        input,
        static_cast<int>(kMaxLobbyChatMessageLength),
        kChatTextLeft + 12,
        kChatInputY,
        green,
        black);
    text_font(oldFont);
    if (inputResult != 0) {
        return ChatEntryResult::Cancelled;
    }

    std::string text = trimmedChatText(input);
    if (text.empty()) {
        return ChatEntryResult::Cancelled;
    }
    if (!networkRuntimeSendChatMessage(text.c_str())) {
        return ChatEntryResult::Failed;
    }
    PlayerId localPlayer = networkRuntimeMode() == NetworkLaunchMode::Host ? kHostPlayerId : kGuestPlayerId;
    appendChatMessage(messages, LobbyChatMessage { localPlayer, std::move(text) });
    return ChatEntryResult::Sent;
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
    unsigned char* background = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 103, 0, 0, 0), 0, 0, &backgroundKey);
    Art* commandPanelFrm = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, 99, 0, 0, 0), &commandPanelKey);
    unsigned char* commandPanel = commandPanelFrm != nullptr ? art_frame_data(commandPanelFrm, 0, 0) : nullptr;
    int commandPanelWidth = commandPanelFrm != nullptr ? art_frame_width(commandPanelFrm, 0, 0) : 0;
    int commandPanelHeight = commandPanelFrm != nullptr ? art_frame_length(commandPanelFrm, 0, 0) : 0;
    if (background == nullptr
        || commandPanel == nullptr
        || commandPanelWidth != kLobbyWidth
        || commandPanelHeight <= 0
        || commandPanelHeight >= kLobbyHeight) {
        if (background != nullptr) {
            art_ptr_unlock(backgroundKey);
        }
        if (commandPanelFrm != nullptr) {
            art_ptr_unlock(commandPanelKey);
        }
        win_delete(window);
        return MultiplayerLobbyScreenResult::Back;
    }

    for (const ControlDefinition& control : kControls) {
        registerControlHotspot(window, control);
    }

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
    std::deque<LobbyChatMessage> chatMessages;
    int highlightedOption = -1;
    MultiplayerLobbyScreenResult result = MultiplayerLobbyScreenResult::Back;
    bool done = false;
    while (!done && game_user_wants_to_quit == 0) {
        sharedFpsLimiter.mark();

        while (std::optional<LobbyChatMessage> message = networkRuntimeTakeChatMessage()) {
            appendChatMessage(chatMessages, std::move(*message));
        }

        if (networkRuntimeMode() == NetworkLaunchMode::Join && networkRuntimeStartRequested()) {
            result = MultiplayerLobbyScreenResult::StartGame;
            break;
        }

        std::string signature = screenSignature(notice, highlightedOption, chatMessages);
        if (signature != drawnSignature) {
            drawnSignature = signature;
            drawLobby(window, background, commandPanel, commandPanelHeight, notice, highlightedOption, chatMessages);
        }

        int keyCode = get_input();
        if (keyCode >= kControlMouseEnterEventBase
            && keyCode < kControlMouseEnterEventBase + static_cast<int>(LobbyOption::Count)) {
            highlightedOption = keyCode - kControlMouseEnterEventBase;
            keyCode = -1;
        } else if (keyCode >= kControlMouseExitEventBase
            && keyCode < kControlMouseExitEventBase + static_cast<int>(LobbyOption::Count)) {
            if (highlightedOption == keyCode - kControlMouseExitEventBase) {
                highlightedOption = -1;
            }
            keyCode = -1;
        } else if (keyCode == KEY_ARROW_RIGHT || keyCode == KEY_ARROW_DOWN || keyCode == KEY_TAB) {
            highlightedOption = (highlightedOption + 1) % static_cast<int>(LobbyOption::Count);
            keyCode = -1;
        } else if (keyCode == KEY_ARROW_LEFT || keyCode == KEY_ARROW_UP) {
            highlightedOption = highlightedOption <= 0
                ? static_cast<int>(LobbyOption::Count) - 1
                : highlightedOption - 1;
            keyCode = -1;
        } else if (keyCode == KEY_SPACE && highlightedOption != -1) {
            keyCode = KEY_1 + highlightedOption;
        }

        switch (keyCode) {
        case KEY_1:
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
                } else {
                    chatMessages.clear();
                }
            }
            break;
        case KEY_2:
        case KEY_UPPERCASE_J:
        case KEY_LOWERCASE_J:
            notice.clear();
            if (win_get_str(joinEndpoint,
                    kJoinEndpointInputLength,
                    "Enter host address (address:port):",
                    windowX + 80,
                    windowY + 170)
                    == 0
                && joinEndpoint[0] != '\0') {
                if (!networkRuntimeJoin(joinEndpoint)) {
                    notice = "CHECK THE HOST ADDRESS AND TRY AGAIN.";
                } else {
                    chatMessages.clear();
                }
            }
            break;
        case KEY_3:
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
        case KEY_4:
        case KEY_UPPERCASE_S:
        case KEY_LOWERCASE_S:
            if (canStartGame() && networkRuntimeRequestStart()) {
                result = MultiplayerLobbyScreenResult::StartGame;
                done = true;
            } else if (networkRuntimeMode() == NetworkLaunchMode::Join) {
                notice = "WAITING FOR THE HOST TO START THE GAME.";
            } else {
                notice = "BOTH PLAYERS MUST CHOOSE A CHARACTER.";
            }
            break;
        case KEY_5:
        case KEY_UPPERCASE_D:
        case KEY_LOWERCASE_D:
            networkRuntimeDisconnect();
            chatMessages.clear();
            notice.clear();
            break;
        case KEY_6:
        case KEY_UPPERCASE_B:
        case KEY_LOWERCASE_B:
        case KEY_ESCAPE:
            done = true;
            break;
        case KEY_RETURN:
            if (!networkRuntimeConnected()) {
                notice = "CONNECT BEFORE USING CHAT.";
            } else {
                ChatEntryResult chatResult = enterChatMessage(window, chatMessages);
                notice = chatResult == ChatEntryResult::Failed ? "MESSAGE NOT SENT." : "";
            }
            drawnSignature.clear();
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
    art_ptr_unlock(commandPanelKey);
    art_ptr_unlock(backgroundKey);
    return result;
}

} // namespace multiplayer
} // namespace fallout
