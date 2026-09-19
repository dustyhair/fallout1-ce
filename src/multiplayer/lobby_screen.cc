#include "multiplayer/lobby_screen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <string>
#include <vector>

#include "agent_journal.h"
#include "game/game.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/object.h"
#include "game/options.h"
#include "game/palette.h"
#include "game/select.h"
#include "multiplayer/lobby_art.h"
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
constexpr int kControlPanelTop = 216;
constexpr int kControlPanelBottom = 291;
constexpr int kControlMouseEnterEventBase = 1200;
constexpr int kControlMouseExitEventBase = 1300;
constexpr int kJoinEndpointInputLength = 48;
constexpr int kChatTextLeft = 174;
constexpr int kChatTextRight = 501;
constexpr int kChatInputY = 431;
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
    { LobbyOption::Host, "HOST", 115, 38 },
    { LobbyOption::Join, "JOIN", 161, 38 },
    { LobbyOption::ChooseCharacter, "CHARACTER", 214, 48 },
    { LobbyOption::StartGame, "START", 282, 48 },
    { LobbyOption::Disconnect, "DISCONNECT", 418, 50 },
    { LobbyOption::Back, "BACK", 468, 38 },
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

void drawHardwareState(unsigned char* buffer, int highlightedOption)
{
    for (const ControlDefinition& control : kControls) {
        if (highlightedOption == static_cast<int>(control.option) && controlEnabled(control.option)) {
            int left = control.centerX - control.width / 2;
            lighten_buf(buffer + kLobbyWidth * 226 + left, control.width, 58, kLobbyWidth);
            draw_box(buffer, kLobbyWidth, left, 224, left + control.width, 286, colorTable[32747]);
        }
    }

    bool ready = networkRuntimeLocalSheet() != nullptr;
    drawLamp(buffer, 330, 262, !ready, false);
    drawLamp(buffer, 380, 262, ready, true);

    drawLamp(buffer, 143, 357,
        networkRuntimeMode() == NetworkLaunchMode::Host && networkRuntimeConnected(),
        false);
    drawLamp(buffer, 143, 391,
        networkRuntimeMode() == NetworkLaunchMode::Join && networkRuntimeConnected(),
        true);

    if (networkRuntimeConnected()) {
        int needleX = networkRuntimeLobbyReady() ? 570 : 557;
        int needleY = networkRuntimeLobbyReady() ? 255 : 246;
        draw_line(buffer, kLobbyWidth, 546, 274, needleX, needleY, paletteColor(26, 3, 2));
        draw_line(buffer, kLobbyWidth, 547, 274, needleX + 1, needleY, paletteColor(31, 5, 2));
    }
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
    int green = colorTable[992];
    int dimGreen = colorTable[8804];

    int oldFont = text_curr();
    text_font(101);

    std::vector<std::string> lines = wrappedChatLines(chatMessages);
    std::size_t firstLine = lines.size() > kVisibleChatLines ? lines.size() - kVisibleChatLines : 0;
    int y = 348;
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
    const std::vector<unsigned char>& background,
    const std::string& notice,
    int highlightedOption,
    const std::deque<LobbyChatMessage>& chatMessages)
{
    unsigned char* buffer = win_get_buf(window);
    buf_to_buf(const_cast<unsigned char*>(background.data()),
        kLobbyWidth,
        kLobbyHeight,
        kLobbyWidth,
        buffer,
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

    drawHardwareState(buffer, highlightedOption);
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
    agentJournalWriteChat("outgoing",
        localPlayer.value,
        localPlayer == kHostPlayerId ? "HOST" : "GUEST",
        text.c_str());
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

    bool cursorWasHidden = mouse_hidden();
    if (cursorWasHidden) {
        mouse_show();
    }
    loadColorTable("color.pal");
    palette_fade_to(cmap);

    std::vector<unsigned char> background;
    if (!loadLobbyHardwareArt(background)) {
        if (cursorWasHidden) {
            mouse_hide();
        }
        win_delete(window);
        return MultiplayerLobbyScreenResult::Back;
    }

    for (const ControlDefinition& control : kControls) {
        registerControlHotspot(window, control);
    }

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
            agentJournalWriteChat("incoming",
                message->playerId.value,
                message->playerId == kHostPlayerId ? "HOST" : "GUEST",
                message->text.c_str());
            appendChatMessage(chatMessages, std::move(*message));
        }

        if (networkRuntimeMode() == NetworkLaunchMode::Join && networkRuntimeStartRequested()) {
            result = MultiplayerLobbyScreenResult::StartGame;
            break;
        }

        std::string signature = screenSignature(notice, highlightedOption, chatMessages);
        if (signature != drawnSignature) {
            drawnSignature = signature;
            drawLobby(window, background, notice, highlightedOption, chatMessages);
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
    return result;
}

} // namespace multiplayer
} // namespace fallout
