// The NHL ports' launcher for Windows and Linux: one window (SDL3 + Dear ImGui, the libraries the
// game itself uses) to choose the game folder and the settings, and Play. Works with a mouse, a
// keyboard or a controller (Steam Deck included).
//
// It is a front end. The settings are the Windows launcher's: sacredfire.ini in the data folder
// (%LOCALAPPDATA%\<product> or ~/.local/share/<product>; BB_DATA_DIR wins), read and written here
// with any key this program does not know kept as it is. Play saves them and starts what already
// runs the game with them: <starter> --play beside this program (NHL19.exe on Windows, nhl19.sh
// on Linux; launcher/imgui/game.h names them). While the game runs, the window waits minimised and
// reports how it ended.
//
// Development: BB_LAUNCHER_SCREENSHOT=<file.bmp> draws a few frames, saves the last and quits
// (BB_LAUNCHER_TAB=0..3 picks the tab), for checking the layout without a display
// (SDL_VIDEO_DRIVER=offscreen); with BB_LAUNCHER_PLAY=1 it presses Play first and saves the frame
// after the game start has ended.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "game.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef BB_BUILD_VERSION
#define BB_BUILD_VERSION "unknown"
#endif

// DejaVu Sans and DejaVu Sans Bold, embedded (gpu/third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
#define BB_RODATA ".section .rdata,\"dr\"\n"
#define BB_HIDDEN(name) ""
#define BB_PREVIOUS ".text\n"
#else
#define BB_RODATA ".section .rodata\n"
#define BB_HIDDEN(name) ".hidden " name "\n"
#define BB_PREVIOUS ".previous\n"
#endif
asm(BB_RODATA ".balign 16\n"
    BB_HIDDEN("bb_launcher_font") ".global bb_launcher_font\nbb_launcher_font:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    BB_HIDDEN("bb_launcher_font_end") ".global bb_launcher_font_end\nbb_launcher_font_end:\n"
    ".balign 16\n"
    BB_HIDDEN("bb_launcher_bold") ".global bb_launcher_bold\nbb_launcher_bold:\n"
    ".incbin \"" BB_BOLD_FONT_PATH "\"\n"
    BB_HIDDEN("bb_launcher_bold_end") ".global bb_launcher_bold_end\nbb_launcher_bold_end:\n"
    BB_PREVIOUS);
extern "C" const unsigned char bb_launcher_font[], bb_launcher_font_end[];
extern "C" const unsigned char bb_launcher_bold[], bb_launcher_bold_end[];

namespace {

#ifdef _WIN32
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif

// ---- colours: the rink ----
ImVec4 Hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}
const ImVec4 kBoard = Hex(0x0D1B2A);   // the background: the boards' navy
const ImVec4 kPanel = Hex(0x132538);   // fields and tabs
const ImVec4 kPanelHi = Hex(0x1B3350);
const ImVec4 kIce = Hex(0xEEF3F7);     // text
const ImVec4 kSteel = Hex(0x8DA2B8);   // secondary text
const ImVec4 kBlueLine = Hex(0x2563B8);// focus and selection
const ImVec4 kGoalRed = Hex(0xC8102E); // Play and the centre line, nothing else
const ImVec4 kOk = Hex(0x7FD1A8), kWarn = Hex(0xF2C14E), kBad = Hex(0xFF7A7A);

std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return a.back() == kSep || a.back() == '/' ? a + b : a + kSep + b;
}
std::string Parent(std::string path) {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const auto cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : path.substr(0, cut == 0 ? 1 : cut);
}
std::string Basename(std::string path) {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const auto cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}
bool IsFile(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}
bool IsDir(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}
std::string Env(const char* name) {
    const char* v = SDL_getenv(name);
    return v ? v : "";
}
std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// ---- where things are ----
struct Places {
    std::string root;     // the package folder (the starter is here)
    std::string data;     // settings, saves, shader cache
    std::string starter;  // NHL19.exe / nhl19.sh
    std::string version;  // the package's VERSION, or the build's
};
Places FindPlaces() {
    Places p;
    const char* base = SDL_GetBasePath();
    p.root = base ? base : "";
    while (!p.root.empty() && (p.root.back() == '/' || p.root.back() == '\\')) p.root.pop_back();
    // On Linux this program sits in the package's bin folder.
    if (Basename(p.root) == "bin") p.root = Parent(p.root);
    if (!Env("BB_DATA_DIR").empty()) {
        p.data = Env("BB_DATA_DIR");
    } else {
#ifdef _WIN32
        p.data = Join(Env("LOCALAPPDATA"), GAME_PRODUCT);
#else
        const std::string xdg = Env("XDG_DATA_HOME");
        p.data = Join(xdg.empty() ? Join(Env("HOME"), ".local/share") : xdg, GAME_PRODUCT);
#endif
    }
    SDL_CreateDirectory(p.data.c_str());
#ifdef _WIN32
    p.starter = Join(p.root, GAME_STARTER_WINDOWS);
#else
    p.starter = Join(p.root, GAME_STARTER_LINUX);
#endif
    p.version = BB_BUILD_VERSION;
    size_t size = 0;
    if (char* text = static_cast<char*>(SDL_LoadFile(Join(p.root, "VERSION").c_str(), &size))) {
        const std::string v = Trim(std::string(text, size));
        if (!v.empty()) p.version = v;
        SDL_free(text);
    }
    return p;
}
std::string LogFolder(const Places& p, const std::string& log_dir) {
    if (!log_dir.empty()) return log_dir;
    if (!Env("BB_LOG_DIR").empty()) return Env("BB_LOG_DIR");
    // Beside the AppImage (nhl19.sh does the same), or beside the launcher.
    if (!Env("APPIMAGE").empty()) return Join(Parent(Env("APPIMAGE")), "logs");
    return Join(p.root, "logs");
}

// ---- sacredfire.ini: key=value lines, the order and unknown keys kept ----
struct Ini {
    std::string path;
    std::vector<std::pair<std::string, std::string>> items;
    void Load() {
        items.clear();
        size_t size = 0;
        char* text = static_cast<char*>(SDL_LoadFile(path.c_str(), &size));
        if (!text) return;
        std::string all(text, size);
        SDL_free(text);
        if (all.compare(0, 3, "\xEF\xBB\xBF") == 0) all.erase(0, 3);  // the Windows launcher writes a BOM
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find('\n', at);
            if (end == std::string::npos) end = all.size();
            const std::string line = Trim(all.substr(at, end - at));
            at = end + 1;
            const auto eq = line.find('=');
            if (eq != std::string::npos) items.emplace_back(line.substr(0, eq), line.substr(eq + 1));
        }
    }
    bool Save() const {
        std::string all;
        for (const auto& [k, v] : items) all += k + "=" + v + "\n";
        return SDL_SaveFile(path.c_str(), all.data(), all.size());
    }
    std::string Get(const std::string& key, const std::string& fallback = "") const {
        for (const auto& [k, v] : items) if (k == key) return v;
        return fallback;
    }
    int GetInt(const std::string& key, int fallback, int count) const {
        const std::string v = Get(key);
        if (v.empty()) return fallback;
        const int i = std::atoi(v.c_str());
        return i >= 0 && i < count ? i : fallback;
    }
    void Set(const std::string& key, const std::string& value) {
        for (auto& [k, v] : items) if (k == key) { v = value; return; }
        items.emplace_back(key, value);
    }
};

// ---- the choices (the Windows launcher's, saved the same way) ----
const char* const kLanguageNames[] = {"English (US)", "French", "German", "Italian", "Spanish (Spain)",
    "Spanish (Latin America)", "Portuguese (Brazil)", "Portuguese (Portugal)", "Polish", "Russian",
    "Japanese", "Chinese (Traditional)"};
const char* const kLanguageIds[] = {"1", "2", "4", "5", "3", "20", "17", "7", "16", "8", "0", "10"};
const char* const kResolutionNames[] = {"1280 x 720", "1920 x 1080"};
const char* const kDisplayNames[] = {"Same as the game", "1920 x 1080 (Full HD)", "2560 x 1440 (2K)", "3840 x 2160 (4K)"};
const char* const kFpsNames[] = {"60 FPS", "90 FPS", "120 FPS", "144 FPS", "240 FPS", "No limit"};
const char* const kFpsLimits[] = {"60", "90", "120", "144", "240", "0"};
const char* const kPresentNames[] = {"Mailbox", "VSync (FIFO)", "Immediate (may tear)"};
const char* const kUpscalerNames[] = {"Off (bilinear)", "AMD FSR 1.0"};
const char* const kSharpnessNames[] = {"Off", "Low", "Medium", "High"};
template <size_t N> constexpr int Count(const char* const (&)[N]) { return int(N); }
int IndexOf(const char* const* list, int count, const std::string& value, int fallback) {
    for (int i = 0; i < count; ++i) if (value == list[i]) return i;
    return fallback;
}

struct Settings {
    std::string game, gamepad, log_dir;
    char player[17] = "";
    int language = 0, resolution = 1, display = 0, fps = 5, upscaler = 0, sharpness = 2;
#ifdef _WIN32
    int present = 2;  // Immediate: the Windows launcher's default
#else
    int present = 1;  // VSync
#endif
    bool fullscreen = false, neo = false, clear_cache = false, frame_stats = false, validation = false;

    void From(const Ini& ini) {
        game = ini.Get("game");
        gamepad = ini.Get("gamepad");
        log_dir = ini.Get("log_dir");
        SDL_strlcpy(player, ini.Get("player").c_str(), sizeof player);
        language = IndexOf(kLanguageIds, Count(kLanguageIds), ini.Get("language"), 0);
        resolution = ini.GetInt("resolution", 1, Count(kResolutionNames));
        display = ini.GetInt("display", 0, Count(kDisplayNames));
        fps = IndexOf(kFpsLimits, Count(kFpsLimits), ini.Get("fps_limit"), 5);
        present = ini.GetInt("present_mode", present, Count(kPresentNames));
        upscaler = ini.GetInt("upscaler", 0, Count(kUpscalerNames));
        sharpness = ini.GetInt("sharpness", 2, Count(kSharpnessNames));
        fullscreen = ini.Get("fullscreen") == "1";
        neo = ini.Get("neo") == "1";
        clear_cache = ini.Get("clear_cache") == "1";
        frame_stats = ini.Get("frame_stats") == "1";
        validation = ini.Get("validation") == "1";
    }
    void To(Ini& ini) const {
        ini.Set("game", game);
        ini.Set("gamepad", gamepad);
        ini.Set("player", player);
        ini.Set("language", kLanguageIds[language]);
        ini.Set("resolution", std::to_string(resolution));
        ini.Set("display", std::to_string(display));
        ini.Set("fps_limit", kFpsLimits[fps]);
        ini.Set("present_mode", std::to_string(present));
        ini.Set("upscaler", std::to_string(upscaler));
        ini.Set("sharpness", std::to_string(sharpness));
        ini.Set("fullscreen", fullscreen ? "1" : "0");
        ini.Set("neo", neo ? "1" : "0");
        ini.Set("clear_cache", clear_cache ? "1" : "0");
        ini.Set("frame_stats", frame_stats ? "1" : "0");
        ini.Set("validation", validation ? "1" : "0");
        ini.Set("log_dir", log_dir);
    }
};

// ---- the game folder: what sce_sys/param.sfo says ----
struct Sfo { std::string title, version; };
bool ReadSfo(const std::string& folder, Sfo& out) {
    size_t size = 0;
    auto* d = static_cast<unsigned char*>(SDL_LoadFile(Join(Join(folder, "sce_sys"), "param.sfo").c_str(), &size));
    if (!d) return false;
    auto u16 = [&](size_t o) { return o + 2 <= size ? unsigned(d[o] | d[o + 1] << 8) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= size ? unsigned(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | unsigned(d[o + 3]) << 24) : 0u; };
    bool ok = size >= 20 && !std::memcmp(d, "\0PSF", 4);
    if (ok) {
        const unsigned keys = u32(8), values = u32(12), count = u32(16);
        for (unsigned i = 0; i < count && 20 + i * 16 + 16 <= size; ++i) {
            const size_t e = 20 + size_t(i) * 16;
            const size_t key = keys + u16(e), value = values + size_t(u32(e + 12)), length = u32(e + 4);
            if (key >= size || value + length > size) continue;
            const std::string name(reinterpret_cast<const char*>(d + key), strnlen(reinterpret_cast<const char*>(d + key), size - key));
            std::string text(reinterpret_cast<const char*>(d + value), length);
            text = text.substr(0, text.find('\0'));
            if (name == "TITLE_ID") out.title = text;
            else if (name == "APP_VER") out.version = text;
        }
    }
    SDL_free(d);
    return ok;
}
enum class FolderState { None, Ready, MergeOnPlay, NoUpdate, OtherGame, NotAGame };
struct FolderCheck {
    FolderState state = FolderState::None;
    std::string message;
};
FolderCheck CheckFolder(const std::string& folder) {
    FolderCheck c;
    if (folder.empty()) { c.message = "Choose the " GAME_TITLE_ID " folder of your PS4 dump."; return c; }
    Sfo sfo;
    if (!IsFile(Join(folder, "eboot.bin")) || !ReadSfo(folder, sfo)) {
        c.state = FolderState::NotAGame;
        c.message = "This isn't a PS4 game folder: choose the one with eboot.bin and sce_sys in it.";
        return c;
    }
    if (sfo.title != GAME_TITLE_ID) {
        c.state = FolderState::OtherGame;
        c.message = "This is " + sfo.title + ", not " GAME_NAME " " GAME_TITLE_ID " (" GAME_REGION ").";
        return c;
    }
    if (sfo.version >= GAME_UPDATE) {
        c.state = FolderState::Ready;
        c.message = GAME_NAME " " + sfo.version + " found. Ready to play.";
        return c;
    }
    // The base game: the update dumped beside it is merged when the game starts.
    for (const char* suffix : {"-patch", "-PATCH", "-update", "_patch"}) {
        Sfo patch;
        const std::string candidate = folder + suffix;
        if (IsDir(candidate) && ReadSfo(candidate, patch) && patch.version >= GAME_UPDATE) {
            c.state = FolderState::MergeOnPlay;
            c.message = "Base game found, with update " + patch.version + " beside it: they're merged when you press Play.";
            return c;
        }
    }
    c.state = FolderState::NoUpdate;
    c.message = "This is the base game (" + sfo.version + "). Put the " GAME_UPDATE " update next to it as " +
                Basename(folder) + "-patch.";
    return c;
}

// ---- the folder dialog (its answer can come from another thread) ----
std::mutex g_dialog_lock;
std::string g_dialog_answer;
bool g_dialog_answered = false;
void SDLCALL FolderChosen(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> lock(g_dialog_lock);
    if (files && files[0]) { g_dialog_answer = files[0]; g_dialog_answered = true; }
}

// ---- the game run ----
struct Run {
    SDL_Process* process = nullptr;
    Uint64 started = 0;
    std::string status;   // the last run's outcome, shown under the folder
    ImVec4 colour = kSteel;
};

struct Gamepad { std::string guid, name; };
std::vector<Gamepad> ListGamepads() {
    std::vector<Gamepad> list;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            char guid[64];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof guid);
            const char* name = SDL_GetGamepadNameForID(ids[i]);
            list.push_back({guid, name ? name : "Controller"});
        }
        SDL_free(ids);
    }
    return list;
}

int PlayerNameFilter(ImGuiInputTextCallbackData* data) {
    return data->EventChar >= 32 && data->EventChar < 127 ? 0 : 1;  // the game shows ASCII names
}

class Launcher {
public:
    explicit Launcher(SDL_Window* window) : window_(window) {
        places_ = FindPlaces();
        ini_.path = Join(places_.data, "sacredfire.ini");
        ini_.Load();
        settings_.From(ini_);
        if (!Env("BB_GAME_DIR").empty()) settings_.game = Env("BB_GAME_DIR");
        SDL_strlcpy(folder_edit_, settings_.game.c_str(), sizeof folder_edit_);
        check_ = CheckFolder(settings_.game);
        const std::string tab = Env("BB_LAUNCHER_TAB");
        if (!tab.empty()) forced_tab_ = std::atoi(tab.c_str());
    }

    bool Running() const { return run_.process != nullptr; }

    void Frame() {
        PollDialog();
        PollRun();
        if (++frames_ % 120 == 1) gamepads_ = ListGamepads();
        if (!Running() && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) Play();
        if (frames_ == 2 && Env("BB_LAUNCHER_PLAY") == "1") Play();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        Header();
        FolderRow();
        ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
        Tabs();
        Footer();
        ImGui::End();
    }

    void Save() {
        settings_.To(ini_);
        if (!ini_.Save()) Report("Settings couldn't be saved to " + ini_.path + ": " + SDL_GetError(), kBad);
    }

    ImFont* bold = nullptr;

private:
    void Report(const std::string& text, ImVec4 colour) { run_.status = text; run_.colour = colour; }

    void Header() {
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::PushFont(bold, 46.0f);
        ImGui::TextUnformatted(GAME_NAME);
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, kSteel);
        ImGui::Text("Native PC port for your PS4 dump, %s", places_.version.c_str());
        ImGui::PopStyleColor();
        const float header_h = ImGui::GetCursorScreenPos().y - top.y;

        // Play: the one red control.
        const ImVec2 play_size(std::max(200.0f, w * 0.2f), header_h - 8.0f);
        ImGui::SetCursorScreenPos(ImVec2(top.x + w - play_size.x, top.y + 4.0f));
        const bool can_play = !Running() && (check_.state == FolderState::Ready || check_.state == FolderState::MergeOnPlay);
        ImGui::PushStyleColor(ImGuiCol_Button, can_play ? kGoalRed : kPanelHi);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, can_play ? Hex(0xE0223F) : kPanelHi);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, can_play ? Hex(0xA00D25) : kPanelHi);
        ImGui::PushStyleColor(ImGuiCol_Text, can_play ? kIce : kSteel);
        ImGui::PushFont(bold, 24.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::BeginDisabled(!can_play);
        if (ImGui::Button(Running() ? "Playing" : "Play", play_size)) Play();
        if (frames_ == 1 && can_play) ImGui::SetItemDefaultFocus();
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::PopFont();
        ImGui::PopStyleColor(4);

        // The centre red line with its faceoff circle.
        const float line_y = top.y + header_h + 18.0f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 red = ImGui::GetColorU32(kGoalRed);
        const float left = ImGui::GetWindowPos().x, right = left + ImGui::GetWindowSize().x;
        const float centre = (left + right) * 0.5f;
        draw->AddRectFilled(ImVec2(left, line_y - 2.0f), ImVec2(right, line_y + 2.0f), red);
        draw->AddCircleFilled(ImVec2(centre, line_y), 15.0f, ImGui::GetColorU32(kBoard), 48);
        draw->AddCircle(ImVec2(centre, line_y), 15.0f, red, 48, 3.0f);
        draw->AddCircleFilled(ImVec2(centre, line_y), 4.0f, red, 16);
        ImGui::SetCursorScreenPos(ImVec2(top.x, line_y + 22.0f));
    }

    void FolderRow() {
        ImGui::PushStyleColor(ImGuiCol_Text, kSteel);
        ImGui::TextUnformatted("Game folder");
        ImGui::PopStyleColor();
        const float browse_w = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputTextWithHint("##folder", "The " GAME_TITLE_ID " folder of your dump", folder_edit_, sizeof folder_edit_)) {
            SetGame(folder_edit_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const std::string start = settings_.game.empty() ? Env("HOME") : Parent(settings_.game);
            SDL_ShowOpenFolderDialog(FolderChosen, nullptr, window_, start.empty() ? nullptr : start.c_str(), false);
        }
        const bool good = check_.state == FolderState::Ready || check_.state == FolderState::MergeOnPlay;
        const ImVec4 colour = check_.state == FolderState::None ? kSteel : good ? kOk :
                              check_.state == FolderState::NoUpdate ? kWarn : kBad;
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::TextWrapped("%s %s", check_.state == FolderState::None ? "" : good ? "\xE2\x9C\x93" : "\xE2\x9C\x95",
                           check_.message.c_str());
        ImGui::PopStyleColor();
        if (!run_.status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, run_.colour);
            ImGui::TextWrapped("%s", run_.status.c_str());
            ImGui::PopStyleColor();
            if (run_.colour.x == kBad.x && run_.colour.y == kBad.y &&
                ImGui::Button("Open the logs folder")) OpenFolder(LogFolder(places_, settings_.log_dir));
        }
    }

    // A row: the label on the left, the control filling the rest.
    bool Row(const char* label) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        return true;
    }
    void Note(const char* text) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, kSteel);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }
    bool BeginRows(const char* id) {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) return false;
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        return true;
    }
    bool Combo(const char* id, int* value, const char* const* names, int count) {
        bool changed = false;
        if (ImGui::BeginCombo(id, names[*value])) {
            for (int i = 0; i < count; ++i) {
                if (ImGui::Selectable(names[i], *value == i)) { *value = i; changed = true; }
                if (*value == i) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    void Tabs() {
        const float footer_h = ImGui::GetFrameHeightWithSpacing() + 8.0f;
        if (!ImGui::BeginTabBar("##tabs")) return;
        auto tab = [&](const char* name, int index) {
            return ImGui::BeginTabItem(name, nullptr, forced_tab_ == index ? ImGuiTabItemFlags_SetSelected : 0);
        };
        bool changed = false;
        if (tab("Game", 0)) {
            ImGui::BeginChild("##game", ImVec2(0, -footer_h));
            if (BeginRows("##gamerows")) {
                Row("Language");
                changed |= Combo("##language", &settings_.language, kLanguageNames, Count(kLanguageNames));
                Note("The game's text and commentary.");
                Row("Player name");
                changed |= ImGui::InputTextWithHint("##player", "Player 1", settings_.player, sizeof settings_.player,
                                                    ImGuiInputTextFlags_CallbackCharFilter, PlayerNameFilter);
                Note("The name the game shows for you: up to 16 letters, numbers or symbols, no accents.");
                Row("PS4 Pro mode");
                changed |= ImGui::Checkbox("##neo", &settings_.neo);
                Note("Runs the game as on a PS4 Pro.");
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab("Video", 1)) {
            ImGui::BeginChild("##video", ImVec2(0, -footer_h));
            if (BeginRows("##videorows")) {
                Row("Game resolution");
                changed |= Combo("##resolution", &settings_.resolution, kResolutionNames, Count(kResolutionNames));
                Row("Window size");
                changed |= Combo("##display", &settings_.display, kDisplayNames, Count(kDisplayNames));
                Note("Larger than the game's resolution: the picture is scaled up to it.");
                Row("Fullscreen");
                changed |= ImGui::Checkbox("##fullscreen", &settings_.fullscreen);
                Row("Frame rate limit");
                changed |= Combo("##fps", &settings_.fps, kFpsNames, Count(kFpsNames));
                Row("Presentation");
                changed |= Combo("##present", &settings_.present, kPresentNames, Count(kPresentNames));
                Row("Upscaling");
                changed |= Combo("##upscaler", &settings_.upscaler, kUpscalerNames, Count(kUpscalerNames));
                Row("Sharpening");
                changed |= Combo("##sharpness", &settings_.sharpness, kSharpnessNames, Count(kSharpnessNames));
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab("Controller", 2)) {
            ImGui::BeginChild("##controller", ImVec2(0, -footer_h));
            if (BeginRows("##padrows")) {
                Row("Player 1");
                std::string current = "Automatic (the first one connected)";
                bool saved_connected = settings_.gamepad.empty();
                for (const auto& g : gamepads_) if (g.guid == settings_.gamepad) { current = g.name; saved_connected = true; }
                if (!saved_connected) current = "A controller that isn't connected now";
                if (ImGui::BeginCombo("##gamepad", current.c_str())) {
                    if (ImGui::Selectable("Automatic (the first one connected)", settings_.gamepad.empty())) { settings_.gamepad.clear(); changed = true; }
                    for (size_t i = 0; i < gamepads_.size(); ++i) {
                        ImGui::PushID(int(i));
                        if (ImGui::Selectable(gamepads_[i].name.c_str(), gamepads_[i].guid == settings_.gamepad)) {
                            settings_.gamepad = gamepads_[i].guid; changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                Note(gamepads_.empty() ? "No controller is connected. The keyboard works too."
                                       : "Other controllers become players 2 to 4 in the order they're connected.");
                ImGui::EndTable();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, kSteel);
            ImGui::TextWrapped("In the game: hold \xE2\x97\x8B (B) for 2 seconds to quit. L3 + R3, or Insert, opens the port's settings.");
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab("Advanced", 3)) {
            ImGui::BeginChild("##advanced", ImVec2(0, -footer_h));
            if (BeginRows("##advrows")) {
                Row("Clear the shader cache");
                changed |= ImGui::Checkbox("##clear", &settings_.clear_cache);
                Note("At every start, until unticked. The first match after it loads slowly while shaders are built.");
                Row("Frame statistics");
                changed |= ImGui::Checkbox("##stats", &settings_.frame_stats);
                Note("Frame timing in the log, for reports about stutter.");
                Row("Vulkan validation");
                changed |= ImGui::Checkbox("##validation", &settings_.validation);
                Note("Checks every graphics call. Slow: for reports about graphics bugs only.");
                Row("Log folder");
                static char log_dir[1024];
                if (ImGui::IsWindowAppearing()) SDL_strlcpy(log_dir, settings_.log_dir.c_str(), sizeof log_dir);
                if (ImGui::InputTextWithHint("##logdir", LogFolder(places_, "").c_str(), log_dir, sizeof log_dir)) {
                    settings_.log_dir = log_dir; changed = true;
                }
                Row("");
                if (ImGui::Button("Open the logs folder")) OpenFolder(LogFolder(places_, settings_.log_dir));
                ImGui::SameLine();
                if (ImGui::Button("Open the data folder")) OpenFolder(places_.data);
                Note("Saves, settings and the shader cache are in the data folder.");
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        if (changed) Save();
        forced_tab_ = -1;
    }

    void Footer() {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kSteel);
        if (!gamepads_.empty())
            ImGui::TextUnformatted("\xE2\x9C\x95 / A  choose     \xE2\x97\x8B / B  back     L1 / R1  tabs     Options / Start  play");
        else
            ImGui::TextUnformatted("Settings are saved as you change them.");
        ImGui::PopStyleColor();
    }

    void SetGame(const std::string& folder) {
        settings_.game = Trim(folder);
        SDL_strlcpy(folder_edit_, settings_.game.c_str(), sizeof folder_edit_);
        check_ = CheckFolder(settings_.game);
        Save();
    }
    void PollDialog() {
        std::lock_guard<std::mutex> lock(g_dialog_lock);
        if (g_dialog_answered) { g_dialog_answered = false; SetGame(g_dialog_answer); }
    }
    void OpenFolder(const std::string& folder) {
        SDL_CreateDirectory(folder.c_str());
        std::string url = "file://";
#ifdef _WIN32
        url += "/";
#endif
        for (char ch : folder) url += ch == '\\' ? '/' : ch;
        if (!SDL_OpenURL(url.c_str())) Report("Couldn't open " + folder + ": " + SDL_GetError(), kBad);
    }

    void Play() {
        check_ = CheckFolder(settings_.game);
        if (check_.state != FolderState::Ready && check_.state != FolderState::MergeOnPlay) return;
        if (!IsFile(places_.starter)) {
            Report(Basename(places_.starter) + " is missing beside the launcher: unpack the whole release again.", kBad);
            return;
        }
        Save();
        const char* args[] = {places_.starter.c_str(), "--play", nullptr};
        run_.process = SDL_CreateProcess(args, false);
        if (!run_.process) {
            Report(std::string("The game couldn't be started: ") + SDL_GetError(), kBad);
            return;
        }
        run_.started = SDL_GetTicks();
        Report("Starting " GAME_NAME ". The first start prepares the game, which takes a minute or so.", kSteel);
        if (Env("BB_LAUNCHER_SCREENSHOT").empty()) SDL_MinimizeWindow(window_);
    }
    void PollRun() {
        if (!run_.process) return;
        int code = 0;
        if (!SDL_WaitProcess(run_.process, false, &code)) return;
        SDL_DestroyProcess(run_.process);
        run_.process = nullptr;
        SDL_RestoreWindow(window_);
        SDL_RaiseWindow(window_);
        const Uint64 seconds = (SDL_GetTicks() - run_.started) / 1000;
        if (code == 0)
            Report(GAME_NAME " closed after " + std::to_string(seconds / 60) + " min.", kSteel);
        else
            Report(GAME_NAME " stopped with an error (code " + std::to_string(code) + "). Its log is in " +
                   LogFolder(places_, settings_.log_dir) + ": please attach it to a report.", kBad);
    }

    SDL_Window* window_;
    Places places_;
    Ini ini_;
    Settings settings_;
    FolderCheck check_;
    Run run_;
    std::vector<Gamepad> gamepads_;
    char folder_edit_[2048] = "";
    int forced_tab_ = -1;
    unsigned frames_ = 0;
};

void Style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(32, 26);
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(12, 10);
    s.ItemInnerSpacing = ImVec2(10, 6);
    s.CellPadding = ImVec2(8, 7);
    s.WindowRounding = 0.0f;
    s.FrameRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.PopupRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.ScrollbarSize = 12.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 1.0f;
    s.TabBarBorderSize = 2.0f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kBoard;
    c[ImGuiCol_ChildBg] = kBoard;
    c[ImGuiCol_PopupBg] = kPanel;
    c[ImGuiCol_Text] = kIce;
    c[ImGuiCol_TextDisabled] = kSteel;
    c[ImGuiCol_Border] = Hex(0x24405F);
    c[ImGuiCol_Separator] = Hex(0x24405F);
    c[ImGuiCol_FrameBg] = kPanel;
    c[ImGuiCol_FrameBgHovered] = kPanelHi;
    c[ImGuiCol_FrameBgActive] = Hex(0x22406A);
    c[ImGuiCol_Button] = kPanelHi;
    c[ImGuiCol_ButtonHovered] = Hex(0x24476F);
    c[ImGuiCol_ButtonActive] = kBlueLine;
    c[ImGuiCol_Header] = Hex(0x2563B8, 0.55f);
    c[ImGuiCol_HeaderHovered] = Hex(0x2563B8, 0.75f);
    c[ImGuiCol_HeaderActive] = kBlueLine;
    c[ImGuiCol_CheckMark] = kIce;
    c[ImGuiCol_Tab] = kBoard;
    c[ImGuiCol_TabHovered] = kPanelHi;
    c[ImGuiCol_TabSelected] = kPanel;
    c[ImGuiCol_TabSelectedOverline] = kBlueLine;
    c[ImGuiCol_TabDimmed] = kBoard;
    c[ImGuiCol_TabDimmedSelected] = kPanel;
    c[ImGuiCol_NavCursor] = Hex(0x6EA8FF);
    c[ImGuiCol_ScrollbarBg] = kBoard;
    c[ImGuiCol_ScrollbarGrab] = kPanelHi;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

}  // namespace

int main(int, char**) {
    SDL_SetHint(SDL_HINT_APP_NAME, GAME_NAME " launcher");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_NAME, SDL_GetError(), nullptr);
        return 1;
    }
    const std::string screenshot = Env("BB_LAUNCHER_SCREENSHOT");
    const bool deck = Env("SteamDeck") == "1" || Env("SteamGamepadUI") == "1";
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (deck) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* window = SDL_CreateWindow(GAME_NAME, 1040, 680, flags);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_NAME, SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 760, 520);
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    float scale = SDL_GetWindowDisplayScale(window);
    if (scale <= 0.0f) scale = 1.0f;
    Style(scale);
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_launcher_font),
                                   int(bb_launcher_font_end - bb_launcher_font), 18.0f, &config);
    ImFont* bold = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_launcher_bold),
                                                  int(bb_launcher_bold_end - bb_launcher_bold), 18.0f, &config);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    Launcher launcher(window);
    launcher.bold = bold;
    int shot_frames = 0;
    bool quit = false;
    while (!quit) {
        SDL_Event event;
        // Idle: wait for input (the run and the dialog are polled four times a second).
        if (screenshot.empty() && SDL_WaitEventTimeout(&event, 250)) {
            do {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) quit = true;
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) quit = true;
                if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                    // Rebuild the style at the new scale.
                    scale = SDL_GetWindowDisplayScale(window);
                    Style(scale > 0.0f ? scale : 1.0f);
                }
            } while (SDL_PollEvent(&event));
        } else {
            SDL_Delay(10);
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) quit = true;
            }
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED && screenshot.empty()) {
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            launcher.Frame();  // keeps polling the game while minimised
            ImGui::EndFrame();
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        launcher.Frame();
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, kBoard.x, kBoard.y, kBoard.z, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!screenshot.empty() && ++shot_frames >= 6 && !launcher.Running()) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SaveBMP(shot, screenshot.c_str());
                SDL_DestroySurface(shot);
            }
            quit = true;
        }
        SDL_RenderPresent(renderer);
    }
    // A game still running keeps running: it doesn't depend on this window.
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
