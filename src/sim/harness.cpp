// Desktop preview of the Cardputer Hermes Terminal UI.
//
// Compiles the real drawing unit (src/app_draw.cpp) against the M5GFX SDL
// backend and scripts App's private state for each screen. Hardware-facing
// members are satisfied by the stubs in sim/stubs and the definitions below,
// so this file is the only place that knows the preview is not a device.
//
//   pio run -e native-sim && .pio/build/native-sim/program   interactive
//   .pio/build/native-sim/program --shots out/               PPM per screen
//   .pio/build/native-sim/program --list                     scenario names

#include "hermes_terminal/app.h"

#include <SDL.h>
#include <WiFi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../app_ui.h"
#include "hermes_terminal/ui_rules.h"

// ---------------------------------------------------------------------------
// Globals the stubs and the compiled drawing unit expect.

M5CardputerStub M5Cardputer;
WiFiStub WiFi;

namespace hermes_sim {

unsigned long frozenMs = 0;
bool frozen = false;

unsigned long simMillis()
{
    return frozen ? frozenMs : static_cast<unsigned long>(SDL_GetTicks());
}

void simDelay(unsigned long ms)
{
    if (!frozen) SDL_Delay(static_cast<Uint32>(ms));
}

// Fake device/network facts read through the stubbed members.
struct FakeDevice {
    bool connected = true;
    bool connectionFailed = false;
    std::string diagnostic = "CONNECTED";
    std::string authMode = "password";
    std::string ssid = "Workshop-5G";
    std::string ip = "192.168.1.42";
    int rssi = -58;
    unsigned long voiceElapsedMs = 0;
    std::uint64_t cacheBytes = 3'184'640;
};
FakeDevice device;

}  // namespace hermes_sim

String WiFiStub::SSID() const { return hermes_sim::device.ssid.c_str(); }
wl_status_t WiFiStub::status() const
{
    return hermes_sim::device.ssid.empty() ? WL_DISCONNECTED : WL_CONNECTED;
}
IPAddress WiFiStub::localIP() const
{
    return IPAddress(hermes_sim::device.ip.c_str());
}
int WiFiStub::RSSI() const { return hermes_sim::device.rssi; }

// ---------------------------------------------------------------------------
// Stubs for members whose real definitions live in hardware-heavy units.

namespace hermes_terminal {

using hermes_sim::device;

bool HermesClient::connected() { return device.connected; }
bool HermesClient::connectionFailed() const { return device.connectionFailed; }
String HermesClient::diagnostic() const { return device.diagnostic.c_str(); }
String HermesClient::authMode() const { return device.authMode.c_str(); }
std::uint64_t SdCache::usageBytes() { return device.cacheBytes; }
unsigned long VoiceCapture::elapsedMs() const { return device.voiceElapsedMs; }
const char* App::wifiReasonText(std::uint8_t reason) { return reason ? "NO AP" : ""; }

// App's listener overrides exist only to satisfy the vtable here.
void App::onHermesConnected() {}
void App::onHermesDisconnected(const String&) {}
void App::onHermesMessage(JsonDocument&) {}
void App::onHermesAuthCookieUpdated(const String&) {}

// ---------------------------------------------------------------------------
// Scenarios. Each one resets the app to a known state, sets the fields the
// screen reads, and is rendered with the real App::draw().

struct SimAccess {
    struct Scenario {
        const char* name;
        void (*apply)(App&);
    };

    static void reset(App& app)
    {
        device = hermes_sim::FakeDevice();
        app.screenSleep_ = false;
        app.screen_ = App::Screen::kSessions;
        app.status_ = "READY";
        app.usageText_ = "";
        app.activeSessionId_ = "";
        app.activeStoredSessionId_ = "";
        app.activeSessionTitle_ = "";
        app.timeline_ = "";
        app.scroll_ = 0;
        app.resumeRequestId_ = 0;
        app.historySyncPending_ = false;
        app.timelineFromCache_ = false;
        app.voiceRetryAvailable_ = false;
        app.compose_ = "";
        app.composeMode_ = App::ComposeMode::kPrompt;
        app.interactionType_ = "";
        app.interactionPrompt_ = "";
        app.approvalChoices_ = ",once,session,deny,";
        app.voiceTest_ = false;
        app.helpPage_ = 0;
        app.settingRow_ = 0;
        app.cacheClearConfirm_ = false;
        app.sessions_.clear();
        app.selectedSession_ = 0;
        app.sessionsSyncPending_ = false;
        app.sessionsRequestId_ = 0;
        app.sessionsWindowOffset_ = 0;
        app.sessionsTotal_ = 0;
        app.sleepFrame_ = 0;
        app.wifiNetworks_.clear();
        app.wifiPhase_ = App::WifiPhase::kList;
        app.selectedWifi_ = 0;
        app.wifiTargetSsid_ = "";
        app.wifiSavedSsid_ = "";
        app.wifiNotice_ = "";
        app.voice_.active_ = false;
        app.voice_.levelBars_ = 0;
        app.cache_.enabled_ = true;
        app.cache_.error_ = "";
        app.config_.baseUrl = "https://hermes.lab.example.net";
        app.config_.profile = "";
        app.config_.screenSleepSeconds = 60;
        app.config_.screenSleepBrightness = 48;
        app.config_.ttsVolume = 180;
    }

    static void addSession(App& app, const char* id, const char* title,
                           const char* preview, const char* state)
    {
        CachedSession session;
        session.id = id;
        session.title = title;
        session.preview = preview;
        session.state = state;
        app.sessions_.push_back(session);
    }

    static void fillSessions(App& app)
    {
        addSession(app, "s_9f1c2ab7", "Cloud backup daily status",
                   "Three archives verified successfully", "active");
        addSession(app, "s_44d0e1", "Refactor the Cardputer voice capture ring buffer to avoid",
                   "Reviewing voice_capture.cpp queue depth", "idle");
        addSession(app, "s_88aa01", "Deploy report", "cloud operations", "saved");
        addSession(app, "s_12ee9c", "Untitled", "", "");
        addSession(app, "s_5b6c7d", "Проверка кириллицы в заголовке",
                   "Кириллица не рендерится шрифтом 1", "idle");
        addSession(app, "s_77aa88", "Kitchen renovation quotes",
                   "Compared four contractor bids and their timelines in detail", "saved");
        addSession(app, "s_1a2b3c", "Weekly meal plan", "Weekly meal plan", "saved");
        addSession(app, "s_9c8b7a", "Hermes gateway TLS renewal",
                   "certbot renew --deploy-hook 'systemctl reload nginx'", "active");
        addSession(app, "s_0d0d0d", "Trip to Kyoto", "Ryokan shortlist", "idle");
        addSession(app, "s_e1e1e1", "Fix flaky CI on main", "pytest -x -q", "saved");
        addSession(app, "s_f2f2f2", "Garden irrigation schedule", "", "idle");
        addSession(app, "s_a3a3a3", "Reading list 2026", "Books", "saved");
        app.sessionsTotal_ = 143;
        app.sessionsWindowOffset_ = 24;
    }

    static const char* longChat()
    {
        return "YOU: Summarise the deployment incident from last night and "
               "list the follow-ups.\n\n"
               "HERMES: The 02:14 UTC deploy of gateway-1.8.3 rolled back after "
               "the health check on /api/status failed for 4 minutes. Root "
               "cause was an expired intermediate certificate on the reverse "
               "proxy, not the application build.\n\n"
               "Follow-ups:\n"
               "1. Rotate the intermediate certificate before 2026-09-10.\n"
               "2. Add certificate expiry to the pre-deploy checklist.\n"
               "3. Alert on TLS handshake failures above 1% per minute.\n\n"
               "YOU: Draft the alert rule.\n\n"
               "HERMES: ";
    }

    static void chatBase(App& app)
    {
        reset(app);
        app.screen_ = App::Screen::kChat;
        app.activeSessionId_ = "rt_1";
        app.activeStoredSessionId_ = "s_9f1c2ab7";
        app.activeSessionTitle_ = "Cloud backup daily status";
        app.timeline_ = "YOU: Verify the latest archive.\n\n"
                        "HERMES: Backup completed. Three archives verified "
                        "successfully.\n";
        app.scroll_ = kScrollFollowBottom;
    }

    // --- sessions -----------------------------------------------------------
    static void sessionsConnecting(App& app)
    {
        reset(app);
        device.connected = false;
        app.status_ = "CONNECTING";
    }
    static void sessionsLoading(App& app)
    {
        reset(app);
        app.sessionsRequestId_ = 3;
        app.status_ = "LOADING SESSIONS";
    }
    static void sessionsError(App& app)
    {
        reset(app);
        device.connected = false;
        device.connectionFailed = true;
        device.diagnostic = "TLS CONNECT FAILED";
        app.status_ = "TLS CONNECT FAILED - RETRYING";
    }
    static void sessionsEmpty(App& app)
    {
        reset(app);
        app.status_ = "0 HERMES SESSIONS";
    }
    static void sessionsList(App& app)
    {
        reset(app);
        fillSessions(app);
        app.selectedSession_ = 1;
        app.status_ = "143 HERMES SESSIONS";
    }
    static void sessionsSyncing(App& app)
    {
        sessionsList(app);
        app.sessionsSyncPending_ = true;
        app.status_ = "SYNCED 300 SESSIONS";
    }
    static void sessionsOffline(App& app)
    {
        sessionsList(app);
        device.connected = false;
        app.selectedSession_ = 4;
        app.status_ = "OFFLINE CACHE";
    }

    // --- chat ---------------------------------------------------------------
    static void chatLoading(App& app)
    {
        chatBase(app);
        app.activeSessionId_ = "";
        app.resumeRequestId_ = 7;
        app.activeSessionTitle_ =
            "Refactor the Cardputer voice capture ring buffer to avoid drops";
        app.status_ = "RESUMING SESSION";
    }
    static void chatCachingHistory(App& app)
    {
        chatLoading(app);
        app.resumeRequestId_ = 0;
        app.historySyncPending_ = true;
        app.status_ = "LOADING HISTORY";
    }
    static void chatReady(App& app)
    {
        chatBase(app);
        app.usageText_ = "812t";
    }
    static void chatLong(App& app)
    {
        chatBase(app);
        app.activeSessionTitle_ = "Hermes gateway TLS renewal";
        app.timeline_ = longChat();
        app.timeline_ += "Here is a Prometheus rule that fires when handshake "
                         "failures exceed one percent of attempts for five "
                         "minutes.\n";
        app.usageText_ = "12k";
    }
    static void chatScrolled(App& app)
    {
        chatLong(app);
        app.scroll_ = 4;
    }
    static void chatWorking(App& app)
    {
        chatBase(app);
        app.activeSessionTitle_ = "Hermes gateway TLS renewal";
        app.timeline_ = longChat();
        app.timeline_ += "Drafting the rule";
        app.status_ = "HERMES THINKING";
        app.usageText_ = "12k";
    }
    static void chatTool(App& app)
    {
        chatWorking(app);
        app.status_ = "TOOL terminal";
    }
    static void chatCyrillic(App& app)
    {
        chatBase(app);
        app.activeSessionTitle_ = "Проверка кириллицы в заголовке";
        app.timeline_ = "YOU: Расскажи коротко, что случилось ночью с деплоем.\n\n"
                        "HERMES: Деплой gateway-1.8.3 в 02:14 UTC откатился: проверка "
                        "/api/status не проходила четыре минуты. Причина — истёкший "
                        "промежуточный сертификат на обратном прокси, а не сборка.\n\n"
                        "Что сделать:\n1. Обновить сертификат до 2026-09-10.\n"
                        "2. Добавить срок действия в чек-лист перед деплоем.\n";
        app.status_ = "ЖДУ ОТВЕТ HERMES";
        app.usageText_ = "3k";
    }
    static void chatVoiceRetry(App& app)
    {
        chatBase(app);
        app.voiceRetryAvailable_ = true;
        app.status_ = "WAIT FOR HERMES - V RETRY";
    }
    static void chatOfflineCache(App& app)
    {
        chatBase(app);
        device.connected = false;
        app.timelineFromCache_ = true;
        app.status_ = "OFFLINE CACHE";
    }
    static void chatError(App& app)
    {
        chatBase(app);
        app.status_ = "RPC ERROR {\"code\":-32001,\"message\":\"session not found\"}";
    }

    // --- compose / interaction ---------------------------------------------
    static void compose(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kCompose;
        app.compose_ = "Check whether the nightly archive job also copies the "
                       "photo library";
    }
    static void composeLong(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kCompose;
        app.compose_ =
            "Write a short status update for the team covering the backup "
            "verification, the certificate rotation that is due next week, "
            "the flaky CI job on main that fails roughly one run in five, and "
            "the plan to move the gateway behind the new reverse proxy. Keep "
            "it under two hundred words, use plain language, and finish with "
            "three concrete asks: a reviewer for the proxy config, a decision "
            "on the retention policy, and a date for the migration rehearsal. "
            "Also mention the Cardputer terminal preview harness.";
    }
    static void composeCommand(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kCompose;
        app.compose_ = "/model claude-sonnet-5";
        app.status_ = "HERMES COMMAND";
    }
    static void steer(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kCompose;
        app.composeMode_ = App::ComposeMode::kSteer;
        app.compose_ = "Skip the tests, just show the diff";
        app.status_ = "STEER CURRENT TURN";
    }
    static void approval(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kInteraction;
        app.interactionType_ = "approval.request";
        app.interactionPrompt_ =
            "Allow terminal command: certbot renew --deploy-hook "
            "'systemctl reload nginx' in /etc/letsencrypt";
        app.approvalChoices_ = ",once,session,deny,";
        app.status_ = "APPROVAL REQUIRED";
    }
    static void clarify(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kInteraction;
        app.interactionType_ = "clarify.request";
        app.interactionPrompt_ =
            "[1/2] Which environment should the certificate be rotated in?\n"
            "Choices: staging, production, both";
        app.compose_ = "production";
        app.status_ = "HERMES QUESTION";
    }
    static void secret(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kInteraction;
        app.interactionType_ = "secret.request";
        app.interactionPrompt_ = "Hermes needs the SMTP relay password to "
                                 "finish the alert configuration.";
        app.compose_ = "hunter2";
        app.status_ = "SECRET REQUIRED";
    }
    static void playback(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kPlayback;
        app.status_ = "HERMES SYNTHESIZING SPEECH";
    }

    // --- voice --------------------------------------------------------------
    static void recording(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kRecording;
        app.voice_.active_ = true;
        app.voice_.levelBars_ = 8;
        device.voiceElapsedMs = 7300;
        app.status_ = "LISTENING";
    }
    static void recordingProcessing(App& app)
    {
        recording(app);
        app.voice_.active_ = false;
        app.status_ = "TRANSCRIBING VOICE";
    }
    static void micTest(App& app)
    {
        recording(app);
        app.voiceTest_ = true;
        app.voice_.levelBars_ = 3;
        device.voiceElapsedMs = 2100;
    }

    // --- help / settings / status -------------------------------------------
    static void helpManual(App& app)
    {
        chatBase(app);
        app.screen_ = App::Screen::kHelpSettings;
        app.helpPage_ = 0;
    }
    static void helpSetup(App& app)
    {
        helpManual(app);
        app.helpPage_ = 1;
        app.settingRow_ = 3;
        app.config_.profile = "workshop";
    }
    static void helpSetupCacheClear(App& app)
    {
        helpSetup(app);
        app.settingRow_ = 7;
        app.cacheClearConfirm_ = true;
        app.status_ = "PRESS C AGAIN TO CLEAR CACHE";
    }
    static void helpStatus(App& app)
    {
        helpManual(app);
        app.helpPage_ = 2;
        app.status_ = "LIVE / CACHED";
    }
    static void helpStatusOffline(App& app)
    {
        helpStatus(app);
        device.connected = false;
        device.rssi = -81;
        device.ssid = "";
        app.config_.wifiSsid = "Workshop-5G";
        app.cache_.enabled_ = false;
        app.cache_.error_ = "CACHE SCHEMA MISMATCH";
        app.status_ = "SOCKET CLOSED - RETRYING";
    }

    // --- wifi ---------------------------------------------------------------
    static void addNetwork(App& app, const char* ssid, int rssi, bool secured)
    {
        App::WifiNetwork network;
        network.ssid = ssid;
        network.rssi = static_cast<std::int8_t>(rssi);
        network.secured = secured;
        app.wifiNetworks_.push_back(network);
    }
    static void wifiScanning(App& app)
    {
        reset(app);
        app.screen_ = App::Screen::kWifi;
        app.wifiPhase_ = App::WifiPhase::kScanning;
    }
    static void wifiList(App& app)
    {
        reset(app);
        app.screen_ = App::Screen::kWifi;
        app.wifiSavedSsid_ = "Workshop-5G";
        addNetwork(app, "Workshop-5G", -52, true);
        addNetwork(app, "Kuznetsov Home Network Extended Range AP", -61, true);
        addNetwork(app, "Kafe Zolotoy Kolos Guest", -66, false);
        addNetwork(app, "Дача", -70, true);
        addNetwork(app, "TP-LINK_7A21", -74, true);
        addNetwork(app, "iPhone (Anton)", -78, true);
        addNetwork(app, "DIRECT-3F-HP OfficeJet", -83, true);
        addNetwork(app, "xfinitywifi", -88, false);
        app.selectedWifi_ = 1;
    }
    static void wifiPassword(App& app)
    {
        wifiList(app);
        app.wifiPhase_ = App::WifiPhase::kPassword;
        app.wifiTargetSsid_ = "Kuznetsov Home Network Extended Range AP";
        app.compose_ = "correct horse battery staple 2026!";
    }
    static void wifiJoining(App& app)
    {
        wifiList(app);
        app.wifiPhase_ = App::WifiPhase::kJoining;
        app.wifiTargetSsid_ = "Kuznetsov Home Network Extended Range AP";
    }
    static void wifiFailed(App& app)
    {
        wifiList(app);
        app.wifiNotice_ = "JOIN FAILED / PASSWORD?";
    }
    static void wifiEmpty(App& app)
    {
        reset(app);
        app.screen_ = App::Screen::kWifi;
        device.ssid = "";
        app.wifiNotice_ = "NO NETWORKS FOUND";
    }

    // --- sleep --------------------------------------------------------------
    static void sleepReady(App& app)
    {
        chatBase(app);
        app.screenSleep_ = true;
        app.status_ = "LIVE / CACHED";
    }
    static void sleepWorking(App& app)
    {
        sleepReady(app);
        app.status_ = "HERMES THINKING";
        app.sleepFrame_ = 12;
    }
    static void sleepOffline(App& app)
    {
        reset(app);
        app.screenSleep_ = true;
        device.connected = false;
        app.status_ = "TLS CONNECT FAILED - RETRYING";
    }
    static void sleepCyrillic(App& app)
    {
        sleepReady(app);
        app.activeSessionTitle_ = "Проверка кириллицы в заголовке";
        app.status_ = "ОТВЕТ ГОТОВ";
    }

    static const std::vector<Scenario>& scenarios()
    {
        static const std::vector<Scenario> list = {
            {"sessions-connecting", sessionsConnecting},
            {"sessions-loading", sessionsLoading},
            {"sessions-error", sessionsError},
            {"sessions-empty", sessionsEmpty},
            {"sessions-list", sessionsList},
            {"sessions-syncing", sessionsSyncing},
            {"sessions-offline", sessionsOffline},
            {"chat-loading", chatLoading},
            {"chat-caching-history", chatCachingHistory},
            {"chat-ready", chatReady},
            {"chat-long", chatLong},
            {"chat-scrolled", chatScrolled},
            {"chat-working", chatWorking},
            {"chat-tool", chatTool},
            {"chat-cyrillic", chatCyrillic},
            {"chat-voice-retry", chatVoiceRetry},
            {"chat-offline-cache", chatOfflineCache},
            {"chat-error", chatError},
            {"compose", compose},
            {"compose-long", composeLong},
            {"compose-command", composeCommand},
            {"steer", steer},
            {"approval", approval},
            {"clarify", clarify},
            {"secret", secret},
            {"playback", playback},
            {"recording", recording},
            {"recording-processing", recordingProcessing},
            {"mic-test", micTest},
            {"help-manual", helpManual},
            {"help-setup", helpSetup},
            {"help-setup-cache-clear", helpSetupCacheClear},
            {"help-status", helpStatus},
            {"help-status-offline", helpStatusOffline},
            {"wifi-scanning", wifiScanning},
            {"wifi-list", wifiList},
            {"wifi-password", wifiPassword},
            {"wifi-joining", wifiJoining},
            {"wifi-failed", wifiFailed},
            {"wifi-empty", wifiEmpty},
            {"sleep-ready", sleepReady},
            {"sleep-working", sleepWorking},
            {"sleep-offline", sleepOffline},
            {"sleep-cyrillic", sleepCyrillic},
        };
        return list;
    }

    static void render(App& app)
    {
        app.dirty_ = true;
        app.draw();
    }
};

}  // namespace hermes_terminal

// ---------------------------------------------------------------------------
// Entry point.

namespace {

using hermes_terminal::App;
using hermes_terminal::SimAccess;

std::string shotsDirectory;
bool listOnly = false;

bool writePpm(const std::string& path)
{
    auto& display = M5Cardputer.Display;
    const int width = display.width();
    const int height = display.height();
    std::vector<lgfx::rgb888_t> pixels(static_cast<std::size_t>(width) * height);
    display.readRect(0, 0, width, height, pixels.data());
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (const auto& pixel : pixels) {
        const unsigned char rgb[3] = {pixel.r, pixel.g, pixel.b};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    return true;
}

int userMain(bool* running)
{
    auto& display = M5Cardputer.Display;
    display.init();
    display.setRotation(1);

    static App app;
    const auto& scenarios = SimAccess::scenarios();

    if (!shotsDirectory.empty()) {
        hermes_sim::frozen = true;
        hermes_sim::frozenMs = 100000;
        for (const auto& scenario : scenarios) {
            scenario.apply(app);
            SimAccess::render(app);
            display.display();
            const std::string path = shotsDirectory + "/" + scenario.name + ".ppm";
            if (!writePpm(path)) {
                fprintf(stderr, "could not write %s\n", path.c_str());
                return 1;
            }
            printf("%s\n", path.c_str());
        }
        fflush(stdout);
        // Panel_sdl::main keeps running until the window closes; a headless
        // render has nothing more to show.
        std::exit(0);
    }

    std::size_t current = 0;
    scenarios[current].apply(app);
    bool leftHeld = false;
    bool rightHeld = false;
    unsigned long lastAutoMs = SDL_GetTicks();
    while (*running) {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        const bool left = keys[SDL_SCANCODE_LEFT] != 0;
        const bool right = keys[SDL_SCANCODE_RIGHT] != 0 || keys[SDL_SCANCODE_SPACE] != 0;
        if (keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_Q]) break;
        bool changed = false;
        if (right && !rightHeld) {
            current = (current + 1) % scenarios.size();
            changed = true;
        } else if (left && !leftHeld) {
            current = (current + scenarios.size() - 1) % scenarios.size();
            changed = true;
        }
        rightHeld = right;
        leftHeld = left;
        if (keys[SDL_SCANCODE_A] && SDL_GetTicks() - lastAutoMs > 2500) {
            current = (current + 1) % scenarios.size();
            changed = true;
        }
        if (changed) {
            lastAutoMs = SDL_GetTicks();
            scenarios[current].apply(app);
            std::string title = "Hermes terminal preview: ";
            title += scenarios[current].name;
            title += "   (LEFT/RIGHT switch, hold A to cycle, ESC quit)";
            static_cast<lgfx::Panel_sdl*>(display.panel())->setWindowTitle(title.c_str());
        }
        SimAccess::render(app);
        SDL_Delay(33);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--shots") == 0 && index + 1 < argc) {
            shotsDirectory = argv[++index];
        } else if (strcmp(argv[index], "--list") == 0) {
            listOnly = true;
        }
    }
    if (listOnly) {
        for (const auto& scenario : SimAccess::scenarios()) printf("%s\n", scenario.name);
        return 0;
    }
    return lgfx::Panel_sdl::main(userMain, 128);
}
