/**
 * @file DM-TB4C01_ESP32_Audio_Controller.ino
 * @brief Digital audio controller firmware for diymore DM-TB4C01 baseboard and TDA7440 chip.
 * @author [Gabor Nemes / nemesgabor001@gmail.com]
 * @version 2.0.0 (Per-Channel Audio Profiles, Soft-Ramp, Loudness & Auto-Dimmer)
 * @date 13.11.2026
 */

#include <Adafruit_GFX.h>    
#include <Adafruit_ST7735.h> 
#include <Wire.h>
#include <Preferences.h> 

#define TDA_ADDR 0x44  // I2C Address of the TDA7440 audio processor

// --- HARDWARE PIN CONFIGURATION ---
#define TFT_CS     15  
#define TFT_DC      2   
#define TFT_RST     4   
#define TFT_SCLK   18  
#define TFT_MOSI   23  
#define TFT_BLK    32  

const int PIN_CH   = 33; 
const int PIN_BAS  = 27; 
const int PIN_TRE  = 14; 
const int PIN_BAL  = 13; 
const int PIN_PUSH = 12; // Rotary encoder push button

const int ENCODER_A = 26; 
const int ENCODER_B = 25; 

// --- COLOR PALETTE (Hardware color-channel correction applied) ---
#define PANEL_BLACK        0x0000  // Clear background
#define PANEL_WHITE        0xFFFF  // Dynamic values color
#define PANEL_BLUE         0xF800  // Static labels color
#define PANEL_YELLOW       0x07E0  // Selected/Active menu highlight color
#define PANEL_RED          0x001F  // MUTE warning color

// --- SYSTEM VARIABLES ---
// State Machine Index: 0=VOL, 1=BAS, 2=TRE, 3=BAL, 4=SRC
int currentMode = 0; 

// Per-channel Audio Profiles (Index 0=IN1, 1=IN2, 2=IN3, 3=IN4)
int channelVolume[4] = {-20, -20, -20, -20}; // Range: -55dB to +30dB
int channelBass[4]   = {6, 6, 6, 6};         // Range: -14dB to +14dB
int channelTreble[4] = {0, 0, 0, 0};         // Range: -14dB to +14dB

int balance     = 0;   // Global Balance: -40 to +40 (Left / Right attenuation)
int inputSource = 2;   // Active channel: 1 to 4 (IN1 - IN4)

bool isMuted       = false;
bool isLoudnessOn  = false;

// Soft-Ramp Volume Transition variables
int currentAppliedVol = -55; // Tracks real-time hardware volume for smooth ramping

// Display Auto-Dimmer variables
const int DIM_TIMEOUT_MS = 30000; // 30 seconds until dim
const int BRIGHTNESS_HIGH = 255;  // 100% brightness
const int BRIGHTNESS_LOW  = 45;   // ~18% standby brightness
bool isDisplayDimmed = false;

// Push button long-press (1.5s hold = Loudness toggle, short press = Mute/Back)
unsigned long pushPressStart = 0;
bool pushLongHandled = false;

// Quadrature decoder variables
volatile int encoderSteps = 0;
volatile byte encoderStatePrevious;

unsigned long lastInteractionTime = 0;
bool saveRequired = false;

// Initialize TFT Display using software SPI for custom board layout
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Preferences prefs; 

// --- HARDWARE INTERRUPT SERVICE ROUTINE (ISR) ---
void IRAM_ATTR encoderInterrupt() {
  byte encoderStateCurrent = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);
  
  if (encoderStateCurrent != encoderStatePrevious) {
    if ((encoderStatePrevious == 0b00 && encoderStateCurrent == 0b01) ||
        (encoderStatePrevious == 0b01 && encoderStateCurrent == 0b11) ||
        (encoderStatePrevious == 0b11 && encoderStateCurrent == 0b10) ||
        (encoderStatePrevious == 0b10 && encoderStateCurrent == 0b00)) {
      encoderSteps++;
    } 
    else if ((encoderStatePrevious == 0b00 && encoderStateCurrent == 0b10) ||
             (encoderStatePrevious == 0b10 && encoderStateCurrent == 0b11) ||
             (encoderStatePrevious == 0b11 && encoderStateCurrent == 0b01) ||
             (encoderStatePrevious == 0b01 && encoderStateCurrent == 0b00)) {
      encoderSteps--;
    }
    encoderStatePrevious = encoderStateCurrent;
  }
}

// Helper to wake up dimmed display on any user activity
bool wakeDisplayIfDimmed() {
  lastInteractionTime = millis();
  if (isDisplayDimmed) {
    analogWrite(TFT_BLK, BRIGHTNESS_HIGH);
    isDisplayDimmed = false;
    return true; // Was dimmed, just woke up
  }
  return false;
}

void setup() {
  Serial.begin(115200);

  // Configure display backlight PWM
  pinMode(TFT_BLK, OUTPUT);
  analogWrite(TFT_BLK, BRIGHTNESS_HIGH);

  // Configure control buttons with internal pullups
  pinMode(PIN_CH, INPUT_PULLUP);
  pinMode(PIN_BAS, INPUT_PULLUP);
  pinMode(PIN_TRE, INPUT_PULLUP);
  pinMode(PIN_BAL, INPUT_PULLUP);
  pinMode(PIN_PUSH, INPUT_PULLUP);
  
  pinMode(ENCODER_A, INPUT_PULLUP);
  pinMode(ENCODER_B, INPUT_PULLUP);

  encoderStatePrevious = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);

  attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoderInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), encoderInterrupt, CHANGE);

  // Initialize I2C Bus (SDA: GPIO 21, SCL: GPIO 22)
  Wire.begin(21, 22);
  delay(100);

  // --- MEMORY LOAD (Restore Per-Channel Profiles & Global Settings) ---
  prefs.begin("audio-settings", false);
  for (int i = 0; i < 4; i++) {
    String idx = String(i + 1);
    channelVolume[i] = constrain(prefs.getInt(("vol" + idx).c_str(), -20), -55, 30);
    channelBass[i]   = constrain(prefs.getInt(("bas" + idx).c_str(), 6), -14, 14);
    channelTreble[i] = constrain(prefs.getInt(("tre" + idx).c_str(), 0), -14, 14);
  }
  balance      = constrain(prefs.getInt("balance", 0), -40, 40); 
  inputSource  = constrain(prefs.getInt("input", 2), 1, 4);
  isLoudnessOn = prefs.getBool("loudness", false);
  prefs.end();

  // Initialize ST7735S 160x80 TFT Display profile
  tft.initR(INITR_MINI160x80); 
  tft.invertDisplay(false); 
  tft.setRotation(1); // Landscape orientation      
  tft.fillScreen(PANEL_BLACK); 

  updateDisplayGUI();

  // Soft-Ramp boot fade-in from -55dB to saved channel volume
  int activeCh = constrain(inputSource - 1, 0, 3);
  currentAppliedVol = -55;
  softRampToVolume(channelVolume[activeCh]);

  lastInteractionTime = millis();
}

void loop() {
  int chIdx = constrain(inputSource - 1, 0, 3);

  // --- ROTARY PUSH BUTTON (SHORT PRESS = MUTE/BACK, 1.5s HOLD = LOUDNESS TOGGLE) ---
  bool pushPressed = (digitalRead(PIN_PUSH) == LOW);

  if (pushPressed) {
    wakeDisplayIfDimmed();
    if (pushPressStart == 0) {
      pushPressStart = millis();
      pushLongHandled = false;
    } 
    else if (!pushLongHandled && (millis() - pushPressStart >= 1500)) {
      // 1.5 second hold -> Toggle Physiological Loudness mode
      pushLongHandled = true;
      isLoudnessOn = !isLoudnessOn;
      isMuted = false;
      updateAudioHardware(channelVolume[chIdx]);
      updateDisplayGUI();
      saveRequired = true;
    }
  } else {
    if (pushPressStart > 0) {
      if (!pushLongHandled && (millis() - pushPressStart > 30)) {
        // Short press -> Return to VOL or Toggle MUTE
        if (currentMode != 0) { 
          currentMode = 0; 
        } else { 
          isMuted = !isMuted; 
          if (!isMuted) {
            // Soft ramp back from mute
            currentAppliedVol = -55;
            softRampToVolume(channelVolume[chIdx]);
          } else {
            updateAudioHardware(channelVolume[chIdx]);
          }
        }
        lastInteractionTime = millis(); 
        updateDisplayGUI(); 
      }
      pushPressStart = 0;
      pushLongHandled = false;
    }
  }

  // --- DEDICATED MENU KEYS ---
  if (digitalRead(PIN_BAS) == LOW) {
    if (!wakeDisplayIfDimmed()) { currentMode = 1; isMuted = false; updateAudioHardware(channelVolume[chIdx]); updateDisplayGUI(); }
    delay(250);
  }
  if (digitalRead(PIN_TRE) == LOW) {
    if (!wakeDisplayIfDimmed()) { currentMode = 2; isMuted = false; updateAudioHardware(channelVolume[chIdx]); updateDisplayGUI(); }
    delay(250);
  }
  if (digitalRead(PIN_BAL) == LOW) {
    if (!wakeDisplayIfDimmed()) { currentMode = 3; isMuted = false; updateAudioHardware(channelVolume[chIdx]); updateDisplayGUI(); }
    delay(250);
  }
  if (digitalRead(PIN_CH) == LOW) {
    if (!wakeDisplayIfDimmed()) { currentMode = 4; isMuted = false; updateAudioHardware(channelVolume[chIdx]); updateDisplayGUI(); }
    delay(250);
  }

  // --- ROTARY ENCODER PROCESSING ---
  if (abs(encoderSteps) >= 4) {
    noInterrupts(); 
    int localSteps = encoderSteps;
    encoderSteps = 0;
    interrupts();

    // If display was dimmed, first turn wakes it up cleanly without accidental volume jump
    if (wakeDisplayIfDimmed()) {
      return;
    }

    int direction = (localSteps > 0) ? 1 : -1;

    // Dynamic acceleration for Volume
    if (abs(localSteps) >= 8 && currentMode == 0) {
      direction *= 3; 
    }

    isMuted = false; // Automatically unmute on turning the dial

    if (currentMode == 0) {
      channelVolume[chIdx] = constrain(channelVolume[chIdx] + direction, -55, 30);
      currentAppliedVol = channelVolume[chIdx];
      updateAudioHardware(currentAppliedVol);
    } 
    else if (currentMode == 1) {
      channelBass[chIdx] = constrain(channelBass[chIdx] + direction, -14, 14);
      updateAudioHardware(channelVolume[chIdx]);
    } 
    else if (currentMode == 2) {
      channelTreble[chIdx] = constrain(channelTreble[chIdx] + direction, -14, 14);
      updateAudioHardware(channelVolume[chIdx]);
    } 
    else if (currentMode == 3) {
      balance = constrain(balance + direction, -40, 40);
      updateAudioHardware(channelVolume[chIdx]);
    } 
    else if (currentMode == 4) {
      int newSource = constrain(inputSource + direction, 1, 4);
      if (newSource != inputSource) {
        inputSource = newSource;
        int newChIdx = inputSource - 1;
        updateDisplayGUI(); // Update UI immediately with new channel's profile
        softRampToVolume(channelVolume[newChIdx]); // Smoothly transition volume to new channel
      }
    }

    if (currentMode != 4) {
      updateDisplayGUI();
    }
    
    lastInteractionTime = millis();
    saveRequired = true;
  }

  // --- AUTOMATIC TIMEOUT RETURN TO VOLUME MENU (5 seconds of inactivity) ---
  if (currentMode != 0 && (millis() - lastInteractionTime > 5000)) {
    currentMode = 0;
    updateDisplayGUI();
  }
// --- AUTOMATIC DISPLAY DIMMER (30 seconds of inactivity) ---
if (!isDisplayDimmed && (millis() - lastInteractionTime > DIM_TIMEOUT_MS)) {
analogWrite(TFT_BLK, BRIGHTNESS_LOW);
isDisplayDimmed = true;
}
// --- SMART NON-VOLATILE FLASH SAVE (Triggers after 3 seconds of inactivity) ---
if (saveRequired && (millis() - lastInteractionTime > 3000)) {
prefs.begin("audio-settings", false);
for (int i = 0; i < 4; i++) {
String idx = String(i + 1);
prefs.putInt(("vol" + idx).c_str(), channelVolume[i]);
prefs.putInt(("bas" + idx).c_str(), channelBass[i]);
prefs.putInt(("tre" + idx).c_str(), channelTreble[i]);
}
prefs.putInt("balance", balance);
prefs.putInt("input", inputSource);
prefs.putBool("loudness", isLoudnessOn);
prefs.end();
saveRequired = false;
Serial.println("[MEMORY] Per-Channel Profiles & Settings saved to Flash!");
}
}
// --- SOFT-RAMP SMOOTH VOLUME TRANSITION (~180ms max) ---
void softRampToVolume(int targetVol) {
targetVol = constrain(targetVol, -55, 30);
int diff = targetVol - currentAppliedVol;
if (diff == 0) {
updateAudioHardware(targetVol);
return;
}
int steps = min(abs(diff), 18); // Up to 18 smooth steps
int delayPerStep = 10;          // 10ms per step -> max 180ms transition
for (int s = 1; s <= steps; s++) {
int interpVol = currentAppliedVol + ((diff * s) / steps);
updateAudioHardware(interpVol);
delay(delayPerStep);
}
currentAppliedVol = targetVol;
updateAudioHardware(currentAppliedVol);
}
// Helper function for dynamic right-alignment on the text interface
void printRightAligned(String text, int max_X, int y) {
int textWidth = text.length() * 12; // Each char is exactly 12px wide at TextSize 2
int start_X = max_X - textWidth;
tft.setCursor(start_X, y);
tft.print(text);
}
// --- REFRESH DISPLAY INTERFACE ---
void updateDisplayGUI() {
int chIdx = constrain(inputSource - 1, 0, 3);
int activeVol = channelVolume[chIdx];
int activeBas = channelBass[chIdx];
int activeTre = channelTreble[chIdx];
tft.setTextWrap(false);
tft.setTextSize(2);
// --- ROW 1: VOL, BAS, SRC ---
if (isMuted) {
tft.setTextColor(PANEL_RED, PANEL_BLACK);
tft.setCursor(20, 6); tft.print("MUT");
tft.fillRect(0, 26, 60, 16, PANEL_BLACK);
tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
tft.setCursor(2, 26); tft.print("MUTE");
} else {
tft.setTextColor(currentMode == 0 ? PANEL_YELLOW : PANEL_BLUE, PANEL_BLACK);
tft.setCursor(20, 6); tft.print("VOL");
tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
tft.fillRect(0, 26, 60, 16, PANEL_BLACK);
String volStr = (activeVol > 0 ? "+" : "") + String(activeVol) + "dB";
printRightAligned(volStr, 60, 26);
}
// BAS (Per-channel Bass)
tft.setTextColor(currentMode == 1 ? PANEL_YELLOW : PANEL_BLUE, PANEL_BLACK);
tft.setCursor(72, 6); tft.print("BAS");
tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
tft.fillRect(65, 26, 50, 16, PANEL_BLACK);
String basStr = (activeBas >= 0 ? "+" : "") + String(activeBas) + "dB";
printRightAligned(basStr, 115, 26);
// SRC / CH (Source channel Input)
tft.setTextColor(currentMode == 4 ? PANEL_YELLOW : PANEL_BLUE, PANEL_BLACK);
tft.setCursor(120, 6); tft.print("SRC");
tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
tft.fillRect(120, 26, 40, 16, PANEL_BLACK);
tft.setCursor(120, 26); tft.print("IN"); tft.print(inputSource);
// --- ROW 2: BAL, LOUDNESS INDICATOR, TRE ---
// BAL (Balance)
tft.setTextColor(currentMode == 3 ? PANEL_YELLOW : PANEL_BLUE, PANEL_BLACK);
tft.setCursor(24, 45); tft.print("BAL");
tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
tft.fillRect(0, 65, 95, 16, PANEL_BLACK);
tft.setCursor(2, 65);
if (balance == 0) {
tft.print("CENTER");
} else if (balance < 0) {
tft.print("L "); tft.print(balance);
} else {
tft.print("R -"); tft.print(balance);
}
// Loudness indicator ("LDNS" in yellow between BAL and TRE when active)
  tft.fillRect(72, 45, 26, 16, PANEL_BLACK);
  if (isLoudnessOn) {
    tft.setTextSize(1);
    tft.setTextColor(PANEL_YELLOW, PANEL_BLACK);
    tft.setCursor(73, 49);
    tft.print("LDNS");
    tft.setTextSize(2); 
  }
// TRE (Per-channel Treble)
tft.setTextColor(currentMode == 2 ? PANEL_YELLOW : PANEL_BLUE, PANEL_BLACK);
tft.setCursor(110, 45); tft.print("TRE");
tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
tft.fillRect(95, 65, 60, 16, PANEL_BLACK);
String treStr = (activeTre >= 0 ? "+" : "") + String(activeTre) + "dB";
printRightAligned(treStr, 155, 65);
}
// --- LOW-LEVEL I2C COMMUNICATION TO TDA7440 ---
void tdaWrite(byte reg, byte val) {
Wire.beginTransmission(TDA_ADDR);
Wire.write(reg);
Wire.write(val);
Wire.endTransmission();
}
// --- SYNC STATE TO TDA7440 AUDIO PROCESSOR HARDWARE ---
void updateAudioHardware(int targetVol) {
int chIdx = constrain(inputSource - 1, 0, 3);
// 1. REGISTER 0x00: Input Selector & Input Gain Stage (0 to +30dB)
byte gainVal = (targetVol > 0) ? constrain(targetVol / 2, 0, 15) : 0;
tdaWrite(0x00, chIdx | (gainVal << 3));
// 2. REGISTER 0x02: Main Volume Attenuation (0 to 55dB attenuation)
int finalVolAttenuation = (targetVol < 0) ? abs(targetVol) : 0;
if (isMuted) {
tdaWrite(0x02, 56); // Safe maximum attenuation (Hardware Mute state)
} else {
byte volVal = constrain(finalVolAttenuation, 0, 47);
tdaWrite(0x02, volVal);
}
// 3. REGISTERS 0x04 & 0x05: Left & Right Speaker Attenuation (Balance Block)
int extraAttenuation = (finalVolAttenuation > 47) ? (finalVolAttenuation - 47) : 0;
int attenuationL = extraAttenuation;
int attenuationR = extraAttenuation;
if (balance < 0)  attenuationR += abs(balance);
else if (balance > 0) attenuationL += abs(balance);
tdaWrite(0x04, constrain(attenuationL, 0, 79)); // Left Speaker Attenuation
tdaWrite(0x05, constrain(attenuationR, 0, 79)); // Right Speaker Attenuation
// 4. REGISTERS 0x06 & 0x07: Tone Control (Per-Channel Bass & Treble + Loudness Boost)
int effectiveBass = channelBass[chIdx];
int effectiveTreble = channelTreble[chIdx];
if (isLoudnessOn && targetVol < -15) {
// Gradually boosts up to +4dB Bass and +2dB Treble as volume drops from -15dB to -45dB
int loudFactor = constrain(abs(targetVol + 15), 0, 30);
effectiveBass   = constrain(effectiveBass + ((loudFactor * 4) / 30), -14, 14);
effectiveTreble = constrain(effectiveTreble + ((loudFactor * 2) / 30), -14, 14);
}
tdaWrite(0x06, 7 - (effectiveBass / 2));   // Register 0x06 handles Bass on TDA7440
tdaWrite(0x07, 7 - (effectiveTreble / 2)); // Register 0x07 handles Treble on TDA7440
}
