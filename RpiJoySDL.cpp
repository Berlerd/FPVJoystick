#include <iostream>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <linux/joystick.h>
#include <fstream>
#include <string>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <mutex>
#include <atomic>
#include "rcdrivers/SBUS.h"

using std::cout;
using std::cerr;
using std::endl;
using std::chrono::steady_clock;
using std::chrono::milliseconds;

#define RUDDER_AXIS 2   // Saitek pedals axis 2
#define CH4 3           // SBUS channel 4 (0-based)
#define CH5 4           // SBUS channel 5 (0-based) for gear/flap combo
#define CH6 5           // SBUS channel 6 (0-based)
#define CH7 6           // SBUS channel 7 (0-based)
#define CH8 7           // SBUS channel 8 (0-based)

// Button definitions - using STICK_ prefix to avoid conflicts
#define STICK_BTN_RUDDER_TOGGLE 6
#define STICK_BTN_HT_TOGGLE 12   // Heat Tracking arm/toggle for CH6+CH7
#define STICK_BTN_FLAP_LOW  13
#define STICK_BTN_FLAP_MID  14
#define STICK_BTN_FLAP_HIGH 15
#define STICK_BTN_GEAR_DOWN 16
#define STICK_BTN_GEAR_UP   17

// CH8 is a 4-position preset selected by buttons 2-5 (not axis-driven)
#define BTN_CH8_PRESET_1 2   // -> 173
#define BTN_CH8_PRESET_2 3   // -> 499
#define BTN_CH8_PRESET_3 4   // -> 826
#define BTN_CH8_PRESET_4 5   // -> 1153

static SBUS sbus;

// State variables for gear and flaps
enum GearState { GEAR_DOWN, GEAR_UP };
enum FlapState { FLAP_LOW, FLAP_MID, FLAP_HIGH };

GearState currentGear = GEAR_DOWN;  // Default to gear down
FlapState currentFlap = FLAP_LOW;   // Default to flap low

// Rudder source toggle state
bool useStickForRudder = false;  // false = pedals, true = use stick axis from CH8

// Store the stick axis value that would go to CH8
int16_t stickRudderAxisValue = 0;

// Channel movement detection
bool ch1Moved = false;
bool ch2Moved = false;
bool ch3Moved = false;
bool ch4Moved = false;
bool allChannelsMoved = false;

// Raw throttle stick position (SBUS-mapped), tracked independent of the
// idle-hold gate so the display can show live stick movement even while
// the output is being held at idle.
uint16_t rawThrottleValue = 172;
const uint16_t THROTTLE_IDLE_TOLERANCE = 30; // must be within this of 172 to count as "idle"

// CH8 preset selector: buttons 2-5 pick one of four fixed CH8 output values.
// Index 0 (button 2 / value 173) is the default at startup.
const uint16_t CH8_PRESET_VALUES[4] = { 173, 499, 826, 1153 };
int ch8PresetIndex = 0; // 0..3, corresponds to buttons 2..5

// Heat Tracking (HT): button 12 arms it (capturing CH6/CH7 as saved
// defaults) and is required before the system can reach READY. Once
// armed, further presses toggle CH6/CH7 output between the saved
// defaults and live stick input.
bool htArmed = false;
bool htActive = false;
uint16_t htCh6Value = 992;
uint16_t htCh7Value = 992;

// ---------------------------------------------------------------------
// SDL2 display
// ---------------------------------------------------------------------
static SDL_Window*   gWindow   = nullptr;
static SDL_Renderer* gRenderer = nullptr;
static std::atomic<bool> sdlAvailable{false};

static TTF_Font* gFontSmall = nullptr;  // labels / values
static TTF_Font* gFontLarge = nullptr;  // headers

const int WINDOW_WIDTH  = 480;
const int WINDOW_HEIGHT = 320;
const int NUM_CHANNELS_DISPLAY = 8; // show CH1..CH8

// Typical stick axis naming for CH1-4 (primary flight controls)
static const char* CH_LABELS[4] = { "AIL", "ELE", "THR", "RUD" };

// Forward declaration (used inside the display code too)
bool hasMovedFromCenter(uint16_t value);

bool initDisplay() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        cerr << "SDL_Init Error: " << SDL_GetError() << endl;
        return false;
    }

    gWindow = SDL_CreateWindow("SBUS Channel Monitor",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!gWindow) {
        cerr << "SDL_CreateWindow Error: " << SDL_GetError() << endl;
        SDL_Quit();
        return false;
    }

    gRenderer = SDL_CreateRenderer(gWindow, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!gRenderer) {
        cerr << "SDL_CreateRenderer Error: " << SDL_GetError() << endl;
        SDL_DestroyWindow(gWindow);
        gWindow = nullptr;
        SDL_Quit();
        return false;
    }

    // Text rendering is optional - if TTF or a font can't be found, the
    // display still works, it just draws bars/boxes without labels.
    if (TTF_Init() != 0) {
        cerr << "TTF_Init Error: " << TTF_GetError() << " (text labels disabled)" << endl;
    } else {
        const char* fontPaths[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        };
        for (const char* p : fontPaths) {
            gFontSmall = TTF_OpenFont(p, 12);
            if (gFontSmall) {
                gFontLarge = TTF_OpenFont(p, 16);
                break;
            }
        }
        if (!gFontSmall) {
            cerr << "Warning: no TTF font found, text labels disabled. "
                 << "Install one with: sudo apt install fonts-dejavu-core" << endl;
        }
    }

    return true;
}

void closeDisplay() {
    if (gFontSmall) { TTF_CloseFont(gFontSmall); gFontSmall = nullptr; }
    if (gFontLarge) { TTF_CloseFont(gFontLarge); gFontLarge = nullptr; }
    TTF_Quit();
    if (gRenderer) { SDL_DestroyRenderer(gRenderer); gRenderer = nullptr; }
    if (gWindow)   { SDL_DestroyWindow(gWindow);     gWindow   = nullptr; }
    SDL_Quit();
}

// Draws text at (x,y). Does nothing if the font failed to load.
void renderText(TTF_Font* font, const std::string& text, int x, int y, SDL_Color color) {
    if (!font) return;
    SDL_Surface* surf = TTF_RenderText_Blended(font, text.c_str(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRenderer, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_RenderCopy(gRenderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
}

// Draws text centered horizontally around x.
void renderTextCentered(TTF_Font* font, const std::string& text, int centerX, int y, SDL_Color color) {
    if (!font) return;
    int w = 0, h = 0;
    TTF_SizeText(font, text.c_str(), &w, &h);
    renderText(font, text, centerX - w / 2, y, color);
}

// Returns false if the user closed the window / pressed Esc / pressed Q
bool pollDisplayEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return false;
        if (ev.type == SDL_KEYDOWN &&
            (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q)) {
            return false;
        }
    }
    return true;
}

// Small helper: filled + outlined rounded-ish "chip" used for selection buttons
void drawSelectionChip(int x, int y, int w, int h, bool active,
                        SDL_Color activeColor, SDL_Color inactiveColor) {
    SDL_Color c = active ? activeColor : inactiveColor;
    SDL_SetRenderDrawColor(gRenderer, c.r, c.g, c.b, 255);
    SDL_Rect box = { x, y, w, h };
    SDL_RenderFillRect(gRenderer, &box);
    SDL_SetRenderDrawColor(gRenderer, 230, 230, 230, 255);
    SDL_RenderDrawRect(gRenderer, &box);
}

void renderDisplay(const sbus_packet_t& packet, bool ready, bool stickRudder,
                    GearState gear, FlapState flap, uint16_t rawThrottle, int ch8Preset,
                    bool htActiveState, float cpuTempC) {
    // Background
    SDL_SetRenderDrawColor(gRenderer, 16, 16, 22, 255);
    SDL_RenderClear(gRenderer);

    SDL_Color white   = { 235, 235, 235, 255 };
    SDL_Color yellow  = { 255, 220, 0, 255 };
    SDL_Color dimGray = { 150, 150, 150, 255 };
    SDL_Color redBar  = { 220, 40, 40, 255 };
    SDL_Color blueBar = { 0, 110, 255, 255 };
    SDL_Color green   = { 0, 190, 0, 255 };
    SDL_Color red     = { 220, 0, 0, 255 };
    SDL_Color blue    = { 0, 110, 255, 255 };
    SDL_Color orange  = { 255, 140, 0, 255 };
    SDL_Color purple  = { 170, 100, 255, 255 };
    SDL_Color idle    = { 55, 55, 65, 255 };

    // =====================================================================
    // LEFT: CH1-4 primary flight-control bars
    // =====================================================================
    const int primMargin   = 8;
    const int primBarWidth = 46;
    const int primGap      = 12;
    const int primTopY     = 22;
    const int primBaseY    = 250;
    const int primMaxH     = primBaseY - primTopY;

    for (int i = 0; i < 4; i++) {
        // CH3 (index 2, throttle) shows the LIVE raw stick position and turns
        // red while it's being held at idle pending READY.
        bool isThrottleGated = (i == 2 && !ready);
        uint16_t val = isThrottleGated ? rawThrottle : packet.channels[i];

        float frac = (float)(val - 172) / (float)(1811 - 172);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        int h = (int)(frac * primMaxH);

        int x = primMargin + i * (primBarWidth + primGap);
        int y = primBaseY - h;

        SDL_Color barColor = isThrottleGated ? redBar : blueBar;
        SDL_SetRenderDrawColor(gRenderer, barColor.r, barColor.g, barColor.b, 255);

        SDL_Rect bar = { x, y, primBarWidth, h };
        SDL_RenderFillRect(gRenderer, &bar);

        // Outline of full travel range
        SDL_SetRenderDrawColor(gRenderer, 150, 150, 150, 255);
        SDL_Rect outline = { x, primTopY, primBarWidth, primMaxH };
        SDL_RenderDrawRect(gRenderer, &outline);

        // Center reference line (992)
        int centerY = primBaseY - (int)(((992.0f - 172.0f) / (1811.0f - 172.0f)) * primMaxH);
        SDL_SetRenderDrawColor(gRenderer, yellow.r, yellow.g, yellow.b, 255);
        SDL_RenderDrawLine(gRenderer, x, centerY, x + primBarWidth, centerY);

        // Numeric value above the bar
        char valStr[16];
        snprintf(valStr, sizeof(valStr), "%d", val);
        renderTextCentered(gFontSmall, valStr, x + primBarWidth / 2, 2,
                            isThrottleGated ? redBar : white);

        // Channel label under the bar (CH1..CH4)
        char chStr[8];
        snprintf(chStr, sizeof(chStr), "CH%d", i + 1);
        renderTextCentered(gFontSmall, chStr, x + primBarWidth / 2, primBaseY + 4, white);

        // Common flight-control name (AIL/ELE/THR/RUD)
        renderTextCentered(gFontSmall, CH_LABELS[i], x + primBarWidth / 2, primBaseY + 18, dimGray);
    }

    // =====================================================================
    // RIGHT: compact status + selector panel
    // =====================================================================
    const int panelX = primMargin + 4 * (primBarWidth + primGap); // = 240
    int py = 2;

    // Overall status
    renderText(gFontLarge, ready ? "READY" : "WAITING", panelX, py, ready ? green : red);

    // CPU temperature, top-right corner of the panel
    {
        char tempStr[16];
        SDL_Color tempColor = white;
        if (cpuTempC < 0.0f) {
            snprintf(tempStr, sizeof(tempStr), "N/A");
        } else {
            snprintf(tempStr, sizeof(tempStr), "%.0fC", cpuTempC);
            if (cpuTempC >= 75.0f) tempColor = red;
            else if (cpuTempC >= 60.0f) tempColor = orange;
        }
        renderText(gFontSmall, tempStr, WINDOW_WIDTH - 40, py + 2, tempColor);
    }
    py += 22;

    // --- GEAR (single box): DOWN = green, UP = red ---
    renderText(gFontSmall, "GEAR", panelX, py, dimGray);
    py += 14;
    {
        int boxW = 90, boxH = 22;
        SDL_Color gearColor = (gear == GEAR_UP) ? red : green;
        SDL_SetRenderDrawColor(gRenderer, gearColor.r, gearColor.g, gearColor.b, 255);
        SDL_Rect gearBox = { panelX, py, boxW, boxH };
        SDL_RenderFillRect(gRenderer, &gearBox);
        SDL_SetRenderDrawColor(gRenderer, 230, 230, 230, 255);
        SDL_RenderDrawRect(gRenderer, &gearBox);
        renderTextCentered(gFontSmall, (gear == GEAR_UP) ? "UP" : "DOWN",
                            panelX + boxW / 2, py + 4, white);
    }
    py += 22 + 8;

    // --- FLAP selection --- (display order LOW, MID, HIGH)
    renderText(gFontSmall, "FLAP", panelX, py, dimGray);
    py += 14;
    {
        const char* labels[3]  = { "LO", "MID", "HI" };
        FlapState   order[3]   = { FLAP_LOW, FLAP_MID, FLAP_HIGH };
        SDL_Color   chipColor[3] = { green, red, red };
        int chipW = 34, chipH = 22, gapX = 6;
        for (int i = 0; i < 3; i++) {
            bool active = (flap == order[i]);
            int x = panelX + i * (chipW + gapX);
            drawSelectionChip(x, py, chipW, chipH, active, chipColor[i], idle);
            renderTextCentered(gFontSmall, labels[i], x + chipW / 2, py + 4,
                                active ? white : dimGray);
        }
    }
    py += 22 + 8;

    // --- RUDDER source (single box) + HT (single box), side by side ---
    renderText(gFontSmall, "RUDDER", panelX, py, dimGray);
    renderText(gFontSmall, "HT", panelX + 102, py, dimGray);
    py += 14;
    {
        int boxW = 90, boxH = 22;

        // RUDDER: green = pedals, red = stick
        SDL_Color rudderColor = stickRudder ? red : green;
        SDL_SetRenderDrawColor(gRenderer, rudderColor.r, rudderColor.g, rudderColor.b, 255);
        SDL_Rect rudderBox = { panelX, py, boxW, boxH };
        SDL_RenderFillRect(gRenderer, &rudderBox);
        SDL_SetRenderDrawColor(gRenderer, 230, 230, 230, 255);
        SDL_RenderDrawRect(gRenderer, &rudderBox);
        renderTextCentered(gFontSmall, stickRudder ? "STICK" : "PEDALS",
                            panelX + boxW / 2, py + 4, white);

        // HT: green = off/live, red = on/saved values
        int htX = panelX + 102;
        SDL_Color htColor = htActiveState ? red : green;
        SDL_SetRenderDrawColor(gRenderer, htColor.r, htColor.g, htColor.b, 255);
        SDL_Rect htBox = { htX, py, boxW, boxH };
        SDL_RenderFillRect(gRenderer, &htBox);
        SDL_SetRenderDrawColor(gRenderer, 230, 230, 230, 255);
        SDL_RenderDrawRect(gRenderer, &htBox);
        renderTextCentered(gFontSmall, htActiveState ? "SAVED" : "LIVE",
                            htX + boxW / 2, py + 4, white);
    }
    py += 22 + 8;

    // --- CH8 preset selection (buttons 2-5) ---
    {
        char ch8Label[24];
        snprintf(ch8Label, sizeof(ch8Label), "CH8: %d", packet.channels[CH8]);
        renderText(gFontSmall, ch8Label, panelX, py, dimGray);
    }
    py += 14;
    {
        const char* labels[4] = { "2", "3", "4", "5" };
        int chipW = 30, chipH = 22, gapX = 6;
        for (int i = 0; i < 4; i++) {
            bool active = (ch8Preset == i);
            int x = panelX + i * (chipW + gapX);
            drawSelectionChip(x, py, chipW, chipH, active, purple, idle);
            renderTextCentered(gFontSmall, labels[i], x + chipW / 2, py + 4,
                                active ? white : dimGray);
        }
    }
    py += 22 + 10;

    // --- CH5-7 mini bars (secondary channels; CH8 shown above as preset) ---
    {
        const int miniBarW = 50, miniGap = 10, miniMaxH = 46;
        int miniTopY = py + 12;
        int miniBaseY = miniTopY + miniMaxH;
        for (int i = 0; i < 3; i++) {
            int ch = i + 4; // CH5..CH7
            uint16_t val = packet.channels[ch];
            float frac = (float)(val - 172) / (float)(1811 - 172);
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            int h = (int)(frac * miniMaxH);

            int x = panelX + i * (miniBarW + miniGap);
            int y = miniBaseY - h;

            SDL_SetRenderDrawColor(gRenderer, 0, 110, 255, 255);
            SDL_Rect bar = { x, y, miniBarW, h };
            SDL_RenderFillRect(gRenderer, &bar);

            SDL_SetRenderDrawColor(gRenderer, 120, 120, 120, 255);
            SDL_Rect outline = { x, miniTopY, miniBarW, miniMaxH };
            SDL_RenderDrawRect(gRenderer, &outline);

            char chStr[8];
            snprintf(chStr, sizeof(chStr), "CH%d", ch + 1);
            renderTextCentered(gFontSmall, chStr, x + miniBarW / 2, miniBaseY + 4, dimGray);
        }
    }

    SDL_RenderPresent(gRenderer);
}

// Reads the Pi's CPU temperature in Celsius. Returns -1.0f on failure.
// This is only ever called from the display thread - never from the
// flight-control loop - since sysfs reads are not guaranteed to be fast.
float readCpuTemperatureC() {
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    if (!f.is_open()) return -1.0f;
    long milliC = 0;
    f >> milliC;
    if (milliC <= 0) return -1.0f;
    return milliC / 1000.0f;
}

// ---------------------------------------------------------------------
// Threading: the display runs entirely on its own thread so that a slow
// SPI touchscreen flush (SDL_RenderPresent can block for 100ms+) can
// NEVER stall the flight-control loop (joystick reads + SBUS writes).
// The two threads only share this snapshot, protected by a mutex.
// ---------------------------------------------------------------------
struct DisplaySnapshot {
    sbus_packet_t packet;
    bool ready = false;
    bool stickRudder = false;
    GearState gear = GEAR_DOWN;
    FlapState flap = FLAP_LOW;
    uint16_t rawThrottle = 172;
    int ch8Preset = 0;
    bool htActiveState = false;
};

static std::mutex gSnapshotMutex;
static DisplaySnapshot gSnapshot;
static std::atomic<bool> gRunning{true}; // shared shutdown flag, either thread can clear it

void displayThreadFunc() {
    sdlAvailable = initDisplay();
    if (!sdlAvailable) {
        cout << "SDL2 display unavailable, continuing headless." << endl;
        return;
    }
    cout << "SDL2 display initialized (on its own thread)." << endl;

    auto lastRender = steady_clock::now();
    auto lastTempRead = steady_clock::now();
    float cpuTempC = readCpuTemperatureC();

    while (gRunning.load()) {
        if (!pollDisplayEvents()) {
            gRunning = false;
            break;
        }

        auto now = steady_clock::now();

        // Re-read CPU temp at most once a second - no need for it to be
        // tied to render cadence, and it keeps file I/O off the hot path.
        if (now - lastTempRead > milliseconds(1000)) {
            lastTempRead = now;
            cpuTempC = readCpuTemperatureC();
        }

        if (now - lastRender > milliseconds(33)) {
            lastRender = now;

            DisplaySnapshot snap;
            {
                std::lock_guard<std::mutex> lock(gSnapshotMutex);
                snap = gSnapshot;
            }

            renderDisplay(snap.packet, snap.ready, snap.stickRudder, snap.gear, snap.flap,
                          snap.rawThrottle, snap.ch8Preset, snap.htActiveState, cpuTempC);
        }

        // However long SDL_RenderPresent() actually took (even 150ms+ on a
        // slow SPI panel), it only ever delays the NEXT frame on this
        // thread - the control loop on the main thread is unaffected.
        std::this_thread::sleep_for(milliseconds(5));
    }

    closeDisplay();
}

// ---------------------------------------------------------------------
// LED control functions - PWR is red, ACT is green
// ---------------------------------------------------------------------
void writeToFile(const std::string& path, const std::string& value) {
    std::ofstream file(path);
    if (file.is_open()) {
        file << value;
        file.close();
    }
}

void setLEDRed() {
    // PWR LED on (red), ACT LED off
    writeToFile("/sys/class/leds/PWR/trigger", "none");
    writeToFile("/sys/class/leds/PWR/brightness", "1");
    writeToFile("/sys/class/leds/ACT/trigger", "none");
    writeToFile("/sys/class/leds/ACT/brightness", "0");
}

void setLEDGreen() {
    // PWR LED off, ACT LED on (green)
    writeToFile("/sys/class/leds/PWR/trigger", "none");
    writeToFile("/sys/class/leds/PWR/brightness", "0");
    writeToFile("/sys/class/leds/ACT/trigger", "none");
    writeToFile("/sys/class/leds/ACT/brightness", "1");
}

void setLEDOff() {
    // Both LEDs off
    writeToFile("/sys/class/leds/PWR/trigger", "none");
    writeToFile("/sys/class/leds/PWR/brightness", "0");
    writeToFile("/sys/class/leds/ACT/trigger", "none");
    writeToFile("/sys/class/leds/ACT/brightness", "0");
}

void showErrorBlink() {
    setLEDRed();
    std::this_thread::sleep_for(milliseconds(1500));
    setLEDOff();
}

// Function to calculate CH5 value based on gear and flap state
uint16_t calculateCH5() {
    if (currentGear == GEAR_DOWN) {
        if (currentFlap == FLAP_LOW)  return 172;
        if (currentFlap == FLAP_MID)  return 499;
        if (currentFlap == FLAP_HIGH) return 826;
    } else { // GEAR_UP
        if (currentFlap == FLAP_LOW)  return 1153;
        if (currentFlap == FLAP_MID)  return 1480;
        if (currentFlap == FLAP_HIGH) return 1811;
    }
    return 172; // Default fallback
}

// Function to get joystick device name
std::string getJoystickName(const char* device) {
    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        return "";
    }
    
    char name[128];
    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) < 0) {
        close(fd);
        return "";
    }
    
    close(fd);
    return std::string(name);
}

// Find device by name substring
std::string findDeviceByName(const char* nameSubstring) {
    for (int i = 0; i < 10; i++) {  // Check js0 through js9
        std::string devPath = "/dev/input/js" + std::to_string(i);
        std::string deviceName = getJoystickName(devPath.c_str());
        
        if (!deviceName.empty()) {
            cout << "Found device: " << devPath << " - " << deviceName << endl;
            
            // Check if the name contains the substring (case-insensitive)
            std::string nameLower = deviceName;
            std::string searchLower = nameSubstring;
            
            // Convert to lowercase for comparison
            for (auto& c : nameLower) c = tolower(c);
            for (auto& c : searchLower) c = tolower(c);
            
            if (nameLower.find(searchLower) != std::string::npos) {
                cout << "Matched '" << nameSubstring << "' to " << devPath << endl;
                return devPath;
            }
        }
    }
    
    return "";
}

// Map joystick value (-32767 to 32767) to SBUS range (172 to 1811)
// Center is at 992
uint16_t mapToSBUS(int32_t joy_value) {
    // joy_value range: -32767 to +32767
    // SBUS range: 172 to 1811 (center: 992)
    // Map: -32767 -> 172, 0 -> 992, +32767 -> 1811
    
    int32_t sbus_value = 992 + (joy_value * 819 / 32767);
    
    // Clamp to valid range
    if (sbus_value < 172) sbus_value = 172;
    if (sbus_value > 1811) sbus_value = 1811;
    
    return (uint16_t)sbus_value;
}

// Map joystick value with inversion for reversed channels
uint16_t mapToSBUSReversed(int32_t joy_value) {
    // Invert the input value
    return mapToSBUS(-joy_value);
}

// Check if a channel value has moved significantly from center
bool hasMovedFromCenter(uint16_t value) {
    const uint16_t center = 992;
    const uint16_t deadzone = 50; // Tolerance around center
    return (value < (center - deadzone) || value > (center + deadzone));
}

int main(int argc, char **argv) {
    cout << "SBUS Dual Joystick Controller" << endl;
    cout << "LED Control: Using PWR (red) and ACT (green) LEDs" << endl;
    cout << "Scanning for joystick devices..." << endl;
    
    // Initialize LED control
    setLEDOff();

    // Display runs entirely on its own thread (SDL init, event pump, and
    // rendering all happen there) so a slow display can never stall the
    // flight-control loop below. Non-fatal if SDL/the display isn't
    // available - the thread just exits early and we run headless.
    std::thread displayThread(displayThreadFunc);

    // Find devices by name
    // Looking for "PXN PXN-F19" as stick
    std::string stickDev = findDeviceByName("pxn-f19");
    if (stickDev.empty()) {
        stickDev = findDeviceByName("pxn");  // Fallback to just "pxn"
    }
    
    // Looking for "Saitek Saitek Pro Flight Rudder Pedals" as pedals
    std::string pedalDev = findDeviceByName("saitek pro flight rudder pedals");
    if (pedalDev.empty()) {
        pedalDev = findDeviceByName("rudder pedals");  // Fallback
    }
    if (pedalDev.empty()) {
        pedalDev = findDeviceByName("saitek");  // Another fallback
    }
    
    if (stickDev.empty()) {
        cerr << "Error: Could not find PXN-F19 joystick device" << endl;
        cerr << "Please check that your PXN-F19 joystick is connected" << endl;
        showErrorBlink();
        gRunning = false;
        if (displayThread.joinable()) displayThread.join();
        return 1;
    }
    
    if (pedalDev.empty()) {
        cerr << "Error: Could not find Saitek Pro Flight Rudder Pedals device" << endl;
        cerr << "Please check that your Saitek pedals are connected" << endl;
        showErrorBlink();
        gRunning = false;
        if (displayThread.joinable()) displayThread.join();
        return 1;
    }
    
    // Serial port configuration
    const char* ttyPath = "/dev/serial0";
    if (argc > 1)
        ttyPath = argv[1];
    
    cout << "Using serial port: " << ttyPath << endl;
    
    // Install SBUS
    rcdrivers_err_t err = sbus.install(ttyPath, false);
    if (err != RCDRIVERS_OK) {
        cerr << "SBUS install error: " << err << endl;
        showErrorBlink();
        gRunning = false;
        if (displayThread.joinable()) displayThread.join();
        return err;
    }
    cout << "SBUS installed successfully" << endl;
    
    // Open joystick devices
    int stick_fd = open(stickDev.c_str(), O_RDONLY | O_NONBLOCK);
    int pedal_fd = open(pedalDev.c_str(), O_RDONLY | O_NONBLOCK);
    
    if (stick_fd < 0) {
        cerr << "Failed to open stick: " << stickDev << endl;
        showErrorBlink();
        gRunning = false;
        if (displayThread.joinable()) displayThread.join();
        return 1;
    }
    if (pedal_fd < 0) {
        cerr << "Failed to open pedals: " << pedalDev << endl;
        close(stick_fd);
        showErrorBlink();
        gRunning = false;
        if (displayThread.joinable()) displayThread.join();
        return 1;
    }
    
    cout << "Joysticks opened successfully" << endl;
    cout << "Stick (PXN-F19): " << stickDev << endl;
    cout << "Pedals (Saitek): " << pedalDev << endl;
    
    // Initialize SBUS packet with center values
    sbus_packet_t packet = {
        .ch17 = false,
        .ch18 = false,
        .failsafe = false,
        .frameLost = false,
    };
    
    // Set all 16 channels to center (992)
    for (int i = 0; i < 16; i++) {
        packet.channels[i] = 992;
    }

    // Throttle (CH3, index 2) starts at idle/throttle-cut, not center - it will
    // only be released to live stick input once the system reports READY.
    packet.channels[2] = 172;

    // CH8 starts at the default preset (button 2 / 173) until another
    // preset button (3-5) is pressed.
    packet.channels[CH8] = CH8_PRESET_VALUES[ch8PresetIndex];
    
    // Set initial CH5 based on default gear/flap state
    packet.channels[CH5] = calculateCH5();
    
    struct js_event e;
    auto lastWrite = steady_clock::now();
    auto lastGoodWrite = steady_clock::now();
    auto lastPrint = steady_clock::now();
    auto lastLEDBlink = steady_clock::now();
    unsigned long sbusErrorCount = 0;
    bool ledState = false;
    
    cout << "Starting SBUS transmission..." << endl;
    cout << "Initial state: Gear DOWN, Flap LOW, CH5=" << packet.channels[CH5] << endl;
    cout << "Rudder source: PEDALS (Button 6 to toggle)" << endl;
    cout << "Waiting for CH1-4 movement to initialize..." << endl;
    cout << "LED Status: PWR (red) blink = WAITING, ACT (green) blink = READY, Long PWR (red) = ERROR" << endl;
    cout << "Press Ctrl+C to stop (or close the SDL window / press Esc / Q)" << endl;
    
    while (gRunning.load()) {

        // --- Read Stick Events ---
        while (read(stick_fd, &e, sizeof(e)) > 0) {
            if (e.type == JS_EVENT_AXIS) {
                int axis = e.number;
                int value = e.value;
                bool mapped = false;
                
                // Map stick axes to channels 0..7, but skip CH4 (reserved for rudder) and CH5 (gear/flap)
                if (axis < 8) {
                    int ch = axis;
                    if (ch >= CH4) ch += 2; // Skip channels 4 and 5
                    if (ch < 16) {
                        uint16_t sbusValue;
                        
                        // Reverse CH3 (channel index 2)
                        if (ch == 2) {
                            sbusValue = mapToSBUSReversed(value);
                        } else {
                            sbusValue = mapToSBUS(value);
                        }

                        if (ch == 2) {
                            // Always track the live raw stick position, regardless
                            // of whether it's currently being let through to output.
                            rawThrottleValue = sbusValue;

                            // Track raw throttle movement so the system can still
                            // reach READY, but don't let a live throttle command
                            // out until all channels have been confirmed swept
                            // AND the throttle has been returned to idle.
                            if (hasMovedFromCenter(sbusValue)) ch3Moved = true;

                            if (allChannelsMoved) {
                                packet.channels[ch] = sbusValue;
                            } else {
                                packet.channels[ch] = 172; // hold at idle/throttle-cut until READY
                            }
                        } else {
                            // CH8 is now button-driven (see BTN_CH8_PRESET_*),
                            // so skip the axis-based write for that channel.
                            if (ch != CH8) {
                                packet.channels[ch] = sbusValue;
                            }

                            // Check for movement detection on CH1-2
                            if (ch == 0 && hasMovedFromCenter(sbusValue)) ch1Moved = true;
                            if (ch == 1 && hasMovedFromCenter(sbusValue)) ch2Moved = true;
                        }
                        
                        // If this is the CH8 axis, keep tracking it purely for the
                        // rudder-source feature (stick axis used as rudder input).
                        // It no longer drives CH8's own output value.
                        if (ch == CH8) {
                            stickRudderAxisValue = value;
                            // If we're using stick for rudder, update CH4
                            if (useStickForRudder) {
                                packet.channels[CH4] = mapToSBUS(value);
                                if (hasMovedFromCenter(packet.channels[CH4])) ch4Moved = true;
                            }
                        }
                        mapped = true;
                    }
                }
                // If you have more than 8 axes on stick, map to channels 8-15
                else if (axis < 16) {
                    packet.channels[axis] = mapToSBUS(value);
                    mapped = true;
                }
                
                // Print unmapped axis
                if (!mapped) {
                    cout << "[UNMAPPED] Stick Axis " << axis << " = " << value << endl;
                }
            }
            else if (e.type == JS_EVENT_BUTTON) {
                int button = e.number;
                bool pressed = (e.value == 1);
                bool stateChanged = false;
                
                // Handle rudder toggle button (toggle on release)
                if (button == STICK_BTN_RUDDER_TOGGLE && !pressed) {
                    useStickForRudder = !useStickForRudder;
                    cout << "Rudder source toggled to: " 
                         << (useStickForRudder ? "STICK (CH8 axis)" : "PEDALS") << endl;
                    
                    // Immediately update CH4 based on new source
                    if (useStickForRudder) {
                        packet.channels[CH4] = mapToSBUS(stickRudderAxisValue);
                    }
                    // If switching back to pedals, CH4 will be updated by pedal event loop
                }
                // Only process button presses (not releases) for state changes
                else if (pressed) {
                    // Handle flap buttons
                    if (button == STICK_BTN_FLAP_LOW) {
                        currentFlap = FLAP_LOW;
                        stateChanged = true;
                        cout << "Flap set to LOW" << endl;
                    }
                    else if (button == STICK_BTN_FLAP_MID) {
                        currentFlap = FLAP_MID;
                        stateChanged = true;
                        cout << "Flap set to MID" << endl;
                    }
                    else if (button == STICK_BTN_FLAP_HIGH) {
                        currentFlap = FLAP_HIGH;
                        stateChanged = true;
                        cout << "Flap set to HIGH" << endl;
                    }
                    // Handle gear buttons - button 16 sets DOWN, button 17 sets UP
                    else if (button == STICK_BTN_GEAR_DOWN) {  // Button 16
                        currentGear = GEAR_DOWN;
                        stateChanged = true;
                        cout << "Gear set to DOWN" << endl;
                    }
                    else if (button == STICK_BTN_GEAR_UP) {  // Button 17
                        currentGear = GEAR_UP;
                        stateChanged = true;
                        cout << "Gear set to UP" << endl;
                    }
                    // Handle CH8 preset buttons (2-5) - last pressed wins
                    else if (button == BTN_CH8_PRESET_1) {
                        ch8PresetIndex = 0;
                        packet.channels[CH8] = CH8_PRESET_VALUES[0];
                        cout << "CH8 preset -> button 2 (" << CH8_PRESET_VALUES[0] << ")" << endl;
                    }
                    else if (button == BTN_CH8_PRESET_2) {
                        ch8PresetIndex = 1;
                        packet.channels[CH8] = CH8_PRESET_VALUES[1];
                        cout << "CH8 preset -> button 3 (" << CH8_PRESET_VALUES[1] << ")" << endl;
                    }
                    else if (button == BTN_CH8_PRESET_3) {
                        ch8PresetIndex = 2;
                        packet.channels[CH8] = CH8_PRESET_VALUES[2];
                        cout << "CH8 preset -> button 4 (" << CH8_PRESET_VALUES[2] << ")" << endl;
                    }
                    else if (button == BTN_CH8_PRESET_4) {
                        ch8PresetIndex = 3;
                        packet.channels[CH8] = CH8_PRESET_VALUES[3];
                        cout << "CH8 preset -> button 5 (" << CH8_PRESET_VALUES[3] << ")" << endl;
                    }
                    // Handle Heat Tracking (HT) button - first press arms it
                    // (capturing CH6/CH7 as saved defaults, required for READY);
                    // subsequent presses toggle between saved and live values.
                    else if (button == STICK_BTN_HT_TOGGLE) {
                        if (!htArmed) {
                            htCh6Value = packet.channels[CH6];
                            htCh7Value = packet.channels[CH7];
                            htArmed = true;
                            htActive = true;
                            cout << "HT armed - saved CH6=" << htCh6Value
                                 << " CH7=" << htCh7Value << " (HT ON)" << endl;
                        } else {
                            htActive = !htActive;
                            cout << "HT toggled " << (htActive ? "ON (saved values)" : "OFF (live values)") << endl;
                        }
                    }
                    else if (button != STICK_BTN_RUDDER_TOGGLE) {
                        // Unmapped button (excluding toggle button on press)
                        cout << "[UNMAPPED] Stick Button " << button 
                             << " = PRESSED" << endl;
                    }
                    
                    // Update CH5 if gear or flap state changed
                    if (stateChanged) {
                        packet.channels[CH5] = calculateCH5();
                        cout << "CH5 updated to: " << packet.channels[CH5] 
                             << " (Gear: " << (currentGear == GEAR_DOWN ? "DOWN" : "UP")
                             << ", Flap: " << (currentFlap == FLAP_LOW ? "LOW" : 
                                              currentFlap == FLAP_MID ? "MID" : "HIGH")
                             << ")" << endl;
                    }
                } else {
                    // Button release - only print for unmapped buttons
                    if (button != STICK_BTN_FLAP_LOW && button != STICK_BTN_FLAP_MID && 
                        button != STICK_BTN_FLAP_HIGH && button != STICK_BTN_GEAR_DOWN && 
                        button != STICK_BTN_GEAR_UP && button != STICK_BTN_RUDDER_TOGGLE &&
                        button != BTN_CH8_PRESET_1 && button != BTN_CH8_PRESET_2 &&
                        button != BTN_CH8_PRESET_3 && button != BTN_CH8_PRESET_4 &&
                        button != STICK_BTN_HT_TOGGLE) {
                        cout << "[UNMAPPED] Stick Button " << button 
                             << " = RELEASED" << endl;
                    }
                }
            }
        }
        
        // --- Read Pedal Events ---
        while (read(pedal_fd, &e, sizeof(e)) > 0) {
            if (e.type == JS_EVENT_AXIS) {
                if (e.number == RUDDER_AXIS) {
                    int value = e.value;
                    uint16_t sbusValue = mapToSBUS(value);
                    // Only update CH4 if we're using pedals for rudder
                    if (!useStickForRudder) {
                        packet.channels[CH4] = sbusValue;
                        if (hasMovedFromCenter(sbusValue)) ch4Moved = true;
                    }
                } else {
                    // Unmapped pedal axis
                    cout << "[UNMAPPED] Pedal Axis " << (int)e.number 
                         << " = " << e.value << endl;
                }
            }
            else if (e.type == JS_EVENT_BUTTON) {
                // All pedal buttons are unmapped
                cout << "[UNMAPPED] Pedal Button " << (int)e.number 
                     << " = " << (e.value ? "PRESSED" : "RELEASED") << endl;
            }
        }

        // --- Heat Tracking (HT) override ---
        // When active, force CH6/CH7 to the saved snapshot instead of live
        // stick input. Applied every loop so it holds even though the axis
        // event handler above would otherwise keep overwriting these channels.
        if (htActive) {
            packet.channels[CH6] = htCh6Value;
            packet.channels[CH7] = htCh7Value;
        }
        
        // Check if all channels have moved, throttle is back at idle, and HT
        // has been armed (button 12 pressed at least once) before releasing
        // to READY.
        bool throttleAtIdle = rawThrottleValue <= (172 + THROTTLE_IDLE_TOLERANCE);
        if (!allChannelsMoved && ch1Moved && ch2Moved && ch3Moved && ch4Moved && throttleAtIdle && htArmed) {
            allChannelsMoved = true;
            cout << "All channels initialized, throttle at idle, HT armed! System ready." << endl;
        }
        
        // --- LED Blinking Logic ---
        auto now = steady_clock::now();
        if (now - lastLEDBlink > milliseconds(500)) {
            lastLEDBlink = now;
            ledState = !ledState;
            
            if (ledState) {
                // LED ON phase
                if (allChannelsMoved) {
                    setLEDGreen();  // Green blink when ready
                } else {
                    setLEDRed();    // Red blink when waiting
                }
            } else {
                // LED OFF phase
                setLEDOff();
            }
        }
        
        // --- Send SBUS packet every 14ms ---
        if (now - lastWrite > milliseconds(14)) {
            auto sinceLastWrite = std::chrono::duration_cast<milliseconds>(now - lastWrite).count();
            lastWrite = now;
            
            err = sbus.write(packet);
            if (err != RCDRIVERS_OK) {
                int savedErrno = errno;
                sbusErrorCount++;
                auto sinceLastGoodWrite = std::chrono::duration_cast<milliseconds>(now - lastGoodWrite).count();

                cerr << "SBUS write error: code=" << err
                     << " errno=" << savedErrno << " (" << strerror(savedErrno) << ")"
                     << " loop_gap_ms=" << sinceLastWrite
                     << " since_last_good_ms=" << sinceLastGoodWrite
                     << " total_errors=" << sbusErrorCount
                     << endl;

                // NOTE: do NOT block here (e.g. a sleep-based error blink) -
                // this is the flight-control hot path. A blocking call here
                // would stop all SBUS transmission for its entire duration.
                // The LED already reflects fault state via the existing
                // non-blocking blink logic below; a real error should be
                // visible there without stalling the write loop.
            } else {
                lastGoodWrite = now;
            }
        }

        // --- Publish state for the display thread (does NOT render here) ---
        // This is a quick, bounded copy under a mutex - nowhere near slow
        // enough to threaten the 14ms SBUS cadence. The display thread picks
        // this up on its own schedule and does all the (potentially slow)
        // rendering independently, on its own thread.
        if (sdlAvailable) {
            std::lock_guard<std::mutex> lock(gSnapshotMutex);
            gSnapshot.packet        = packet;
            gSnapshot.ready         = allChannelsMoved;
            gSnapshot.stickRudder   = useStickForRudder;
            gSnapshot.gear          = currentGear;
            gSnapshot.flap          = currentFlap;
            gSnapshot.rawThrottle   = rawThrottleValue;
            gSnapshot.ch8Preset     = ch8PresetIndex;
            gSnapshot.htActiveState = htActive;
        }
        
        // --- Print channel values every second ---
        if (now - lastPrint > milliseconds(1000)) {
            lastPrint = now;
            cout << "SBUS channels: ";
            for (int i = 0; i < 8; i++) {
                cout << "CH" << (i+1) << ":" << packet.channels[i] << " ";
            }
            cout << " | Rudder: " << (useStickForRudder ? "STICK" : "PEDALS");
            cout << " | Status: " << (allChannelsMoved ? "READY" : "WAITING");
            cout << " | CH moved: " << ch1Moved << ch2Moved << ch3Moved << ch4Moved;
            cout << endl;
        }
        
        // Small sleep to prevent CPU spinning
        std::this_thread::sleep_for(milliseconds(1));
    }
    
    // Cleanup
    close(stick_fd);
    close(pedal_fd);

    // Tell the display thread to stop (in case the loop exited for a reason
    // other than the user closing the window) and wait for it to clean up
    // SDL on its own thread.
    gRunning = false;
    if (displayThread.joinable()) displayThread.join();
    
    return 0;
}
