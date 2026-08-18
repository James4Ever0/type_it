/*
 * type_it — Macro Keyboard Firmware for RP2040 Zero
 *
 * A hardware port of ../main.py's profile/candidate macro keyboard.
 *
 * Hardware:
 *   - RP2040 Zero
 *   - 1.3" SH1106 128x64 I2C OLED
 *   - EC11 rotary encoder (TRA=A, TRB=B, PSH=push switch)
 *   - CON (KEY1) confirm button
 *   - BAK (KEYO) back button
 *
 * UX:
 *   - Encoder turn                    -> scroll highlight
 *   - Encoder short press / CON        -> open profile, select menu item, or type candidate
 *   - Encoder long press / BAK         -> go back; from Profiles opens the master menu
 *   - CON mirrors the encoder short press; BAK mirrors the encoder long press
 *   - Master menu (long press from Profiles):
 *       Profiles, Diagnostic, Settings, Stats, About, Help
 *   - Last-used ordering is persisted across reboots
 *
 * Serial commands (115200 baud, CRLF or LF terminated):
 *   SETTEXT:profile <<EOF     replace profile with a block of text
 *   APPENDTEXT:profile <<EOF  append a block of text to profile
 *   APPEND:profile text       append a single line to profile
 *   SETLINE:profile:n text    set/replace candidate at line n (1-based)
 *   ADDPROFILE:profile        create a new empty profile
 *   DELPROFILE:profile        delete a profile
 *   RENAME:old new            rename a profile
 *   DELLINE:profile:n         delete candidate at line n
 *   LIST                      list all profiles
 *   VIEW:profile              view profile contents
 *   INFO                      show version and state
 *   SETOS:lin|mac|win         set host OS for Unicode typing
 *   GETOS                     show current host OS
 *   DIAG                      enter interactive diagnostic mode
 *   HELP                      command summary
 *   RESET                     factory reset
 *
 * Heredoc-style markers are optional; if omitted, the literal string "EOF"
 * ends the upload (only useful for interactive terminals).
 */

#include <Keyboard.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>

// ==================== Pin assignments (RP2040 Zero GPIO) ====================
#define PIN_SDA           4   // OLED SDA  (I2C0)
#define PIN_SCL           5   // OLED SCL  (I2C0)
#define PIN_ENC_A         3   // EC11 A phase (TRA)
#define PIN_ENC_B         2   // EC11 B phase (TRB)
#define PIN_ENC_SW        6   // EC11 push switch (PSH)
#define PIN_CON           14 // Confirm button (KEY1) — optional, set to -1 to disable
#define PIN_BAK          15   // Back button (KEYO) — optional, set to -1 to disable

// ==================== Constants ====================
#define FW_VERSION        0x02
#define EEPROM_SIZE       512
#define SERIAL_BAUD       115200
#define SERIAL_TIMEOUT_MS 1500

#define LONG_PRESS_MS     600
#define DEBOUNCE_MS       25
#define FLASH_MS          800
#define KNOB_STEP_MS      120   // minimum ms between highlight moves via encoder

#define ENCODER_PULSES_PER_DETENT 4
#define MAX_PROFILE_NAME  32
#define MAX_LINE_LEN      256
#define MAX_VISIBLE       3
#define HEADER_H          14
#define LINE_H            15
#define SCREEN_W          128
#define SCREEN_H          64

#define VERBOSE_SERIAL    1       // set to 0 to disable runtime Serial logging

#if VERBOSE_SERIAL
  #define DBG(x) do { if (VERBOSE_SERIAL) Serial.println(x); } while(0)
#else
  #define DBG(x)
#endif

// CJK font. Largest single-font Simplified Chinese coverage available in u8g2.
// Requires u8g2.drawUTF8() / u8g2.getUTF8Width() and u8g2.enableUTF8Print().
#define CJK_FONT u8g2_font_wqy12_t_gb2312

#define PROFILES_DIR      "/profiles"
#define USAGE_PATH        "/usage.dat"

// ==================== EEPROM layout ====================
#define ADDR_MAGIC        0
#define ADDR_VIEW         1
#define ADDR_PROF_IDX_LO  2
#define ADDR_PROF_IDX_HI  3
#define ADDR_CAND_IDX_LO  4
#define ADDR_CAND_IDX_HI  5
#define ADDR_OS_MODE      6
#define ADDR_RESERVED     7

// ==================== U8g2 display object ====================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ==================== Data model ====================
struct Candidate {
  String text;
  uint32_t order;
};

struct Profile {
  String name;
  uint32_t order;
  std::vector<Candidate> candidates;
};

struct UsageEntry {
  char type;      // 'P' or 'C'
  String key;     // profile name OR "profile::candidate"
  uint32_t order;
};

// ==================== Runtime state ====================
std::vector<Profile> profiles;
std::vector<UsageEntry> usage;

String view = "profiles";       // "profiles" | "candidates" | "master" | "settings" | "stats" | "about" | "help"
int profileIndex = 0;
int candidateIndex = 0;
int masterIndex = 0;
int settingsIndex = 0;
int statsScroll = 0;
int helpScroll = 0;

// Scroll window offsets for cursor-based list scrolling
int profileListOffset = 0;
int candidateListOffset = 0;
int masterListOffset = 0;
int settingsListOffset = 0;
int statsListOffset = 0;
int helpListOffset = 0;

uint32_t nextOrder = 1;
bool needsRedraw = true;

// Host OS mode for Unicode typing
enum OSMode { OS_LIN = 0, OS_MAC = 1, OS_WIN = 2 };
const char* OSModeNames[] = {"lin", "mac", "win"};
OSMode osMode = OS_LIN;

// Diagnostic mode state
enum DiagState {
  DIAG_INTRO,
  DIAG_DISPLAY_TEST,
  DIAG_ENCODER_TEST,
  DIAG_BUTTON_TEST,
  DIAG_OS_TEST,
  DIAG_FILESYSTEM_TEST,
  DIAG_EEPROM_TEST,
  DIAG_FONT_TEST,
  DIAG_REPORT
};

struct DiagResult {
  String name;
  char status;      // 'P' = PASS, 'F' = FAIL, 'U' = UNKNOWN
  String detail;
};

bool inDiagnosticMode = false;
DiagState diagState = DIAG_INTRO;
int diagSelection = 0;                  // 0 = YES/CONTINUE, 1 = NO/SKIP

std::vector<DiagResult> diagResults;
std::vector<String> diagReportLines;
int diagReportScroll = 0;
int diagReportOffset = 0;

// Transient per-test state
int diagEncoderDetected = 0;            // +1 CW, -1 CCW, 0 none
uint8_t diagButtonStage = 0;            // 0=encoder switch, 1=CON, 2=BAK
bool diagButtonPhase = false;           // false=await press, true=confirm works
bool diagButtonConsumed = false;        // suppress short-press handling when press consumed as test input
bool diagButtonAnyFail = false;         // aggregate result across button test stages
uint8_t diagOsSubState = 0;             // 0=ask whether to type, 1=ask whether it typed correctly
bool diagFsOk = false;
bool diagEepromOk = false;

// Encoder state
volatile int encoderDelta = 0;
volatile uint8_t encoderState = 0;
const int8_t encoderTable[16] = {
  0, -1, 1, 0,
  1, 0, 0, -1,
  -1, 0, 0, 1,
  0, 1, -1, 0
};

// Button state
bool encPressed = false;
uint32_t encPressStart = 0;
bool longTriggered = false;

// Flash overlay
uint32_t flashUntil = 0;
String flashText = "";

// Upload state
enum UploadMode { UPLOAD_NONE, UPLOAD_SETTEXT, UPLOAD_APPENDTEXT };
UploadMode uploadMode = UPLOAD_NONE;
String uploadProfile = "";
String uploadMarker = "";
String uploadBuffer = "";

// ==================== Forward declarations ====================
void loadUsage();
void saveUsage();
uint32_t touchProfile(const String &name);
uint32_t touchCandidate(const String &profileName, const String &text);
uint32_t getProfileOrder(const String &name);
uint32_t getCandidateOrder(const String &profileName, const String &text);
void loadProfiles();
void loadCandidates(Profile &prof);
void saveProfileFile(const String &name, const String &content);
String readProfileFile(const String &name);
void appendProfileFile(const String &name, const String &content);
void deleteProfileFile(const String &name);
void renameProfileFile(const String &oldName, const String &newName);
bool profileExists(const String &name);
bool isValidProfileName(const String &name);
void splitLines(const String &content, std::vector<String> &out);
String joinLines(const std::vector<String> &lines);
void trimAndDedupLines(std::vector<String> &lines);

void draw();
void drawHeader(const String &title);
void drawList(const std::vector<String> &items, int selected, int &offset);
void flash(const String &text);

void handleSerial();
void processCommand(const String &cmd);
void beginUpload(UploadMode mode, const String &profile, const String &marker);
void handleUploadLine(const String &line);
bool isValidMarker(const String &marker);

void onEncoder();
void readButtons();
void moveHighlight(int delta);
void doShortPress();
void doBack();
void enterMasterMenu();
void handleMenuOrBack();
std::vector<String> buildStatsLines();
std::vector<String> buildHelpLines();
void typeCurrentCandidate();

void saveUIState();
void restoreUIState();
void initStorage();

// OS mode helpers
bool parseOSMode(const String &s, OSMode &out);
const char* osModeName(OSMode mode);

// UTF-8 utilities
bool utf8DecodeNext(const String &s, unsigned &idx, uint32_t &outCodePoint);
String utf8EncodeCodePoint(uint32_t cp);
unsigned utf8CodePointCount(const String &s);
String utf8SubstringByChars(const String &s, unsigned charStart, unsigned charCount);
String utf8CropChars(const String &s, unsigned maxChars);
String utf8CropPixels(const String &s, u8g2_uint_t maxWidth);

// Unicode typing
String toHexLower(uint32_t value, int digits);
void typeCodePoint(uint32_t cp);
void typeUnicode(const String &text);

// Diagnostic mode
void enterDiagnosticMode();
void exitDiagnosticMode();
void diagGotoState(DiagState next);
void diagHandleEncoder(int steps);
void diagHandleShortPress();
void diagHandleLongPress();
void diagCaptureButtonPress(int id);
void diagRecordResult(const String &name, char status, const String &detail = "");
void diagRunFilesystemTest();
void diagRunEepromTest();
void diagBuildReport();
void diagPrintReport();
void drawDiagnostic();

// ==================== Setup ====================
void setup() {
  // Pin setup
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  if (PIN_CON >= 0) pinMode(PIN_CON, INPUT_PULLUP);
  if (PIN_BAK >= 0) pinMode(PIN_BAK, INPUT_PULLUP);

  // Initialize encoder state so the first transition is decoded correctly.
  encoderState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);

  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), onEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), onEncoder, CHANGE);

  // Display
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  u8g2.setBusClock(400000);
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setFont(CJK_FONT);
  u8g2.setContrast(128);

  // Filesystem
  if (!LittleFS.begin()) {
    DBG("[FS] LittleFS.begin() failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
  DBG("[FS] LittleFS started");

  // EEPROM
  EEPROM.begin(EEPROM_SIZE);
  initStorage();

  // Keyboard
  Keyboard.begin();

  // Serial
  Serial.begin(SERIAL_BAUD);
  unsigned long serialTimeout = millis() + SERIAL_TIMEOUT_MS;
  while (!Serial && millis() < serialTimeout) { delay(10); }

  loadUsage();
  loadProfiles();
  restoreUIState();

  Serial.println("=== type_it macro keyboard ===");
  Serial.println("Profiles: " + String(profiles.size()));
  Serial.println("OS mode: " + String(osModeName(osMode)));
  Serial.println("Send HELP for commands.");
}

// ==================== Main loop ====================
void loop() {
  handleSerial();
  readButtons();

  // Encoder navigation
  noInterrupts();
  int delta = encoderDelta;
  encoderDelta = 0;
  interrupts();

  if (inDiagnosticMode) {
    static int diagEncoderAccum = 0;
    static uint32_t diagLastStepMs = 0;
    if (delta != 0) {
      diagEncoderAccum += delta;
      uint32_t now = millis();
      if (abs(diagEncoderAccum) >= ENCODER_PULSES_PER_DETENT && now - diagLastStepMs >= KNOB_STEP_MS) {
        int steps = diagEncoderAccum / ENCODER_PULSES_PER_DETENT;
        diagEncoderAccum -= steps * ENCODER_PULSES_PER_DETENT;
        diagHandleEncoder(steps);
        diagLastStepMs = now;
      }
    }
  } else {
    static int encoderAccum = 0;
    static uint32_t lastStepMs = 0;
    if (delta != 0) {
      encoderAccum += delta;
      uint32_t now = millis();
      if (abs(encoderAccum) >= ENCODER_PULSES_PER_DETENT && now - lastStepMs >= KNOB_STEP_MS) {
        int steps = encoderAccum / ENCODER_PULSES_PER_DETENT;
        encoderAccum -= steps * ENCODER_PULSES_PER_DETENT;
        moveHighlight(steps);
        lastStepMs = now;
      }
    }
  }

  // Draw at ~30 FPS so flash overlays expire cleanly
  static uint32_t lastDrawMs = 0;
  uint32_t now = millis();
  if (needsRedraw || now - lastDrawMs >= 33) {
    draw();
    lastDrawMs = now;
    needsRedraw = false;
  }

  delay(1);
}

// ==================== Encoder ISR ====================
void onEncoder() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  uint8_t state = (a << 1) | b;
  uint8_t idx = (encoderState << 2) | state;
  encoderDelta += encoderTable[idx];
  encoderState = state;
}

// ==================== Button handling ====================
void readButtons() {
  uint32_t now = millis();

  // Encoder switch (active low)
  static bool lastEnc = HIGH;
  static uint32_t lastEncChange = 0;
  bool curEnc = digitalRead(PIN_ENC_SW);
  if (curEnc != lastEnc && now - lastEncChange > DEBOUNCE_MS) {
    lastEnc = curEnc;
    lastEncChange = now;
    if (curEnc == LOW) {
      encPressed = true;
      encPressStart = now;
      longTriggered = false;
      diagButtonConsumed = false;
      if (inDiagnosticMode && diagState == DIAG_BUTTON_TEST && !diagButtonPhase && diagButtonStage == 0) {
        diagCaptureButtonPress(0);
        diagButtonConsumed = true;
      }
      DBG("[BTN] encoder press");
    } else {
      if (encPressed && !longTriggered && now - encPressStart < LONG_PRESS_MS) {
        if (diagButtonConsumed) {
          diagButtonConsumed = false;
          DBG("[BTN] encoder release (consumed)");
        } else if (inDiagnosticMode) {
          DBG("[BTN] encoder release (short)");
          diagHandleShortPress();
        } else {
          DBG("[BTN] encoder release (short)");
          doShortPress();
        }
      } else if (encPressed && longTriggered) {
        DBG("[BTN] encoder release (after long)");
      }
      encPressed = false;
    }
  }

  // Long press detection for encoder switch
  if (encPressed && !longTriggered && (millis() - encPressStart >= LONG_PRESS_MS)) {
    longTriggered = true;
    DBG("[BTN] encoder long press");
    handleMenuOrBack();
  }

  // CON button
  if (PIN_CON >= 0) {
    static bool lastCon = HIGH;
    static uint32_t lastConChange = 0;
    bool curCon = digitalRead(PIN_CON);
    if (curCon != lastCon && now - lastConChange > DEBOUNCE_MS) {
      lastCon = curCon;
      lastConChange = now;
      if (curCon == LOW) {
        DBG("[BTN] CON press");
        if (inDiagnosticMode && diagState == DIAG_BUTTON_TEST && !diagButtonPhase && diagButtonStage == 1) {
          diagCaptureButtonPress(1);
        } else if (inDiagnosticMode) {
          diagHandleShortPress();
        } else {
          doShortPress();
        }
      }
    }
  }

  // BAK button — acts like a long press of the encoder switch: fires
  // handleMenuOrBack() immediately on press. In the diagnostic button test
  // it is captured as the BAK key being tested.
  if (PIN_BAK >= 0) {
    static bool lastBak = HIGH;
    static uint32_t lastBakChange = 0;
    bool curBak = digitalRead(PIN_BAK);
    if (curBak != lastBak && now - lastBakChange > DEBOUNCE_MS) {
      lastBak = curBak;
      lastBakChange = now;
      if (curBak == LOW) {
        DBG("[BTN] BAK press (menu/back)");
        if (inDiagnosticMode && diagState == DIAG_BUTTON_TEST && !diagButtonPhase && diagButtonStage == 2) {
          diagCaptureButtonPress(2);
        } else {
          handleMenuOrBack();
        }
      }
    }
  }
}

// ==================== Navigation actions ====================
void moveHighlight(int delta) {
  if (delta == 0) return;
  if (view == "profiles") {
    if (profiles.empty()) return;
    profileIndex = (profileIndex + delta + profiles.size()) % profiles.size();
    candidateIndex = 0;
    DBG("[UI] scroll profiles delta=" + String(delta) + " idx=" + String(profileIndex));
  } else if (view == "candidates") {
    if (profileIndex < 0 || profileIndex >= (int)profiles.size()) return;
    auto &cands = profiles[profileIndex].candidates;
    if (cands.empty()) return;
    candidateIndex = (candidateIndex + delta + cands.size()) % cands.size();
    DBG("[UI] scroll candidates delta=" + String(delta) + " idx=" + String(candidateIndex));
  } else if (view == "master") {
    const int MASTER_ITEMS = 6;
    masterIndex = (masterIndex + delta + MASTER_ITEMS) % MASTER_ITEMS;
    DBG("[UI] scroll master delta=" + String(delta) + " idx=" + String(masterIndex));
  } else if (view == "settings") {
    const int OS_ITEMS = 3;
    settingsIndex = (settingsIndex + delta + OS_ITEMS) % OS_ITEMS;
    DBG("[UI] scroll settings delta=" + String(delta) + " idx=" + String(settingsIndex));
  } else if (view == "stats") {
    std::vector<String> lines = buildStatsLines();
    int maxScroll = max(0, (int)lines.size() - 1);
    statsScroll = constrain(statsScroll + delta, 0, maxScroll);
    DBG("[UI] scroll stats delta=" + String(delta) + " idx=" + String(statsScroll));
  } else if (view == "help") {
    std::vector<String> lines = buildHelpLines();
    int maxScroll = max(0, (int)lines.size() - 1);
    helpScroll = constrain(helpScroll + delta, 0, maxScroll);
    DBG("[UI] scroll help delta=" + String(delta) + " idx=" + String(helpScroll));
  } else if (view == "about") {
    // static page, no scrolling
  }
  saveUIState();
  needsRedraw = true;
}

void doShortPress() {
  if (view == "profiles") {
    if (profileIndex >= 0 && profileIndex < (int)profiles.size()) {
      String name = profiles[profileIndex].name;
      DBG("[UI] short press: opening profile '" + name + "'");
      touchProfile(name);
      loadProfiles();              // re-sort
      profileIndex = 0;
      for (size_t i = 0; i < profiles.size(); i++) {
        if (profiles[i].name == name) {
          profileIndex = i;
          break;
        }
      }
      view = "candidates";
      loadCandidates(profiles[profileIndex]);  // ensure candidates are loaded now that view is candidates
      candidateIndex = 0;
      saveUIState();
      DBG("[UI] entered candidates view for '" + name + "', " + String(profiles[profileIndex].candidates.size()) + " item(s)");
      needsRedraw = true;
    }
  } else if (view == "candidates") {
    typeCurrentCandidate();
  } else if (view == "master") {
    switch (masterIndex) {
      case 0:
        view = "profiles";
        DBG("[UI] master: Profiles");
        break;
      case 1:
        DBG("[UI] master: Diagnostic");
        enterDiagnosticMode();
        return;  // diagnostic mode takes over; don't touch view/needsRedraw here
      case 2:
        view = "settings";
        settingsIndex = (int)osMode;
        DBG("[UI] master: Settings");
        break;
      case 3:
        view = "stats";
        statsScroll = 0;
        DBG("[UI] master: Stats");
        break;
      case 4:
        view = "about";
        DBG("[UI] master: About");
        break;
      case 5:
        view = "help";
        helpScroll = 0;
        DBG("[UI] master: Help");
        break;
    }
    saveUIState();
    needsRedraw = true;
  } else if (view == "settings") {
    OSMode newMode = (OSMode)settingsIndex;
    if (newMode != osMode) {
      osMode = newMode;
      saveUIState();
      flash("OS: " + String(osModeName(osMode)));
    }
    view = "master";
    saveUIState();
    needsRedraw = true;
  } else if (view == "stats" || view == "about" || view == "help") {
    view = "master";
    saveUIState();
    needsRedraw = true;
  }
}

void doBack() {
  if (view == "candidates") {
    view = "profiles";
    candidateIndex = 0;
    DBG("[UI] back: returning to profiles");
  } else if (view == "settings" || view == "stats" || view == "about" || view == "help") {
    view = "master";
    DBG("[UI] back: returning to master");
  } else if (view == "master") {
    view = "profiles";
    masterIndex = 0;
    DBG("[UI] back: returning to profiles from master");
  }
  saveUIState();
  needsRedraw = true;
}

void enterMasterMenu() {
  view = "master";
  saveUIState();
  needsRedraw = true;
  DBG("[UI] entered master menu");
}

// The action bound to an encoder long press. CON/BAK buttons map onto the
// encoder behaviors: CON = encoder short press (doShortPress), BAK = encoder
// long press (this function). From Profiles it opens the master menu, from
// elsewhere it goes back; during diagnostics it aborts.
void handleMenuOrBack() {
  if (inDiagnosticMode) {
    diagHandleLongPress();
  } else if (view == "profiles") {
    enterMasterMenu();
  } else {
    doBack();
  }
}

size_t computeTxtBytesInDir(const String &path) {
  size_t total = 0;
  File dir = LittleFS.open(path.c_str(), "r");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return total;
  }
  File f = dir.openNextFile();
  while (f) {
    String name = f.name();
    if (!name.startsWith("/")) name = path + (path.endsWith("/") ? "" : "/") + name;
    if (name.endsWith(".txt")) {
      total += f.size();
    } else if (f.isDirectory()) {
      total += computeTxtBytesInDir(name);
    }
    f.close();
    f = dir.openNextFile();
  }
  return total;
}

size_t computeStoredBytes() {
  size_t total = 0;
  if (LittleFS.exists(USAGE_PATH)) {
    File f = LittleFS.open(USAGE_PATH, "r");
    if (f) {
      total += f.size();
      f.close();
    }
  }
  total += computeTxtBytesInDir("/");
  return total;
}

std::vector<String> buildStatsLines() {
  std::vector<String> lines;
  lines.push_back("OS: " + String(osModeName(osMode)));
  lines.push_back("Profiles: " + String(profiles.size()));
  for (auto &p : profiles) {
    loadCandidates(p);
    lines.push_back("  " + p.name + " (" + String(p.candidates.size()) + ")");
  }
  lines.push_back("Stored: " + String(computeStoredBytes()) + " bytes");
  return lines;
}

std::vector<String> buildHelpLines() {
  std::vector<String> lines;
  lines.push_back("1. Connect device to");
  lines.push_back("computer via USB.");
  lines.push_back("2. Open a serial term-");
  lines.push_back("inal at " + String(SERIAL_BAUD) + " baud.");
  lines.push_back("3. Type HELP.");
  return lines;
}

void typeCurrentCandidate() {
  if (profileIndex < 0 || profileIndex >= (int)profiles.size()) return;
  auto &prof = profiles[profileIndex];
  if (candidateIndex < 0 || candidateIndex >= (int)prof.candidates.size()) return;
  auto &cand = prof.candidates[candidateIndex];

  // Capture identifiers before any re-sorting invalidates references.
  String profileName = prof.name;
  String candText = cand.text;

  DBG("[TYPE] typing candidate '" + utf8SafeByteCrop(candText, 32) + "' from profile '" + profileName + "'");

  typeUnicode(candText);

  uint32_t now = touchCandidate(profileName, candText);
  touchProfile(profileName);
  cand.order = now;
  prof.order = now;

  // Re-sort candidates and profiles in-place
  std::sort(prof.candidates.begin(), prof.candidates.end(), [](const Candidate &a, const Candidate &b) {
    return a.order > b.order;
  });
  candidateIndex = 0;
  for (size_t i = 0; i < prof.candidates.size(); i++) {
    if (prof.candidates[i].text == candText) {
      candidateIndex = i;
      break;
    }
  }

  std::sort(profiles.begin(), profiles.end(), [](const Profile &a, const Profile &b) {
    return a.order > b.order;
  });
  profileIndex = 0;
  for (size_t i = 0; i < profiles.size(); i++) {
    if (profiles[i].name == profileName) {
      profileIndex = i;
      break;
    }
  }

  saveUIState();
  flash("Sent: " + utf8CropChars(candText, 8));
  DBG("[TYPE] done; new profile idx=" + String(profileIndex) + " cand idx=" + String(candidateIndex));
  needsRedraw = true;
}

// ==================== Profile / candidate storage ====================
void loadProfiles() {
  profiles.clear();
  if (!LittleFS.exists(PROFILES_DIR)) {
    DBG("[FS] profiles dir missing, creating '" + String(PROFILES_DIR) + "'");
    LittleFS.mkdir(PROFILES_DIR);
  }

  File dir = LittleFS.open(PROFILES_DIR, "r");
  if (!dir || !dir.isDirectory()) {
    DBG("[FS] WARN could not open profiles dir");
    return;
  }

  DBG("[FS] scanning profiles dir");
  File f = dir.openNextFile();
  int found = 0;
  while (f) {
    String fname = f.name();
    f.close();
    if (fname.endsWith(".txt")) {
      String name = fname.substring(0, fname.length() - 4);
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (name.length() > 0 && name.length() <= MAX_PROFILE_NAME) {
        Profile p;
        p.name = name;
        p.order = getProfileOrder(name);
        profiles.push_back(p);
        found++;
      }
    }
    f = dir.openNextFile();
  }
  DBG("[FS] loaded " + String(found) + " profile file(s)");

  // Sort by most recently used
  std::sort(profiles.begin(), profiles.end(), [](const Profile &a, const Profile &b) {
    return a.order > b.order;
  });

  // Load candidates for the current profile if in candidate view
  if (view == "candidates" && profileIndex >= 0 && profileIndex < (int)profiles.size()) {
    DBG("[FS] view=candidates, loading candidates for profile idx=" + String(profileIndex));
    loadCandidates(profiles[profileIndex]);
    candidateIndex = constrain(candidateIndex, 0, max(0, (int)profiles[profileIndex].candidates.size() - 1));
  }
}

void loadCandidates(Profile &prof) {
  prof.candidates.clear();
  String path = String(PROFILES_DIR) + "/" + prof.name + ".txt";
  DBG("[FS] loading candidates from '" + path + "'");
  File f = LittleFS.open(path, "r");
  if (!f) {
    DBG("[FS] WARN could not open '" + path + "'");
    return;
  }

  String line = "";
  int count = 0;
  while (f.available()) {
    char c = f.read();
    if (c == '\n') {
      line.trim();
      if (line.length() > 0 && line.length() <= MAX_LINE_LEN) {
        Candidate cand;
        cand.text = line;
        cand.order = getCandidateOrder(prof.name, line);
        prof.candidates.push_back(cand);
        count++;
      }
      line = "";
    } else if (c != '\r') {
      line += c;
    }
  }
  f.close();

  // Handle last line if file doesn't end with newline
  line.trim();
  if (line.length() > 0 && line.length() <= MAX_LINE_LEN) {
    Candidate cand;
    cand.text = line;
    cand.order = getCandidateOrder(prof.name, line);
    prof.candidates.push_back(cand);
    count++;
  }

  // Deduplicate: keep first occurrence, preserve highest usage order.
  std::vector<Candidate> deduped;
  for (const auto &c : prof.candidates) {
    bool dup = false;
    for (auto &d : deduped) {
      if (d.text == c.text) {
        dup = true;
        if (c.order > d.order) d.order = c.order;
        break;
      }
    }
    if (!dup) deduped.push_back(c);
  }
  prof.candidates = deduped;

  std::sort(prof.candidates.begin(), prof.candidates.end(), [](const Candidate &a, const Candidate &b) {
    return a.order > b.order;
  });
  DBG("[FS] loaded " + String(count) + " candidate(s) for '" + prof.name + "'");
}

String readProfileFile(const String &name) {
  String path = String(PROFILES_DIR) + "/" + name + ".txt";
  File f = LittleFS.open(path, "r");
  if (!f) {
    DBG("[FS] readProfileFile '" + path + "' FAILED");
    return "";
  }
  String out = "";
  out.reserve(f.size());
  while (f.available()) {
    out += (char)f.read();
  }
  f.close();
  DBG("[FS] readProfileFile '" + path + "' " + String(out.length()) + " byte(s)");
  return out;
}

void saveProfileFile(const String &name, const String &content) {
  if (!LittleFS.exists(PROFILES_DIR)) {
    LittleFS.mkdir(PROFILES_DIR);
  }
  String path = String(PROFILES_DIR) + "/" + name + ".txt";
  File f = LittleFS.open(path, "w");
  if (!f) {
    DBG("[FS] saveProfileFile '" + path + "' FAILED to open");
    return;
  }
  String out = content;
  if (out.length() == 0 || out[out.length() - 1] != '\n') {
    out += '\n';
  }
  size_t written = f.print(out);
  f.close();
  DBG("[FS] saveProfileFile '" + path + "' wrote " + String(written) + "/" + String(out.length()) + " byte(s)");
}

void appendProfileFile(const String &name, const String &content) {
  if (!LittleFS.exists(PROFILES_DIR)) {
    LittleFS.mkdir(PROFILES_DIR);
  }
  String path = String(PROFILES_DIR) + "/" + name + ".txt";
  File f = LittleFS.open(path, "a");
  if (!f) {
    DBG("[FS] appendProfileFile '" + path + "' FAILED to open");
    return;
  }
  String out = content;
  if (out.length() == 0 || out[out.length() - 1] != '\n') {
    out += '\n';
  }
  size_t written = f.print(out);
  f.close();
  DBG("[FS] appendProfileFile '" + path + "' wrote " + String(written) + "/" + String(out.length()) + " byte(s)");
}

void deleteProfileFile(const String &name) {
  String path = String(PROFILES_DIR) + "/" + name + ".txt";
  if (LittleFS.exists(path)) {
    bool ok = LittleFS.remove(path);
    DBG("[FS] deleteProfileFile '" + path + "' ok=" + String(ok));
  } else {
    DBG("[FS] deleteProfileFile '" + path + "' not found");
  }
  // Remove usage entries for this profile
  usage.erase(std::remove_if(usage.begin(), usage.end(), [&](const UsageEntry &e) {
    if (e.type == 'P') return e.key == name;
    if (e.type == 'C') return e.key.startsWith(name + "::");
    return false;
  }), usage.end());
  saveUsage();
}

void renameProfileFile(const String &oldName, const String &newName) {
  String oldPath = String(PROFILES_DIR) + "/" + oldName + ".txt";
  String newPath = String(PROFILES_DIR) + "/" + newName + ".txt";
  if (!LittleFS.exists(oldPath)) {
    DBG("[FS] renameProfileFile source '" + oldPath + "' not found");
    return;
  }
  if (LittleFS.exists(newPath)) {
    DBG("[FS] renameProfileFile target exists, removing '" + newPath + "'");
    LittleFS.remove(newPath);
  }
  bool ok = LittleFS.rename(oldPath, newPath);
  DBG("[FS] renameProfileFile '" + oldPath + "' -> '" + newPath + "' ok=" + String(ok));

  // Update usage keys
  for (auto &e : usage) {
    if (e.type == 'P' && e.key == oldName) {
      e.key = newName;
    } else if (e.type == 'C' && e.key.startsWith(oldName + "::")) {
      e.key = newName + "::" + e.key.substring(oldName.length() + 2);
    }
  }
  saveUsage();
}

bool profileExists(const String &name) {
  bool exists = LittleFS.exists(String(PROFILES_DIR) + "/" + name + ".txt");
  DBG("[FS] profileExists '" + name + "' = " + String(exists));
  return exists;
}

// ==================== Usage store (binary file) ====================
void loadUsage() {
  usage.clear();
  nextOrder = 1;

  File f = LittleFS.open(USAGE_PATH, "r");
  if (!f) {
    DBG("[FS] loadUsage '" + String(USAGE_PATH) + "' not found, starting fresh");
    return;
  }

  char magic[4];
  if (f.readBytes(magic, 4) < 4 || strncmp(magic, "TYPE", 4) != 0) {
    f.close();
    DBG("[FS] loadUsage bad magic, resetting usage");
    return;
  }

  while (f.available() >= 6) {
    char type = f.read();
    uint8_t len = f.read();
    if (f.available() < len + 4) break;

    String key;
    key.reserve(len);
    for (int i = 0; i < len; i++) {
      key += (char)f.read();
    }

    uint8_t o[4];
    f.readBytes((char *)o, 4);
    uint32_t order = ((uint32_t)o[0]) | ((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);

    usage.push_back({type, key, order});
    if (order >= nextOrder) nextOrder = order + 1;
  }
  f.close();
  DBG("[FS] loadUsage entries=" + String(usage.size()) + " nextOrder=" + String(nextOrder));
}

void saveUsage() {
  File f = LittleFS.open(USAGE_PATH, "w");
  if (!f) {
    DBG("[FS] saveUsage FAILED to open '" + String(USAGE_PATH) + "'");
    return;
  }

  size_t written = f.write((const uint8_t *)"TYPE", 4);
  for (const auto &e : usage) {
    uint8_t len = min((size_t)255, e.key.length());
    written += f.write((uint8_t)e.type);
    written += f.write(len);
    written += f.write((const uint8_t *)e.key.c_str(), len);
    uint8_t o[4] = {
      (uint8_t)(e.order & 0xFF),
      (uint8_t)((e.order >> 8) & 0xFF),
      (uint8_t)((e.order >> 16) & 0xFF),
      (uint8_t)((e.order >> 24) & 0xFF)
    };
    written += f.write(o, 4);
  }
  f.close();
  DBG("[FS] saveUsage entries=" + String(usage.size()) + " bytes=" + String(written));
}

uint32_t touchProfile(const String &name) {
  uint32_t order = nextOrder++;
  for (auto &e : usage) {
    if (e.type == 'P' && e.key == name) {
      e.order = order;
      saveUsage();
      DBG("[USAGE] touchProfile '" + name + "' order=" + String(order));
      return order;
    }
  }
  usage.push_back({'P', name, order});
  saveUsage();
  DBG("[USAGE] newProfile '" + name + "' order=" + String(order));
  return order;
}

uint32_t touchCandidate(const String &profileName, const String &text) {
  String key = profileName + "::" + text;
  uint32_t order = nextOrder++;
  for (auto &e : usage) {
    if (e.type == 'C' && e.key == key) {
      e.order = order;
      saveUsage();
      DBG("[USAGE] touchCandidate '" + utf8SafeByteCrop(key, 48) + "' order=" + String(order));
      return order;
    }
  }
  usage.push_back({'C', key, order});
  saveUsage();
  DBG("[USAGE] newCandidate '" + utf8SafeByteCrop(key, 48) + "' order=" + String(order));
  return order;
}

uint32_t getProfileOrder(const String &name) {
  for (const auto &e : usage) {
    if (e.type == 'P' && e.key == name) return e.order;
  }
  return 0;
}

uint32_t getCandidateOrder(const String &profileName, const String &text) {
  String key = profileName + "::" + text;
  for (const auto &e : usage) {
    if (e.type == 'C' && e.key == key) return e.order;
  }
  return 0;
}

// ==================== EEPROM helpers ====================
void initStorage() {
  uint8_t magic = EEPROM.read(ADDR_MAGIC);
  if (magic != FW_VERSION) {
    for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
    EEPROM.write(ADDR_MAGIC, FW_VERSION);
    EEPROM.commit();
  }
}

void saveUIState() {
  uint8_t v = 0;
  if (view == "candidates") v = 1;
  else if (view == "master") v = 2;
  else if (view == "settings") v = 3;
  else if (view == "stats") v = 4;
  else if (view == "about") v = 5;
  else if (view == "help") v = 6;
  EEPROM.write(ADDR_VIEW, v);
  EEPROM.write(ADDR_PROF_IDX_LO, profileIndex & 0xFF);
  EEPROM.write(ADDR_PROF_IDX_HI, (profileIndex >> 8) & 0xFF);
  EEPROM.write(ADDR_CAND_IDX_LO, candidateIndex & 0xFF);
  EEPROM.write(ADDR_CAND_IDX_HI, (candidateIndex >> 8) & 0xFF);
  EEPROM.write(ADDR_OS_MODE, (uint8_t)osMode);
  EEPROM.commit();
}

void restoreUIState() {
  uint8_t v = EEPROM.read(ADDR_VIEW);
  int savedProf = EEPROM.read(ADDR_PROF_IDX_LO) | (EEPROM.read(ADDR_PROF_IDX_HI) << 8);
  int savedCand = EEPROM.read(ADDR_CAND_IDX_LO) | (EEPROM.read(ADDR_CAND_IDX_HI) << 8);
  uint8_t savedOs = EEPROM.read(ADDR_OS_MODE);

  if (v == 1) view = "candidates";
  else if (v == 2) view = "master";
  else if (v == 3) view = "settings";
  else if (v == 4) view = "stats";
  else if (v == 5) view = "about";
  else if (v == 6) view = "help";
  else view = "profiles";

  profileIndex = constrain(savedProf, 0, max(0, (int)profiles.size() - 1));
  candidateIndex = 0;
  osMode = (savedOs <= OS_WIN) ? (OSMode)savedOs : OS_LIN;

  if (view == "candidates" && profileIndex >= 0 && profileIndex < (int)profiles.size()) {
    loadCandidates(profiles[profileIndex]);
    candidateIndex = constrain(savedCand, 0, max(0, (int)profiles[profileIndex].candidates.size() - 1));
  } else if (view == "settings") {
    settingsIndex = (int)osMode;
  } else if (view == "stats") {
    statsScroll = 0;
  } else if (view == "help") {
    helpScroll = 0;
  }
}

const char* osModeName(OSMode mode) {
  switch (mode) {
    case OS_MAC: return "mac";
    case OS_WIN: return "win";
    default: return "lin";
  }
}

bool parseOSMode(const String &s, OSMode &out) {
  String lower = s;
  lower.toLowerCase();
  if (lower == "lin" || lower == "linux") { out = OS_LIN; return true; }
  if (lower == "mac" || lower == "macos" || lower == "osx") { out = OS_MAC; return true; }
  if (lower == "win" || lower == "windows") { out = OS_WIN; return true; }
  return false;
}

// ==================== Display drawing ====================
void drawCenteredUTF8(int y, const String &text) {
  int w = u8g2.getUTF8Width(text.c_str());
  int x = (SCREEN_W - w) / 2;
  if (x < 0) x = 0;
  u8g2.drawUTF8(x, y, text.c_str());
}

void draw() {
  u8g2.clearBuffer();

  if (inDiagnosticMode) {
    drawDiagnostic();
    u8g2.sendBuffer();
    return;
  }

  if (view == "master") {
    drawHeader("Menu");
    std::vector<String> items = {"Profiles", "Diagnostic", "Settings", "Stats", "About", "Help"};
    drawList(items, masterIndex, masterListOffset);
  } else if (view == "settings") {
    drawHeader("OS Setting");
    std::vector<String> items;
    for (int i = 0; i < 3; i++) {
      items.push_back(String(OSModeNames[i]) + (i == (int)osMode ? " *" : ""));
    }
    drawList(items, settingsIndex, settingsListOffset);
  } else if (view == "stats") {
    drawHeader("Stats");
    std::vector<String> items = buildStatsLines();
    drawList(items, statsScroll, statsListOffset);
  } else if (view == "help") {
    drawHeader("Help");
    std::vector<String> items = buildHelpLines();
    drawList(items, helpScroll, helpListOffset);
  } else if (view == "about") {
    u8g2.setFont(CJK_FONT);
    drawCenteredUTF8(10, "Type it v"+ String(FW_VERSION) );
    drawCenteredUTF8(22, "unicode visual");
    drawCenteredUTF8(34, "macro keyboard");
    drawCenteredUTF8(46, "by: James Brown | 白雨");
    drawCenteredUTF8(58, "github.com/james4ever0");
  } else {
    String title;
    if (view == "profiles") {
      title = "Profiles";
    } else {
      title = (profileIndex >= 0 && profileIndex < (int)profiles.size()) ? profiles[profileIndex].name : "";
    }
    drawHeader(title);

    std::vector<String> items;
    int selected = 0;
    if (view == "profiles") {
      for (const auto &p : profiles) items.push_back(p.name);
      selected = profileIndex;
    } else if (profileIndex >= 0 && profileIndex < (int)profiles.size()) {
      for (const auto &c : profiles[profileIndex].candidates) items.push_back(c.text);
      selected = candidateIndex;
    }

    drawList(items, selected, (view == "profiles") ? profileListOffset : candidateListOffset);
  }

  // Flash overlay
  if (millis() < flashUntil && flashText.length() > 0) {
    u8g2.setDrawColor(0);
    u8g2.drawBox(0, SCREEN_H / 2 - 10, SCREEN_W, 20);
    u8g2.setDrawColor(1);
    u8g2.drawFrame(0, SCREEN_H / 2 - 10, SCREEN_W, 20);
    u8g2.setFont(CJK_FONT);
    u8g2.drawUTF8(4, SCREEN_H / 2 + 4, flashText.c_str());
    u8g2.setFont(CJK_FONT);
  }

  u8g2.sendBuffer();
}

void drawHeader(const String &title) {
  u8g2.setFont(CJK_FONT);
  u8g2.drawUTF8(0, 12, title.c_str());
  u8g2.drawHLine(0, HEADER_H - 1, SCREEN_W);
}

void drawList(const std::vector<String> &items, int selected, int &offset) {
  u8g2.setFont(CJK_FONT);
  int y0 = HEADER_H + 2;
  int maxVisible = (SCREEN_H - y0) / LINE_H;
  if (maxVisible < 1) maxVisible = 1;

  if (items.empty()) {
    u8g2.drawUTF8(0, y0 + 10, "(empty)");
    return;
  }

  // Cursor-based scrolling: move the highlight within the window until it
  // reaches an edge, then scroll the window.
  if (selected < offset) {
    offset = selected;
  } else if (selected >= offset + maxVisible) {
    offset = selected - maxVisible + 1;
  }
  int maxOffset = max(0, (int)items.size() - maxVisible);
  offset = constrain(offset, 0, maxOffset);

  int start = offset;
  int end = min((int)items.size(), start + maxVisible);

  for (int i = start; i < end; i++) {
    int y = y0 + (i - start) * LINE_H + 11;
    String text = utf8CropPixels(items[i], SCREEN_W - 4);

    if (i == selected) {
      u8g2.setDrawColor(1);
      u8g2.drawBox(0, y0 + (i - start) * LINE_H, SCREEN_W, LINE_H);
      u8g2.setDrawColor(0);
    } else {
      u8g2.setDrawColor(1);
    }
    u8g2.drawUTF8(2, y, text.c_str());
    u8g2.setDrawColor(1);
  }
}

void flash(const String &text) {
  flashText = text;
  flashUntil = millis() + FLASH_MS;
}

// ==================== Serial command handling ====================
void handleSerial() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();

  if (uploadMode != UPLOAD_NONE) {
    handleUploadLine(line);
    return;
  }

  processCommand(line);
}

void processCommand(const String &cmd) {
  if (cmd.length() == 0) return;

  if (cmd.startsWith("SETTEXT:")) {
    String rest = cmd.substring(8);
    rest.trim();
    int markerIdx = rest.indexOf(" <<");
    String profile;
    String marker;
    if (markerIdx < 0) {
      profile = rest;
      marker = "EOF";
    } else {
      profile = rest.substring(0, markerIdx);
      marker = rest.substring(markerIdx + 3);
      marker.trim();
    }
    profile.trim();
    DBG("[CMD] SETTEXT profile='" + profile + "' marker='" + marker + "'");
    if (!isValidProfileName(profile)) {
      Serial.println("ERR invalid profile name");
      return;
    }
    if (!isValidMarker(marker)) {
      Serial.println("ERR invalid marker");
      return;
    }
    beginUpload(UPLOAD_SETTEXT, profile, marker);
  }
  else if (cmd.startsWith("APPENDTEXT:")) {
    String rest = cmd.substring(11);
    rest.trim();
    int markerIdx = rest.indexOf(" <<");
    String profile;
    String marker;
    if (markerIdx < 0) {
      profile = rest;
      marker = "EOF";
    } else {
      profile = rest.substring(0, markerIdx);
      marker = rest.substring(markerIdx + 3);
      marker.trim();
    }
    profile.trim();
    DBG("[CMD] APPENDTEXT profile='" + profile + "' marker='" + marker + "'");
    if (!isValidProfileName(profile)) {
      Serial.println("ERR invalid profile name");
      return;
    }
    if (!isValidMarker(marker)) {
      Serial.println("ERR invalid marker");
      return;
    }
    beginUpload(UPLOAD_APPENDTEXT, profile, marker);
  }
  else if (cmd.startsWith("APPEND:")) {
    String rest = cmd.substring(7);
    rest.trim();
    int spaceIdx = rest.indexOf(' ');
    String profile, text;
    if (spaceIdx < 0) {
      profile = rest;
      text = "";
    } else {
      profile = rest.substring(0, spaceIdx);
      text = rest.substring(spaceIdx + 1);
    }
    profile.trim();
    DBG("[CMD] APPEND profile='" + profile + "' text='" + utf8SafeByteCrop(text, 40) + "'");
    if (!isValidProfileName(profile)) {
      Serial.println("ERR invalid profile name");
      return;
    }
    if (!profileExists(profile)) {
      saveProfileFile(profile, "");
    }
    text.trim();
    if (text.length() == 0) {
      Serial.println("ERR empty text");
      return;
    }

    std::vector<String> lines;
    splitLines(readProfileFile(profile), lines);
    bool exists = false;
    for (const auto &l : lines) {
      String t = l;
      t.trim();
      if (t == text) { exists = true; break; }
    }
    if (exists) {
      Serial.println("OK duplicate skipped in " + profile);
    } else {
      appendProfileFile(profile, text + "\n");
      Serial.println("OK appended to " + profile);
    }
    loadProfiles();
  }
  else if (cmd.startsWith("SETLINE:")) {
    String rest = cmd.substring(8);
    rest.trim();
    int firstColon = rest.indexOf(':');
    if (firstColon < 0) {
      Serial.println("ERR use SETLINE:profile:n text");
      return;
    }
    String profile = rest.substring(0, firstColon);
    rest = rest.substring(firstColon + 1);
    rest.trim();

    int spaceIdx = rest.indexOf(' ');
    int lineNum;
    String text;
    if (spaceIdx < 0) {
      lineNum = rest.toInt();
      text = "";
    } else {
      lineNum = rest.substring(0, spaceIdx).toInt();
      text = rest.substring(spaceIdx + 1);
    }

    if (!isValidProfileName(profile)) {
      Serial.println("ERR invalid profile name");
      return;
    }
    if (lineNum < 1) {
      Serial.println("ERR line number must be >= 1");
      return;
    }

    DBG("[CMD] SETLINE profile='" + profile + "' line=" + String(lineNum) + " text='" + utf8SafeByteCrop(text, 40) + "'");
    String content = readProfileFile(profile);
    std::vector<String> lines;
    splitLines(content, lines);

    while ((int)lines.size() < lineNum) {
      lines.push_back("");
    }
    lines[lineNum - 1] = text;
    trimAndDedupLines(lines);

    saveProfileFile(profile, joinLines(lines));
    Serial.println("OK " + profile + " line " + String(lineNum) + " set");
    loadProfiles();
  }
  else if (cmd.startsWith("ADDPROFILE:")) {
    String profile = cmd.substring(11);
    profile.trim();
    DBG("[CMD] ADDPROFILE '" + profile + "'");
    if (!isValidProfileName(profile)) {
      Serial.println("ERR invalid profile name");
      return;
    }
    if (profileExists(profile)) {
      Serial.println("ERR profile already exists");
      return;
    }
    saveProfileFile(profile, "");
    Serial.println("OK created " + profile);
    loadProfiles();
  }
  else if (cmd.startsWith("DELPROFILE:")) {
    String profile = cmd.substring(11);
    profile.trim();
    DBG("[CMD] DELPROFILE '" + profile + "'");
    if (!profileExists(profile)) {
      Serial.println("ERR profile not found");
      return;
    }
    deleteProfileFile(profile);
    Serial.println("OK deleted " + profile);
    if (view == "candidates" && profileIndex >= 0 && profileIndex < (int)profiles.size()
        && profiles[profileIndex].name == profile) {
      view = "profiles";
      candidateIndex = 0;
    }
    loadProfiles();
    profileIndex = constrain(profileIndex, 0, max(0, (int)profiles.size() - 1));
    saveUIState();
  }
  else if (cmd.startsWith("RENAME:")) {
    String rest = cmd.substring(7);
    rest.trim();
    int spaceIdx = rest.indexOf(' ');
    if (spaceIdx < 0) {
      Serial.println("ERR use RENAME:old new");
      return;
    }
    String oldName = rest.substring(0, spaceIdx);
    String newName = rest.substring(spaceIdx + 1);
    oldName.trim();
    newName.trim();
    DBG("[CMD] RENAME '" + oldName + "' -> '" + newName + "'");
    if (!isValidProfileName(oldName) || !isValidProfileName(newName)) {
      Serial.println("ERR invalid profile name");
      return;
    }
    if (!profileExists(oldName)) {
      Serial.println("ERR source profile not found");
      return;
    }
    if (profileExists(newName)) {
      Serial.println("ERR target profile already exists");
      return;
    }
    renameProfileFile(oldName, newName);
    Serial.println("OK renamed " + oldName + " -> " + newName);
    loadProfiles();
  }
  else if (cmd.startsWith("DELLINE:")) {
    String rest = cmd.substring(8);
    rest.trim();
    int colonIdx = rest.lastIndexOf(':');
    if (colonIdx < 0) {
      Serial.println("ERR use DELLINE:profile:n");
      return;
    }
    String profile = rest.substring(0, colonIdx);
    int lineNum = rest.substring(colonIdx + 1).toInt();
    DBG("[CMD] DELLINE profile='" + profile + "' line=" + String(lineNum));
    if (!profileExists(profile)) {
      Serial.println("ERR profile not found");
      return;
    }
    String content = readProfileFile(profile);
    std::vector<String> lines;
    splitLines(content, lines);
    if (lineNum < 1 || lineNum > (int)lines.size()) {
      Serial.println("ERR line out of range");
      return;
    }
    lines.erase(lines.begin() + (lineNum - 1));
    saveProfileFile(profile, joinLines(lines));
    Serial.println("OK deleted line " + String(lineNum) + " from " + profile);
    loadProfiles();
  }
  else if (cmd == "LIST") {
    Serial.println("Profiles:");
    for (auto &p : profiles) {
      loadCandidates(p);  // ensure count is accurate (loadProfiles only loads the active profile)
      Serial.println("  " + p.name + " (" + String(p.candidates.size()) + " items)");
    }
  }
  else if (cmd.startsWith("VIEW:")) {
    String profile = cmd.substring(5);
    profile.trim();
    if (!profileExists(profile)) {
      Serial.println("ERR profile not found");
      return;
    }
    String content = readProfileFile(profile);
    Serial.println("--- " + profile + " ---");
    std::vector<String> lines;
    splitLines(content, lines);
    for (size_t i = 0; i < lines.size(); i++) {
      Serial.print("[");
      Serial.print(i + 1);
      Serial.print("] ");
      Serial.println(lines[i]);
    }
    Serial.println("---");
  }
  else if (cmd.startsWith("SETOS:")) {
    String arg = cmd.substring(6);
    arg.trim();
    OSMode parsed;
    if (!parseOSMode(arg, parsed)) {
      Serial.println("ERR use SETOS:lin|mac|win");
      return;
    }
    osMode = parsed;
    saveUIState();
    Serial.println(String("OK os=") + osModeName(osMode));
    DBG("[CMD] SETOS set to " + String(osModeName(osMode)));
  }
  else if (cmd == "GETOS") {
    Serial.println(String("os=") + osModeName(osMode));
  }
  else if (cmd == "INFO") {
    Serial.print("type_it v");
    Serial.println(FW_VERSION);
    Serial.print("profiles=");
    Serial.println(profiles.size());
    Serial.print("view=");
    Serial.println(view);
    Serial.print("profileIndex=");
    Serial.println(profileIndex);
    Serial.print("candidateIndex=");
    Serial.println(candidateIndex);
    Serial.print("os=");
    Serial.println(osModeName(osMode));
  }
  else if (cmd == "UNICODE") {
    Serial.println("Linux: IBus Ctrl+Shift+U or Ctrl+Shift+U hex Enter.");
    Serial.println("macOS: add Unicode Hex Input in Settings.");
    Serial.println("Windows: reg add EnableHexNumpad=1, then Alt+plus+hex.");
  }
  else if (cmd == "HELP") {
    Serial.println("Commands:");
    Serial.println("  SETTEXT:profile <<EOF      replace profile with multiline text");
    Serial.println("  APPENDTEXT:profile <<EOF   append multiline text to profile");
    Serial.println("  APPEND:profile text        append single line");
    Serial.println("  SETLINE:profile:n text     set candidate at line n (1-based)");
    Serial.println("  ADDPROFILE:profile         create new empty profile");
    Serial.println("  DELPROFILE:profile         delete profile");
    Serial.println("  RENAME:old new             rename profile");
    Serial.println("  DELLINE:profile:n          delete candidate at line n");
    Serial.println("  LIST                       list profiles");
    Serial.println("  VIEW:profile               show profile contents");
    Serial.println("  UNICODE                    unicode input setup info");
    Serial.println("  INFO                       firmware state");
    Serial.println("  SETOS:lin|mac|win          set host OS for Unicode typing");
    Serial.println("  GETOS                      show current host OS");
    Serial.println("  DIAG                       enter interactive diagnostic mode");
    Serial.println("  HELP                       this help");
    Serial.println("  RESET                      erase all profiles and state");
  }
  else if (cmd == "RESET") {
    DBG("[CMD] RESET factory reset");
    LittleFS.format();
    usage.clear();
    profiles.clear();
    for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
    EEPROM.write(ADDR_MAGIC, FW_VERSION);
    EEPROM.commit();
    view = "profiles";
    profileIndex = 0;
    candidateIndex = 0;
    osMode = OS_LIN;
    saveUIState();
    loadProfiles();
    Serial.println("OK reset");
  }
  else if (cmd == "DIAG") {
    DBG("[CMD] DIAG entering diagnostic mode");
    enterDiagnosticMode();
    Serial.println("OK entering diagnostic mode");
  }
  else {
    Serial.println("ERR unknown command (try HELP)");
  }

  needsRedraw = true;
}

// ==================== Multiline upload ====================
bool isValidMarker(const String &marker) {
  if (marker.length() == 0 || marker.length() > 32) return false;
  for (unsigned i = 0; i < marker.length(); i++) {
    if (marker[i] == ' ') return false;
  }
  return true;
}

bool isValidProfileName(const String &name) {
  if (name.length() == 0 || name.length() > MAX_PROFILE_NAME) return false;
  for (unsigned i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

void beginUpload(UploadMode mode, const String &profile, const String &marker) {
  uploadMode = mode;
  uploadProfile = profile;
  uploadMarker = marker;
  uploadBuffer = "";
  Serial.println("OK send text, end with " + marker);
  Serial.println(">>");
}

void handleUploadLine(const String &line) {
  if (line == uploadMarker) {
    UploadMode mode = uploadMode;
    uploadMode = UPLOAD_NONE;

    // Trim trailing newline so an empty final line does not add a blank line
    while (uploadBuffer.length() > 0 && uploadBuffer[uploadBuffer.length() - 1] == '\n') {
      uploadBuffer.remove(uploadBuffer.length() - 1);
    }

    DBG("[UPLOAD] marker matched, buffer=" + String(uploadBuffer.length()) + " bytes, mode=" + String(mode == UPLOAD_SETTEXT ? "SET" : "APPEND"));
    if (mode == UPLOAD_SETTEXT) {
      std::vector<String> lines;
      splitLines(uploadBuffer, lines);
      trimAndDedupLines(lines);
      String cleaned = joinLines(lines);
      saveProfileFile(uploadProfile, cleaned);
      Serial.println("OK replaced " + uploadProfile + " (" + String(lines.size()) + " entries, " + String(cleaned.length()) + " bytes)");
    } else {
      std::vector<String> lines;
      splitLines(readProfileFile(uploadProfile), lines);
      std::vector<String> newLines;
      splitLines(uploadBuffer, newLines);
      for (const auto &nl : newLines) lines.push_back(nl);
      trimAndDedupLines(lines);
      String cleaned = joinLines(lines);
      saveProfileFile(uploadProfile, cleaned);
      Serial.println("OK appended to " + uploadProfile + " (" + String(lines.size()) + " entries, " + String(cleaned.length()) + " bytes)");
    }

    uploadBuffer = "";
    loadProfiles();
    needsRedraw = true;
    return;
  }

  uploadBuffer += line;
  uploadBuffer += "\n";
}

// ==================== String helpers ====================
void splitLines(const String &content, std::vector<String> &out) {
  out.clear();
  String line = "";
  for (unsigned i = 0; i < content.length(); i++) {
    char c = content[i];
    if (c == '\n') {
      out.push_back(line);
      line = "";
    } else if (c != '\r') {
      line += c;
    }
  }
  if (line.length() > 0) out.push_back(line);
}

String joinLines(const std::vector<String> &lines) {
  String out = "";
  for (size_t i = 0; i < lines.size(); i++) {
    out += lines[i];
    out += "\n";
  }
  return out;
}

// Trim, drop empty lines, and remove duplicates (case-sensitive, keep first occurrence).
void trimAndDedupLines(std::vector<String> &lines) {
  std::vector<String> out;
  out.reserve(lines.size());
  for (const auto &line : lines) {
    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() == 0) continue;
    bool dup = false;
    for (const auto &existing : out) {
      if (existing == trimmed) { dup = true; break; }
    }
    if (!dup) out.push_back(trimmed);
  }
  lines = out;
}

// ==================== UTF-8 helpers ====================

bool utf8DecodeNext(const String &s, unsigned &idx, uint32_t &outCodePoint) {
  if (idx >= s.length()) return false;
  uint8_t b0 = (uint8_t)s[idx];
  if (b0 < 0x80) {
    outCodePoint = b0;
    idx++;
    return true;
  }
  uint32_t cp = 0;
  unsigned len = 0;
  if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; len = 2; }
  else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; len = 3; }
  else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; len = 4; }
  else { idx++; outCodePoint = 0xFFFD; return true; } // invalid leading byte, skip

  if (idx + len > s.length()) { idx = s.length(); outCodePoint = 0xFFFD; return true; }
  for (unsigned i = 1; i < len; i++) {
    uint8_t b = (uint8_t)s[idx + i];
    if ((b & 0xC0) != 0x80) { idx += i; outCodePoint = 0xFFFD; return true; }
    cp = (cp << 6) | (b & 0x3F);
  }
  idx += len;
  outCodePoint = cp;
  return true;
}

String utf8EncodeCodePoint(uint32_t cp) {
  String out;
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
  return out;
}

unsigned utf8CodePointCount(const String &s) {
  unsigned count = 0;
  unsigned idx = 0;
  uint32_t cp;
  while (utf8DecodeNext(s, idx, cp)) count++;
  return count;
}

String utf8SubstringByChars(const String &s, unsigned charStart, unsigned charCount) {
  String out;
  unsigned idx = 0;
  unsigned current = 0;
  uint32_t cp;
  while (utf8DecodeNext(s, idx, cp)) {
    if (current >= charStart + charCount) break;
    if (current >= charStart) out += utf8EncodeCodePoint(cp);
    current++;
  }
  return out;
}

String utf8CropChars(const String &s, unsigned maxChars) {
  if (maxChars == 0) return "";
  String out;
  unsigned idx = 0;
  unsigned count = 0;
  uint32_t cp;
  while (utf8DecodeNext(s, idx, cp)) {
    if (count >= maxChars) break;
    out += utf8EncodeCodePoint(cp);
    count++;
  }
  return out;
}

String utf8CropPixels(const String &s, u8g2_uint_t maxWidth) {
  String out;
  unsigned idx = 0;
  uint32_t cp;
  while (utf8DecodeNext(s, idx, cp)) {
    String trial = out + utf8EncodeCodePoint(cp);
    if (u8g2.getUTF8Width(trial.c_str()) > (int)maxWidth) break;
    out = trial;
  }
  return out;
}

String utf8SafeByteCrop(const String &s, unsigned maxBytes) {
  if (maxBytes >= s.length()) return s;
  unsigned i = maxBytes;
  while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
  return s.substring(0, i);
}

// ==================== Unicode typing ====================

String toHexLower(uint32_t value, int digits) {
  String out;
  out.reserve(digits);
  for (int i = digits - 1; i >= 0; i--) {
    uint8_t nibble = (value >> (i * 4)) & 0x0F;
    out += (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
  }
  return out;
}

void typeCodePoint(uint32_t cp) {
  if (cp < 0x80) {
    // ASCII can be typed directly on all host OSes.
    Keyboard.print((char)cp);
    return;
  }

  String hex = toHexLower(cp, (cp <= 0xFFFF) ? 4 : 6);

  switch (osMode) {
    case OS_LIN: {
      Keyboard.press(KEY_LEFT_CTRL);
      Keyboard.press(KEY_LEFT_SHIFT);
      Keyboard.press('u');
      Keyboard.releaseAll();
      delay(25);
      Keyboard.print(hex);
      delay(15);
      Keyboard.press(' ');
      Keyboard.releaseAll();
      delay(25);
      break;
    }
    case OS_MAC: {
      Keyboard.press(KEY_LEFT_ALT);
      Keyboard.print(hex);
      Keyboard.releaseAll();
      delay(40);
      break;
    }
    case OS_WIN: {
      // Windows Unicode hex entry: hold Left Alt, press numpad-plus, type hex, release Alt.
      // Standard Keyboard.h lacks explicit numpad keys, so we use the main-keyboard '+'.
      // This requires the host registry key EnableHexNumpad and may need a real numpad-plus on some systems.
      Keyboard.press(KEY_LEFT_ALT);
      Keyboard.press('+');
      Keyboard.release('+');
      delay(15);
      Keyboard.print(hex);
      Keyboard.releaseAll();
      delay(40);
      break;
    }
  }
}

void typeUnicode(const String &text) {
  DBG("[TYPE] typing " + String(text.length()) + " byte(s) as " + osModeName(osMode));
  unsigned idx = 0;
  uint32_t cp;
  while (utf8DecodeNext(text, idx, cp)) {
    typeCodePoint(cp);
  }
}

// ==================== Diagnostic mode ====================

void enterDiagnosticMode() {
  inDiagnosticMode = true;
  diagState = DIAG_INTRO;
  diagSelection = 0;
  diagResults.clear();
  diagReportLines.clear();
  diagReportScroll = 0;
  diagEncoderDetected = 0;
  diagButtonStage = 0;
  diagButtonPhase = false;
  diagButtonConsumed = false;
  diagButtonAnyFail = false;
  diagOsSubState = 0;
  diagFsOk = false;
  diagEepromOk = false;
  needsRedraw = true;
}

void exitDiagnosticMode() {
  diagPrintReport();
  inDiagnosticMode = false;
  diagState = DIAG_INTRO;
  diagResults.clear();
  diagReportLines.clear();
  diagReportScroll = 0;
  needsRedraw = true;
}

void diagGotoState(DiagState next) {
  diagState = next;
  diagSelection = 0;
  diagEncoderDetected = 0;
  diagButtonStage = 0;
  diagButtonPhase = false;
  diagButtonConsumed = false;
  diagOsSubState = 0;
  needsRedraw = true;
}

void diagRecordResult(const String &name, char status, const String &detail) {
  diagResults.push_back({name, status, detail});
  String s = (status == 'P') ? "PASS" : (status == 'F') ? "FAIL" : "UNKN";
  DBG("[DIAG] " + s + " " + name + (detail.length() ? " (" + detail + ")" : ""));
}

void diagHandleEncoder(int steps) {
  if (steps == 0) return;
  switch (diagState) {
    case DIAG_ENCODER_TEST:
      if (!diagButtonPhase) {
        diagEncoderDetected = (steps > 0) ? 1 : -1;
        diagButtonPhase = true;
      } else {
        diagSelection = (diagSelection + steps + 2) % 2;
      }
      break;
    case DIAG_BUTTON_TEST:
      if (diagButtonPhase) {
        diagSelection = (diagSelection + steps + 2) % 2;
      }
      break;
    case DIAG_REPORT:
      diagReportScroll = constrain(diagReportScroll + steps, 0, max(0, (int)diagReportLines.size() - 1));
      break;
    default:
      diagSelection = (diagSelection + steps + 2) % 2;
      break;
  }
  needsRedraw = true;
}

void diagCaptureButtonPress(int id) {
  if (diagState != DIAG_BUTTON_TEST || diagButtonPhase) return;
  if (id != (int)diagButtonStage) return;
  diagButtonPhase = true;
  diagSelection = 0;
  needsRedraw = true;
}

void advanceButtonTest(bool ok) {
  if (!ok) diagButtonAnyFail = true;

  diagButtonStage++;
  while (diagButtonStage == 1 && PIN_CON < 0) diagButtonStage++;
  while (diagButtonStage == 2 && PIN_BAK < 0) diagButtonStage++;

  if (diagButtonStage > 2) {
    diagRecordResult("Buttons", diagButtonAnyFail ? 'F' : 'P');
    diagButtonAnyFail = false;
    diagGotoState(DIAG_OS_TEST);
  } else {
    diagButtonPhase = false;
    diagSelection = 0;
    needsRedraw = true;
  }
}

void diagRunFilesystemTest() {
  const char* path = "/diag_test.txt";
  bool ok = false;
  if (LittleFS.exists(path)) LittleFS.remove(path);
  File f = LittleFS.open(path, "w");
  if (f) {
    f.print("type_it fs test");
    f.close();
    f = LittleFS.open(path, "r");
    if (f) {
      String s = f.readString();
      ok = (s == "type_it fs test");
      f.close();
    }
  }
  if (LittleFS.exists(path)) LittleFS.remove(path);
  diagFsOk = ok;
  diagRecordResult("Filesystem", ok ? 'P' : 'F');
}

void diagRunEepromTest() {
  const int testAddr = EEPROM_SIZE - 2;
  uint8_t original = EEPROM.read(testAddr);
  uint8_t pattern = 0xA5;
  EEPROM.write(testAddr, pattern);
  EEPROM.commit();
  uint8_t readback = EEPROM.read(testAddr);
  EEPROM.write(testAddr, original);
  EEPROM.commit();
  diagEepromOk = (readback == pattern);
  diagRecordResult("EEPROM", diagEepromOk ? 'P' : 'F');
}

void diagHandleShortPress() {
  switch (diagState) {
    case DIAG_INTRO:
      diagGotoState(DIAG_DISPLAY_TEST);
      break;
    case DIAG_DISPLAY_TEST:
      diagRecordResult("Display", diagSelection == 0 ? 'P' : 'F');
      diagGotoState(DIAG_ENCODER_TEST);
      break;
    case DIAG_ENCODER_TEST:
      if (!diagButtonPhase) break;
      diagRecordResult("Encoder", diagSelection == 0 ? 'P' : 'F',
                       String("detected ") + (diagEncoderDetected > 0 ? "CW" : "CCW"));
      diagButtonStage = 0;
      diagButtonPhase = false;
      diagButtonAnyFail = false;
      diagGotoState(DIAG_BUTTON_TEST);
      break;
    case DIAG_BUTTON_TEST:
      if (!diagButtonPhase) break;
      advanceButtonTest(diagSelection == 0);
      break;
    case DIAG_OS_TEST:
      if (diagOsSubState == 0) {
        if (diagSelection == 0) {
          diagOsSubState = 1;
          typeUnicode("type_it diag: Hello \xE4\xB8\x96\xE7\x95\x8C");  // Hello 世界
        } else {
          diagRecordResult("OS/Type", 'U', "user skipped typing test");
          diagRunFilesystemTest();
          diagGotoState(DIAG_FILESYSTEM_TEST);
        }
      } else {
        diagRecordResult("OS/Type", diagSelection == 0 ? 'P' : 'F', osModeName(osMode));
        diagRunFilesystemTest();
        diagGotoState(DIAG_FILESYSTEM_TEST);
      }
      break;
    case DIAG_FILESYSTEM_TEST:
      diagRunEepromTest();
      diagGotoState(DIAG_EEPROM_TEST);
      break;
    case DIAG_EEPROM_TEST:
      diagGotoState(DIAG_FONT_TEST);
      break;
    case DIAG_FONT_TEST:
      diagRecordResult("Font", diagSelection == 0 ? 'P' : 'F');
      diagBuildReport();
      diagGotoState(DIAG_REPORT);
      break;
    case DIAG_REPORT:
      exitDiagnosticMode();
      break;
  }
  needsRedraw = true;
}

void diagHandleLongPress() {
  diagRecordResult("Abort", 'F', "user long-pressed BAK");
  diagBuildReport();
  exitDiagnosticMode();
}

void diagBuildReport() {
  diagReportLines.clear();
  for (const auto &r : diagResults) {
    String line;
    switch (r.status) {
      case 'P': line = "[PASS] "; break;
      case 'F': line = "[FAIL] "; break;
      default:  line = "[UNKN] "; break;
    }
    line += r.name;
    if (r.detail.length() > 0) line += ": " + r.detail;
    diagReportLines.push_back(line);
  }
  diagReportLines.push_back("> Press to exit <");
  diagReportScroll = 0;
}

void diagPrintReport() {
  Serial.println("=== Diagnostic Report ===");
  int pass = 0, fail = 0, unknown = 0;
  for (const auto &r : diagResults) {
    String s = (r.status == 'P') ? "PASS" : (r.status == 'F') ? "FAIL" : "UNKNOWN";
    Serial.println("[" + s + "] " + r.name +
                   (r.detail.length() ? " (" + r.detail + ")" : ""));
    if (r.status == 'P') pass++;
    else if (r.status == 'F') fail++;
    else unknown++;
  }
  Serial.println("-------------------------");
  Serial.println("Passed: " + String(pass) +
                 "  Failed: " + String(fail) +
                 "  Unknown: " + String(unknown));
  Serial.println("=========================");
}

void drawDiagYesNo() {
  const char* yes = "YES";
  const char* no = "NO";
  int y = SCREEN_H - 4;
  int yesW = u8g2.getUTF8Width(yes);
  int noW = u8g2.getUTF8Width(no);
  int gap = 16;
  int totalW = yesW + gap + noW;
  int x0 = (SCREEN_W - totalW) / 2;

  if (diagSelection == 0) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(x0 - 2, y - 10, yesW + 4, 12);
    u8g2.setDrawColor(0);
  } else {
    u8g2.setDrawColor(1);
  }
  u8g2.drawUTF8(x0, y, yes);

  u8g2.setDrawColor(1);
  int x1 = x0 + yesW + gap;
  if (diagSelection == 1) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(x1 - 2, y - 10, noW + 4, 12);
    u8g2.setDrawColor(0);
  }
  u8g2.drawUTF8(x1, y, no);
  u8g2.setDrawColor(1);
}

void drawDiagnostic() {
  switch (diagState) {
    case DIAG_INTRO:
      drawHeader("Diagnostic Mode");
      u8g2.drawUTF8(0, 28, "Short press = continue");
      u8g2.drawUTF8(0, 46, "Long BAK = abort");
      break;
    case DIAG_DISPLAY_TEST:
      drawHeader("Test 1/7: Display");
      u8g2.drawUTF8(0, 28, "中文 ABC 123");
      u8g2.drawUTF8(0, 46, "Display OK?");
      drawDiagYesNo();
      break;
    case DIAG_ENCODER_TEST:
      drawHeader("Test 2/7: Encoder");
      if (!diagButtonPhase) {
        u8g2.drawUTF8(0, 36, "Rotate 1 step CW");
      } else {
        String dir = diagEncoderDetected > 0 ? "CW" : "CCW";
        u8g2.drawUTF8(0, 28, ("Detected: " + dir).c_str());
        u8g2.drawUTF8(0, 46, "Correct?");
        drawDiagYesNo();
      }
      break;
    case DIAG_BUTTON_TEST:
      drawHeader("Test 3/7: Buttons");
      if (!diagButtonPhase) {
        String label;
        if (diagButtonStage == 0) label = "Press: encoder";
        else if (diagButtonStage == 1) label = "Press: CON";
        else label = "Press: BAK";
        u8g2.drawUTF8(0, 36, label.c_str());
      } else {
        String label;
        if (diagButtonStage == 0) label = "encoder pressed";
        else if (diagButtonStage == 1) label = "CON pressed";
        else label = "BAK pressed";
        u8g2.drawUTF8(0, 28, label.c_str());
        u8g2.drawUTF8(0, 46, "Works?");
        drawDiagYesNo();
      }
      break;
    case DIAG_OS_TEST:
      drawHeader("Test 4/7: OS/Type");
      if (diagOsSubState == 0) {
        u8g2.drawUTF8(0, 22, ("OS: " + String(osModeName(osMode))).c_str());
        u8g2.drawUTF8(0, 40, "Type test string?");
        drawDiagYesNo();
      } else {
        u8g2.drawUTF8(0, 28, "String typed.");
        u8g2.drawUTF8(0, 46, "Typed OK?");
        drawDiagYesNo();
      }
      break;
    case DIAG_FILESYSTEM_TEST:
      drawHeader("Test 5/7: Filesystem");
      {
        String s = diagFsOk ? "[PASS] write/read" : "[FAIL] write/read";
        u8g2.drawUTF8(0, 36, s.c_str());
      }
      u8g2.drawUTF8(0, 54, "Press to continue");
      break;
    case DIAG_EEPROM_TEST:
      drawHeader("Test 6/7: EEPROM");
      {
        String s = diagEepromOk ? "[PASS] byte r/w" : "[FAIL] byte r/w";
        u8g2.drawUTF8(0, 36, s.c_str());
      }
      u8g2.drawUTF8(0, 54, "Press to continue");
      break;
    case DIAG_FONT_TEST:
      drawHeader("Test 7/7: Font");
      u8g2.drawUTF8(0, 28, "中文渲染 ABC");
      u8g2.drawUTF8(0, 46, "CJK visible?");
      drawDiagYesNo();
      break;
    case DIAG_REPORT:
      drawHeader("Diagnostic Report");
      drawList(diagReportLines, diagReportScroll, diagReportOffset);
      break;
  }
}
