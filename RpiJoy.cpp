#include <iostream>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <linux/joystick.h>
#include <fstream>
#include <string>
#include <cstring>
#include <sys/stat.h>
#include "rcdrivers/SBUS.h"

using std::cout;
using std::cerr;
using std::endl;
using std::chrono::steady_clock;
using std::chrono::milliseconds;

#define RUDDER_AXIS 2   // Saitek pedals axis 2
#define CH4 3           // SBUS channel 4 (0-based)
#define CH5 4           // SBUS channel 5 (0-based) for gear/flap combo
#define CH8 7           // SBUS channel 8 (0-based)

// Button definitions - using STICK_ prefix to avoid conflicts
#define STICK_BTN_RUDDER_TOGGLE 6
#define STICK_BTN_FLAP_LOW  13
#define STICK_BTN_FLAP_MID  14
#define STICK_BTN_FLAP_HIGH 15
#define STICK_BTN_GEAR_DOWN 16
#define STICK_BTN_GEAR_UP   17

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

// LED control functions - PWR is red, ACT is green
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
        return 1;
    }
    
    if (pedalDev.empty()) {
        cerr << "Error: Could not find Saitek Pro Flight Rudder Pedals device" << endl;
        cerr << "Please check that your Saitek pedals are connected" << endl;
        showErrorBlink();
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
        return err;
    }
    cout << "SBUS installed successfully" << endl;
    
    // Open joystick devices
    int stick_fd = open(stickDev.c_str(), O_RDONLY | O_NONBLOCK);
    int pedal_fd = open(pedalDev.c_str(), O_RDONLY | O_NONBLOCK);
    
    if (stick_fd < 0) {
        cerr << "Failed to open stick: " << stickDev << endl;
        showErrorBlink();
        return 1;
    }
    if (pedal_fd < 0) {
        cerr << "Failed to open pedals: " << pedalDev << endl;
        close(stick_fd);
        showErrorBlink();
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
    
    // Set initial CH5 based on default gear/flap state
    packet.channels[CH5] = calculateCH5();
    
    struct js_event e;
    auto lastWrite = steady_clock::now();
    auto lastPrint = steady_clock::now();
    auto lastLEDBlink = steady_clock::now();
    bool ledState = false;
    
    cout << "Starting SBUS transmission..." << endl;
    cout << "Initial state: Gear DOWN, Flap LOW, CH5=" << packet.channels[CH5] << endl;
    cout << "Rudder source: PEDALS (Button 6 to toggle)" << endl;
    cout << "Waiting for CH1-4 movement to initialize..." << endl;
    cout << "LED Status: PWR (red) blink = WAITING, ACT (green) blink = READY, Long PWR (red) = ERROR" << endl;
    cout << "Press Ctrl+C to stop" << endl;
    
    while (true) {
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
                        
                        packet.channels[ch] = sbusValue;
                        
                        // Check for movement detection on CH1-3
                        if (ch == 0 && hasMovedFromCenter(sbusValue)) ch1Moved = true;
                        if (ch == 1 && hasMovedFromCenter(sbusValue)) ch2Moved = true;
                        if (ch == 2 && hasMovedFromCenter(sbusValue)) ch3Moved = true;
                        
                        // If this is CH8, store the value for potential rudder use
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
                    // Handle gear buttons - SWAPPED: button 16 now sets UP, button 17 sets DOWN
                    else if (button == STICK_BTN_GEAR_DOWN) {  // Button 16
                        currentGear = GEAR_UP;  // REVERSED
                        stateChanged = true;
                        cout << "Gear set to UP" << endl;
                    }
                    else if (button == STICK_BTN_GEAR_UP) {  // Button 17
                        currentGear = GEAR_DOWN;  // REVERSED
                        stateChanged = true;
                        cout << "Gear set to DOWN" << endl;
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
                        button != STICK_BTN_GEAR_UP && button != STICK_BTN_RUDDER_TOGGLE) {
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
        
        // Check if all channels have moved
        if (!allChannelsMoved && ch1Moved && ch2Moved && ch3Moved && ch4Moved) {
            allChannelsMoved = true;
            cout << "All channels initialized! System ready." << endl;
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
            lastWrite = now;
            
            err = sbus.write(packet);
            if (err != RCDRIVERS_OK) {
                cerr << "SBUS write error: " << err << endl;
                showErrorBlink();
            }
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
    
    return 0;
}
