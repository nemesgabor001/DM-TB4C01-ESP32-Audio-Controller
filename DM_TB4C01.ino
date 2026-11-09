/**
 * @file DM-TB4C01_ESP32_Audio_Controller.ino
 * @brief Digital audio controller firmware for diymore DM-TB4C01 baseboard and TDA7440 chip.
 * @author [Gabor Nemes / nemesgabor001@gmail.com]
 * @version 1.0.0
 * @date 09.11.2026
 * 
 * @note Software & Library Dependencies:
 * - Arduino IDE v2.3.10
 * - Adafruit_GFX Library v1.13.0
 * - Adafruit_ST7735 and ST7789 Library v1.12.0
 * - Wire (Built-in ESP32 I2C library)
 * - Preferences (Built-in ESP32 EEPROM alternative)
 * 
 * Hardware Pinout (Verified via continuity test):
 * - ST7735 TFT Display: CS=15, DC=2, RST=4, SCLK=18, MOSI=23, BLK=32
 * - Tactile Buttons: CH=33, BAS=27, TRE=14, BAL=13, PUSH=12 (Rotary click)
 * - Rotary Encoder: Phase A=26, Phase B=25
 * - TDA7440 I2C Bus: SDA=21, SCL=22
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

int volumeDb    = -20; // Range: -80dB to 0dB
int bassDb      = 6;   // Range: -14dB to +14dB
int trebleDb    = 0;   // Range: -14dB to +14dB
int balance     = 0;   // Range: -40 to +40 (Left / Right attenuation)
int inputSource = 2;   // Range: 1 to 4 channels (IN1 - IN4)

bool isMuted = false;

// Quadrature decoder variables
volatile int encoderSteps = 0;
volatile byte encoderStatePrevious;

unsigned long lastInteractionTime = 0;
bool saveRequired = false;

// Initialize TFT Display using software SPI for custom board layout
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Preferences prefs; 

// --- HARDWARE INTERRUPT SERVICE ROUTINE (ISR) ---
// Runs instantly in the background to handle fast encoder rotation without dropping steps
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

void setup() {
  Serial.begin(115200);

  // Turn on display backlight
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);

  // Configure control buttons with internal pullups
  pinMode(PIN_CH, INPUT_PULLUP);
  pinMode(PIN_BAS, INPUT_PULLUP);
  pinMode(PIN_TRE, INPUT_PULLUP);
  pinMode(PIN_BAL, INPUT_PULLUP);
  pinMode(PIN_PUSH, INPUT_PULLUP);
  
  pinMode(ENCODER_A, INPUT_PULLUP);
  pinMode(ENCODER_B, INPUT_PULLUP);

  // Read initial encoder phase before attaching interrupts
  encoderStatePrevious = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);

  // Attach hardware interrupts to both encoder pins for precise decoding
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoderInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), encoderInterrupt, CHANGE);

  // Initialize I2C Bus (SDA: GPIO 21, SCL: GPIO 22)
  Wire.begin(21, 22);
  delay(100);

  // --- MEMORY LOAD (Restore last saved settings from non-volatile storage) ---
  prefs.begin("audio-settings", false);
  volumeDb    = prefs.getInt("volume", -20);
  bassDb      = prefs.getInt("bass", 6);
  trebleDb    = prefs.getInt("treble", 0);
  balance     = prefs.getInt("balance", 0); 
  inputSource = prefs.getInt("input", 2);
  prefs.end();

  updateAudioHardware();

  // Initialize ST7735S 160x80 TFT Display profile
  tft.initR(INITR_MINI160x80); 
  tft.invertDisplay(false); 
  tft.setRotation(1); // Landscape orientation      
  tft.fillScreen(PANEL_BLACK); 

  updateDisplayGUI();
  lastInteractionTime = millis();
}

void loop() {
  // --- BUTTON SCANNING & MODE SWITCHING ---
  if (digitalRead(PIN_PUSH) == LOW) { 
    if (currentMode != 0) { currentMode = 0; } // Quick return to Volume menu
    else { isMuted = !isMuted; updateAudioHardware(); } // Toggle Mute if already in Volume menu
    lastInteractionTime = millis(); updateDisplayGUI(); delay(250); 
  }
  if (digitalRead(PIN_BAS) == LOW)  { currentMode = 1; isMuted = false; lastInteractionTime = millis(); updateDisplayGUI(); delay(250); }
  if (digitalRead(PIN_TRE) == LOW)  { currentMode = 2; isMuted = false; lastInteractionTime = millis(); updateDisplayGUI(); delay(250); }
  if (digitalRead(PIN_BAL) == LOW)  { currentMode = 3; isMuted = false; lastInteractionTime = millis(); updateDisplayGUI(); delay(250); }
  if (digitalRead(PIN_CH) == LOW)   { currentMode = 4; isMuted = false; lastInteractionTime = millis(); updateDisplayGUI(); delay(250); }

  // --- ROTARY ENCODER PROCESSING ---
  if (abs(encoderSteps) >= 4) {
    noInterrupts(); // Protect shared variables from ISR context switching
    int localSteps = encoderSteps;
    encoderSteps = 0;
    interrupts();

    int direction = (localSteps > 0) ? 1 : -1;
    
    // Dynamic acceleration: if spun rapidly, increase stepping size for volume
    if (abs(localSteps) >= 8 && currentMode == 0) {
      direction *= 3; 
    }

    isMuted = false; // Automatically unmute on turning the dial

    // Modify selected parameter based on the active mode
    if (currentMode == 0)      volumeDb    = constrain(volumeDb + direction, -55, 30); 
    else if (currentMode == 1) bassDb      = constrain(bassDb + direction, -14, 14);
    else if (currentMode == 2) trebleDb    = constrain(trebleDb + direction, -14, 14);
    else if (currentMode == 3) balance     = constrain(balance + direction, -40, 40); 
    else if (currentMode == 4) inputSource = constrain(inputSource + direction, 1, 4);

    updateAudioHardware(); 
    updateDisplayGUI();   
    
    lastInteractionTime = millis();
    saveRequired = true;
  }

  // --- AUTOMATIC TIMEOUT RETURN TO VOLUME MENU (5 seconds of inactivity) ---
  if (currentMode != 0 && (millis() - lastInteractionTime > 5000)) {
    currentMode = 0;
    updateDisplayGUI();
  }

  // --- SMART NON-VOLATILE FLASH SAVE (Triggers after 3 seconds of inactivity) ---
  if (saveRequired && (millis() - lastInteractionTime > 3000)) {
    prefs.begin("audio-settings", false);
    prefs.putInt("volume", volumeDb);
    prefs.putInt("bass", bassDb);
    prefs.putInt("treble", trebleDb);
    prefs.putInt("balance", balance); 
    prefs.putInt("input", inputSource);
    prefs.end();
    saveRequired = false;
    Serial.println("[MEMORY] Settings successfully saved to Flash storage!");
  }
}

// Helper function for dynamic right-alignment on the text interface
void printRightAligned(String text, int max_X, int y) {
  int textWidth = text.length() * 12; // Each char is exactly 12px wide at TextSize 2
  int start_X = max_X - textWidth;
  tft.setCursor(start_X, y);
  tft.print(text);
} 

// --- REFRESH DISPLAY INTERFACE (Your Exact Custom Fine-Tuned Layout) ---
void updateDisplayGUI() {
  tft.setTextWrap(false);
  tft.setTextSize(2);
  
  // --- ROW 1: VOL, BAS, SRC ---
  // VOL / MUTE Area
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
    tft.fillRect(0, 26, 60, 16, PANEL_BLACK); // Ghost-pixel eraser box
    
    String volStr = String(volumeDb) + "dB";
    printRightAligned(volStr, 60, 26);
  }

  // BAS (Bass)
  tft.setTextColor(currentMode == 1 ? PANEL_YELLOW : PANEL_BLUE, PANEL_BLACK);
  tft.setCursor(72, 6); tft.print("BAS");
  tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
  tft.fillRect(65, 26, 50, 16, PANEL_BLACK); 
  
  String basStr = (bassDb >= 0 ? "+" : "") + String(bassDb) + "dB";
  printRightAligned(basStr, 115, 26);

  // SRC / CH (Source channel Input)
  tft.setTextColor(currentMode == 4 ? PANEL_YELLOW : PANEL_BLUE, PANEL_BLACK);
  tft.setCursor(120, 6); tft.print("SRC");
  tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
  tft.fillRect(120, 26, 40, 16, PANEL_BLACK); 
  tft.setCursor(120, 26); tft.print("IN"); tft.print(inputSource);

  // --- ROW 2: BAL, TRE ---
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
    tft.print("R +"); tft.print(balance); 
  }

  // TRE (Treble)
  tft.setTextColor(currentMode == 2 ? PANEL_YELLOW : PANEL_BLUE, PANEL_BLACK);
  tft.setCursor(110, 45); tft.print("TRE");
  tft.setTextColor(PANEL_WHITE, PANEL_BLACK);
  
  // FIXED: Eraser box adjusted perfectly to X=95 to clean any leftover +/- chars without bleeding into BAL
  tft.fillRect(95, 65, 60, 16, PANEL_BLACK); 
  
  String treStr = (trebleDb >= 0 ? "+" : "") + String(trebleDb) + "dB";
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
void updateAudioHardware() {
  int inputGain = 0;
  int finalVolAttenuation = 0;

  // Split volume range between positive Gain and negative Attenuation
  if (volumeDb > 0) {
    inputGain = volumeDb;        // 0 to +30dB range
    finalVolAttenuation = 0;     // No main attenuation needed
  } else {
    inputGain = 0;               // No positive gain needed
    finalVolAttenuation = abs(volumeDb); // 0 to 55dB attenuation range
  }

  // 1. REGISTER 0x00: Input Selector & Input Gain Stage
  // Input Gain scale: 2dB per step (Max +30dB requires a value of 15)
  byte gainVal = constrain(inputGain / 2, 0, 15);
  // Lower 3 bits control Input channel (0-3), upper bits control Input Gain
  tdaWrite(0x00, (inputSource - 1) | (gainVal << 3));

  // 2. REGISTER 0x02: Main Volume Attenuation
  if (isMuted) {
    tdaWrite(0x02, 56); // Safe maximum attenuation (Hardware Mute state)
  } else {
    // The main volume register caps at 47dB. 
    // Any remaining attenuation beyond 47dB will be offloaded to the speaker registers.
    byte volVal = constrain(finalVolAttenuation, 0, 47);
    tdaWrite(0x02, volVal);
  }

  // 3. REGISTERS 0x04 & 0x05: Left & Right Speaker Attenuation (Balance Block)
  // If the required attenuation is deeper than -47dB (e.g. -55dB), apply the rest here.
  int extraAttenuation = (finalVolAttenuation > 47) ? (finalVolAttenuation - 47) : 0;

  int attenuationL = extraAttenuation;
  int attenuationR = extraAttenuation;

  // Overlay the user-selected Left/Right balance offset
  if (balance < 0)  attenuationR += abs(balance);
  else if (balance > 0) attenuationL += abs(balance);

  tdaWrite(0x04, constrain(attenuationL, 0, 79)); // Left Speaker Attenuation
  tdaWrite(0x05, constrain(attenuationR, 0, 79)); // Right Speaker Attenuation

  // 4. REGISTERS 0x06 & 0x07: Tone Control (Bass & Treble)
  // Maps software values (-14 to +14) to the internal register scale (0x07 is flat 0dB)
  tdaWrite(0x06, 7 - (bassDb / 2));   // Register 0x06 handles Bass on TDA7440
  tdaWrite(0x07, 7 - (trebleDb / 2)); // Register 0x07 handles Treble on TDA7440
}
