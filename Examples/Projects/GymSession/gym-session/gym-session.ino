#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <XPT2046_Touchscreen.h>
#include <TFT_eSPI.h>
#include <driver/i2s.h>
#include <driver/dac.h>
#include <math.h>
#include "exercise_catalog.h"

#define LCD_BACK_LIGHT_PIN 21
#define BUZZER_PIN 26
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
#define BOOT_BUTTON_PIN 0
#define SD_CS 5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCLK 18
#define LEDC_BASE_FREQ 5000
#define I2S_SAMPLE_RATE 22050

const int16_t SCREEN_W = 240;
const int16_t SCREEN_H = 320;
const char APP_VERSION[] = "v1.0";
const uint16_t BG = 0x10A2, PANEL = 0x9492, INK = TFT_WHITE;
const uint16_t MUTED = 0xBDF7, ACCENT = 0x5D9F, ORANGE = 0xFC60, GREEN = 0x5DCA, RED = 0xF800;

SPIClass touchSpi = SPIClass(VSPI);
XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
TFT_eSprite activeArt = TFT_eSprite(&tft);
TFT_eSprite activeValue = TFT_eSprite(&tft);
const uint16_t DEFAULT_TOUCH_MIN_X = 200, DEFAULT_TOUCH_MAX_X = 3800, DEFAULT_TOUCH_MIN_Y = 250, DEFAULT_TOUCH_MAX_Y = 3850;
uint16_t touchMinX = DEFAULT_TOUCH_MIN_X, touchMaxX = DEFAULT_TOUCH_MAX_X;
uint16_t touchMinY = DEFAULT_TOUCH_MIN_Y, touchMaxY = DEFAULT_TOUCH_MAX_Y;
const uint16_t TAP_LOCKOUT_MS = 200;

// Four bytes per step: type/id, exercise id, reps or seconds, weight in kg.
struct Step { uint8_t type; uint8_t id; uint8_t value; uint8_t weight; };
struct LegacySessionData { uint8_t age; uint8_t weightKg; uint8_t heightCm; uint8_t stepCount; Step steps[12]; };
struct LegacyCurrentSessionData { uint8_t age; uint8_t weightKg; uint8_t heightCm; uint8_t stepCount; Step steps[50]; };
struct ProfileData { uint8_t age; uint8_t weightKg; uint8_t heightCm; };
struct SessionData { uint8_t stepCount; Step steps[50]; };
ProfileData profile = {35, 75, 175};
SessionData session = {2, {{0, 0, 10, 40}, {1, 0, 30, 0}}};
const uint8_t MAX_SESSIONS = 10;
bool sessionPresent[MAX_SESSIONS] = {};
uint8_t selectedSession = 0, sessionPage = 0;
bool selectingWorkout = false;

enum Screen : uint8_t { HOME, PROFILE, SESSION_LIST, BUILDER, STEP_CONFIG, ACTIVE, SUMMARY, SETTINGS, CALIBRATION };
Screen screen = HOME;
uint8_t selectedStep = 0, selectedExercise = 0, activeStep = 0, builderOffset = 0;
bool editingWeight = false;
uint32_t sessionStarted = 0, stepStarted = 0, stepDuration = 0, totalCalories = 0, lastFrame = 0;
bool touchWasDown = false, screenDirty = true;
uint16_t lastRawTouchX = 0, lastRawTouchY = 0;
uint8_t calibrationPoint = 0;
uint16_t calibrationRawX[4] = {}, calibrationRawY[4] = {};
const int16_t calibrationTargetX[4] = {20, 220, 220, 20};
const int16_t calibrationTargetY[4] = {55, 55, 235, 235};
uint32_t tapLockoutUntil = 0;
uint32_t lastActiveRender = 0;
uint16_t toneFrequency = 0;
uint32_t toneUntil = 0, nextToneToggle = 0;
bool toneHigh = false;
uint32_t lastBeepSecond = UINT32_MAX;
bool i2sReady = false;
bool i2sInstalled = false;
uint8_t volumeIndex = 3;
uint8_t audioVolume = 4;
uint8_t screenBrightness = 218;
bool bootButtonWasDown = false;
uint32_t screenshotBlockedUntil = 0;
const uint8_t volumeLevels[] = {0, 1, 4, 8};
const int8_t SINE_WAVE[64] = {
  0, 12, 25, 37, 49, 60, 71, 81, 90, 99, 106, 113, 118, 122, 125, 127,
  127, 127, 125, 122, 118, 113, 106, 99, 90, 81, 71, 60, 49, 37, 25, 12,
  0, -12, -25, -37, -49, -60, -71, -81, -90, -99, -106, -113, -118, -122, -125, -127,
  -127, -127, -125, -122, -118, -113, -106, -99, -90, -81, -71, -60, -49, -37, -25, -12
};

bool takeScreenshot() {
  const uint16_t width = tft.width();
  const uint16_t height = tft.height();
  const uint32_t rowBytes = (uint32_t)width * 3;
  const uint32_t padding = (4 - (rowBytes % 4)) % 4;
  const uint32_t dataSize = (rowBytes + padding) * height;
  const uint32_t fileSize = 54 + dataSize;

  Serial.printf("[SCREENSHOT] Starting capture: %ux%u, %lu bytes\n", width, height, fileSize);

  digitalWrite(XPT2046_CS, HIGH);
  touchSpi.end();
  touchSpi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, touchSpi, 20000000)) {
    Serial.printf("[SCREENSHOT] SD initialization failed on CS GPIO %d\n", SD_CS);
    restoreTouchSpi();
    return false;
  }
  Serial.println("[SCREENSHOT] SD initialized");

  char path[24];
  uint16_t captureNumber = 1;
  do {
    snprintf(path, sizeof(path), "/cap_%u.bmp", captureNumber);
    captureNumber++;
  } while (SD.exists(path));

  captureNumber--;

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("[SCREENSHOT] Could not open %s for writing\n", path);
    SD.end();
    restoreTouchSpi();
    return false;
  }
  Serial.printf("[SCREENSHOT] Writing cap_%u.bmp\n", captureNumber);

  uint8_t header[54] = {
    'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
    40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 24, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
  };

  header[2] = fileSize; header[3] = fileSize >> 8;
  header[4] = fileSize >> 16; header[5] = fileSize >> 24;
  header[18] = width; header[19] = width >> 8;
  header[20] = width >> 16; header[21] = width >> 24;

  int32_t topDownHeight = -static_cast<int32_t>(height);
  header[22] = topDownHeight; header[23] = topDownHeight >> 8;
  header[24] = topDownHeight >> 16; header[25] = topDownHeight >> 24;
  file.write(header, sizeof(header));

  uint8_t row[SCREEN_W * 3];
  uint8_t paddingBytes[3] = {0, 0, 0};

  for (uint16_t y = 0; y < height; y++) {
    tft.readRectRGB(0, y, width, 1, row);

    for (uint32_t i = 0; i < rowBytes; i += 3) {
      uint8_t red = row[i + 1];
      row[i + 1] = row[i + 2];
      row[i + 2] = red;
    }

    file.write(row, rowBytes);
    if (padding) file.write(paddingBytes, padding);
  }

  file.close();
  SD.end();
  restoreTouchSpi();
  Serial.printf("[SCREENSHOT] Saved successfully: cap_%u.bmp\n", captureNumber);
  return true;
}

void restoreTouchSpi() {
  digitalWrite(SD_CS, HIGH);
  digitalWrite(XPT2046_CS, HIGH);
  touchSpi.end();
  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  pinMode(XPT2046_CS, OUTPUT);
  digitalWrite(XPT2046_CS, HIGH);
  Serial.println("[TOUCH] SPI restored");
}

void setup() {
  Serial.begin(115200);
  Serial.println("[BOOT] Gym Session starting");
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touchSpi); touch.setRotation(0); tft.init(); tft.setRotation(0); tft.invertDisplay(false);
  ledcAttach(LCD_BACK_LIGHT_PIN, LEDC_BASE_FREQ, 12); setScreenBrightness(screenBrightness);
  prefs.begin("gym-session", false); loadSession(); tft.fillScreen(BG);
  activeArt.createSprite(108, 112);
  activeValue.createSprite(108, 32);
  playStartupTune();
}

void loop() {
  bool bootButtonDown = digitalRead(BOOT_BUTTON_PIN) == LOW;
  if (bootButtonDown && !bootButtonWasDown && (int32_t)(millis() - screenshotBlockedUntil) >= 0) {
    Serial.printf("[BOOT] Button pressed on GPIO %d, level=%d\n", BOOT_BUTTON_PIN, digitalRead(BOOT_BUTTON_PIN));
    bool saved = takeScreenshot();
    Serial.printf("[BOOT] Screenshot result: %s\n", saved ? "SUCCESS" : "FAILED");
    screenshotBlockedUntil = millis() + 300;
  }
  bootButtonWasDown = bootButtonDown;

  uint16_t x = 0, y = 0; bool down = readTouch(x, y);
  if (down && !touchWasDown && (int32_t)(millis() - tapLockoutUntil) >= 0) {
    handleTap(x, y);
    tapLockoutUntil = millis() + TAP_LOCKOUT_MS;
  }
  touchWasDown = down;
  if (screen == ACTIVE) updateActive();
  serviceTone();
  if (screen == ACTIVE) {
    if (millis() - lastActiveRender > 180) { drawActiveDynamic(); lastActiveRender = millis(); }
  } else if (screenDirty) {
    drawScreen(); screenDirty = false; lastFrame = millis();
  }
}

bool readTouch(uint16_t &x, uint16_t &y) {
  if (!(touch.tirqTouched() && touch.touched())) return false;
  TS_Point p = touch.getPoint();
  lastRawTouchX = constrain(p.x, 0, 4095);
  lastRawTouchY = constrain(p.y, 0, 4095);
  x = constrain(map(lastRawTouchX, touchMinX, touchMaxX, 0, SCREEN_W), 0, SCREEN_W - 1);
  y = constrain(map(lastRawTouchY, touchMinY, touchMaxY, 0, SCREEN_H), 0, SCREEN_H - 1); return true;
}
const Rect HOME_PROFILE = {16, 90, 208, 48}, HOME_BUILD = {16, 154, 208, 48}, HOME_START = {16, 218, 208, 48}, HOME_SETTINGS = {140, 276, 84, 32};
const Rect PROFILE_AGE_DOWN = {16, 76, 64, 42}, PROFILE_AGE_UP = {160, 76, 64, 42}, PROFILE_WEIGHT_DOWN = {16, 132, 64, 42}, PROFILE_WEIGHT_UP = {160, 132, 64, 42}, PROFILE_HEIGHT_DOWN = {16, 188, 64, 42}, PROFILE_HEIGHT_UP = {160, 188, 64, 42}, PROFILE_SAVE = {16, 258, 96, 42};
const Rect SESSION_NEW = {10, 238, 220, 34}, SESSION_BACK = {10, 278, 80, 32}, SESSION_PREVIOUS = {105, 278, 40, 32}, SESSION_NEXT = {191, 278, 40, 32};
const Rect BUILDER_ADD_EXERCISE = {10, 224, 105, 34}, BUILDER_ADD_REST = {125, 224, 105, 34}, BUILDER_PREVIOUS = {10, 264, 40, 40}, BUILDER_NEXT = {100, 264, 40, 40}, BUILDER_SAVE = {150, 264, 80, 40};
const Rect STEP_EXERCISE = {10, 54, 220, 38}, STEP_VALUE = {10, 102, 220, 38}, STEP_DECREASE = {10, 150, 64, 38}, STEP_INCREASE = {82, 150, 64, 38}, STEP_REMOVE = {154, 150, 76, 38}, STEP_BACK = {10, 264, 96, 40}, STEP_SAVE = {120, 264, 100, 40};
const Rect ACTIVE_FINISH = {12, 236, 216, 36}, ACTIVE_SKIP_REPS = {12, 276, 100, 32}, ACTIVE_END_REPS = {124, 276, 104, 32}, ACTIVE_SKIP_TIMED = {12, 236, 100, 72}, ACTIVE_END_TIMED = {124, 236, 104, 72};
const Rect SETTINGS_VOLUME_DOWN = {10, 82, 64, 42}, SETTINGS_VOLUME_UP = {166, 82, 64, 42}, SETTINGS_BRIGHTNESS_DOWN = {10, 152, 64, 42}, SETTINGS_BRIGHTNESS_UP = {166, 152, 64, 42}, SETTINGS_CALIBRATE = {10, 208, 220, 42}, SETTINGS_BACK = {10, 263, 96, 42};
const Rect CALIBRATION_CANCEL = {10, 274, 96, 32}, SUMMARY_DONE = {16, 258, 96, 42};

Rect sessionRow(uint8_t row) { return {10, (int16_t)(54 + row * 34), 172, 28}; }
Rect sessionDelete(uint8_t row) { return {188, (int16_t)(54 + row * 34), 42, 28}; }
Rect builderStep(uint8_t row) { return {10, (int16_t)(54 + row * 28), 220, 22}; }

uint16_t extrapolateTouchEdge(uint16_t firstRaw, uint16_t secondRaw, int16_t firstTarget, int16_t secondTarget, int16_t edge) {
  return constrain((int32_t)firstRaw + ((int32_t)secondRaw - firstRaw) * (edge - firstTarget) / (secondTarget - firstTarget), 0, 4095);
}

void saveTouchCalibration() {
  touchMinX = extrapolateTouchEdge(calibrationRawX[0], calibrationRawX[1], calibrationTargetX[0], calibrationTargetX[1], 0);
  touchMaxX = extrapolateTouchEdge(calibrationRawX[0], calibrationRawX[1], calibrationTargetX[0], calibrationTargetX[1], SCREEN_W);
  touchMinY = extrapolateTouchEdge(calibrationRawY[0], calibrationRawY[3], calibrationTargetY[0], calibrationTargetY[3], 0);
  touchMaxY = extrapolateTouchEdge(calibrationRawY[0], calibrationRawY[3], calibrationTargetY[0], calibrationTargetY[3], SCREEN_H);
  if (touchMaxX <= touchMinX || touchMaxY <= touchMinY) {
    touchMinX = DEFAULT_TOUCH_MIN_X; touchMaxX = DEFAULT_TOUCH_MAX_X;
    touchMinY = DEFAULT_TOUCH_MIN_Y; touchMaxY = DEFAULT_TOUCH_MAX_Y;
  }
  prefs.putUShort("touchMinX", touchMinX); prefs.putUShort("touchMaxX", touchMaxX);
  prefs.putUShort("touchMinY", touchMinY); prefs.putUShort("touchMaxY", touchMaxY);
}

void beginTouchCalibration() { calibrationPoint = 0; screen = CALIBRATION; screenDirty = true; }

void handleTap(uint16_t x, uint16_t y) {
  if (screen == HOME) { if (HOME_PROFILE.contains(x,y)) screen=PROFILE; else if (HOME_BUILD.contains(x,y)) { selectingWorkout=false; sessionPage=0; screen=SESSION_LIST; } else if (HOME_START.contains(x,y)) { selectingWorkout=true; sessionPage=0; screen=SESSION_LIST; } else if (HOME_SETTINGS.contains(x,y)) screen=SETTINGS; screenDirty=true; return; }
  if (screen == PROFILE) {
    int16_t updatedRow = -1;
    if (PROFILE_AGE_DOWN.contains(x,y)) { profile.age=constrain(profile.age-1,12,99); updatedRow=76; }
    if (PROFILE_AGE_UP.contains(x,y)) { profile.age=constrain(profile.age+1,12,99); updatedRow=76; }
    if (PROFILE_WEIGHT_DOWN.contains(x,y)) { profile.weightKg=constrain(profile.weightKg-1,30,200); updatedRow=132; }
    if (PROFILE_WEIGHT_UP.contains(x,y)) { profile.weightKg=constrain(profile.weightKg+1,30,200); updatedRow=132; }
    if (PROFILE_HEIGHT_DOWN.contains(x,y)) { profile.heightCm=constrain(profile.heightCm-1,100,230); updatedRow=188; }
    if (PROFILE_HEIGHT_UP.contains(x,y)) { profile.heightCm=constrain(profile.heightCm+1,100,230); updatedRow=188; }
    if (PROFILE_SAVE.contains(x,y)) { saveProfile(); screen=HOME; screenDirty=true; }
    else if (updatedRow >= 0) drawProfileValue(updatedRow);
    return;
  }
  if (screen == SESSION_LIST) {
    uint8_t firstSlot = sessionPage * 5;
    for (uint8_t row = 0; row < 5; row++) {
      uint8_t slot = firstSlot + row;
      if (slot >= MAX_SESSIONS) break;
      if (sessionPresent[slot] && sessionRow(row).contains(x,y)) {
        selectedSession = slot;
        loadSessionSlot(selectedSession);
        if (selectingWorkout) startSession();
        else { builderOffset = 0; screen = BUILDER; }
        screenDirty = true;
        return;
      }
      if (!selectingWorkout && sessionPresent[slot] && sessionDelete(row).contains(x,y)) {
        deleteSessionSlot(slot);
        screenDirty = true;
        return;
      }
    }
    if (!selectingWorkout && SESSION_NEW.contains(x,y)) {
      createSession();
      screenDirty = true;
      return;
    }
    if (SESSION_BACK.contains(x,y)) screen = HOME;
    if (SESSION_PREVIOUS.contains(x,y) && sessionPage) sessionPage--;
    if (SESSION_NEXT.contains(x,y) && sessionPage == 0) sessionPage = 1;
    screenDirty = true;
    return;
  }
  if (screen == BUILDER) {
    for (uint8_t row = 0; row < 6; row++) {
      if (builderOffset + row < session.stepCount && builderStep(row).contains(x,y)) {
        selectedStep = builderOffset + row;
        editingWeight = false;
        screen = STEP_CONFIG;
      }
    }
    if (BUILDER_ADD_EXERCISE.contains(x,y) && session.stepCount<50) { session.steps[session.stepCount++]={0,selectedExercise,exerciseDefaultValue(selectedExercise),exerciseDefaultWeight(selectedExercise)}; selectedStep=session.stepCount-1; builderOffset=(selectedStep/6)*6; }
    if (BUILDER_ADD_REST.contains(x,y) && session.stepCount<50) { session.steps[session.stepCount++]={1,0,30,0}; selectedStep=session.stepCount-1; builderOffset=(selectedStep/6)*6; }
    if (BUILDER_PREVIOUS.contains(x,y) && builderOffset >= 6) builderOffset -= 6;
    if (BUILDER_NEXT.contains(x,y) && builderOffset + 6 < session.stepCount) builderOffset += 6;
    if (BUILDER_SAVE.contains(x,y)) { saveSession(); screen=HOME; }
    screenDirty=true; return;
  }
  if (screen == STEP_CONFIG) {
    if (!session.stepCount) { screen=BUILDER; screenDirty=true; return; }
    bool screenNeedsRedraw = false;
    if (STEP_EXERCISE.contains(x,y) && session.steps[selectedStep].type==0) {
      session.steps[selectedStep].id=(session.steps[selectedStep].id+1)%EXERCISE_COUNT;
      if (!exerciseHasWeight(session.steps[selectedStep].id)) editingWeight=false;
      screenNeedsRedraw=true;
    }
    if (STEP_VALUE.contains(x,y) && session.steps[selectedStep].type==0 && exerciseHasWeight(session.steps[selectedStep].id)) { editingWeight=!editingWeight; screenNeedsRedraw=true; }
    bool valueUpdated = false;
    if (STEP_DECREASE.contains(x,y)) { adjustSelected(-1); valueUpdated=true; }
    if (STEP_INCREASE.contains(x,y)) { adjustSelected(1); valueUpdated=true; }
    if (STEP_REMOVE.contains(x,y)) { removeSelected(); screen=BUILDER; }
    if (STEP_BACK.contains(x,y)) screen=BUILDER;
    if (STEP_SAVE.contains(x,y)) { saveSession(); screen=HOME; }
    if (valueUpdated && !screenNeedsRedraw && screen == STEP_CONFIG) drawStepValue();
    if (screenNeedsRedraw || screen != STEP_CONFIG) screenDirty=true;
    return;
  }
  if (screen == ACTIVE) {
    if (!stepDuration && ACTIVE_FINISH.contains(x,y) && session.steps[activeStep].type==0 && exercises[session.steps[activeStep].id].kind==REPS) finishStep();
    else if ((!stepDuration && ACTIVE_SKIP_REPS.contains(x,y)) || (stepDuration && ACTIVE_SKIP_TIMED.contains(x,y))) skipStep();
    else if ((!stepDuration && ACTIVE_END_REPS.contains(x,y)) || (stepDuration && ACTIVE_END_TIMED.contains(x,y))) endSession();
    return;
  }
  if (screen == SETTINGS) {
    bool volumeUpdated = false;
    bool brightnessUpdated = false;
    if (SETTINGS_VOLUME_DOWN.contains(x,y)) { volumeIndex=volumeIndex ? volumeIndex-1 : 0; audioSetVolume(volumeLevels[volumeIndex]); prefs.putUChar("volume",volumeIndex); volumeUpdated=true; }
    if (SETTINGS_VOLUME_UP.contains(x,y)) { volumeIndex=volumeIndex<3 ? volumeIndex+1 : 3; audioSetVolume(volumeLevels[volumeIndex]); prefs.putUChar("volume",volumeIndex); volumeUpdated=true; }
    if (SETTINGS_BRIGHTNESS_DOWN.contains(x,y)) { screenBrightness=screenBrightness>16 ? screenBrightness-16 : 1; setScreenBrightness(screenBrightness); prefs.putUChar("brightness",screenBrightness); brightnessUpdated=true; }
    if (SETTINGS_BRIGHTNESS_UP.contains(x,y)) { screenBrightness=screenBrightness<=239 ? screenBrightness+16 : 255; setScreenBrightness(screenBrightness); prefs.putUChar("brightness",screenBrightness); brightnessUpdated=true; }
    if (SETTINGS_CALIBRATE.contains(x,y)) beginTouchCalibration();
    else if (SETTINGS_BACK.contains(x,y)) { screen=HOME; screenDirty=true; }
    else {
      if (volumeUpdated) drawSettingsVolume();
      if (brightnessUpdated) drawSettingsBrightness();
    }
    return;
  }
  if (screen == CALIBRATION) {
    if (CALIBRATION_CANCEL.contains(x,y)) { screen=SETTINGS; screenDirty=true; return; }
    calibrationRawX[calibrationPoint] = lastRawTouchX;
    calibrationRawY[calibrationPoint] = lastRawTouchY;
    calibrationPoint++;
    if (calibrationPoint >= 4) { saveTouchCalibration(); screen=SETTINGS; }
    screenDirty=true;
    return;
  }
  if (screen == SUMMARY && SUMMARY_DONE.contains(x,y)) { screen=HOME; screenDirty=true; }
}

void adjustSelected(int delta) { Step &step=session.steps[selectedStep]; if (editingWeight && step.type==0 && exerciseHasWeight(step.id)) step.weight=constrain((int)step.weight+delta,0,250); else if (step.type==1 || exercises[step.id].kind==TIME) step.value=constrain((int)step.value+delta*5,5,600); else step.value=constrain((int)step.value+delta,1,99); }
void removeSelected() { for(uint8_t i=selectedStep;i+1<session.stepCount;i++) session.steps[i]=session.steps[i+1]; if(session.stepCount) session.stepCount--; if(selectedStep>=session.stepCount&&session.stepCount) selectedStep=session.stepCount-1; if(builderOffset>=session.stepCount&&builderOffset>=6) builderOffset-=6; }
void startSession() { if(!session.stepCount)return; activeStep=0; totalCalories=0; sessionStarted=millis(); beginStep(); screen=ACTIVE; drawActiveStatic(); drawActiveDynamic(); }
void beginStep() { stepStarted=millis(); lastBeepSecond=UINT32_MAX; Step &step=session.steps[activeStep]; stepDuration=(step.type==1||exercises[step.id].kind==TIME)?step.value*1000UL:0; playStepTune(); }
void addCurrentStepCalories() { Step &step=session.steps[activeStep]; if(step.type==0){uint32_t seconds=(millis()-stepStarted)/1000; uint8_t met=exercises[step.id].met; uint32_t activeSeconds=stepDuration?stepDuration/1000:max(seconds,(uint32_t)step.value*3); totalCalories+=(uint32_t)(met*3.5f*profile.weightKg*activeSeconds/12000.0f);} }
void finishStep() { addCurrentStepCalories(); if(++activeStep>=session.stepCount){playCompletionTune(); screen=SUMMARY; screenDirty=true;} else {beginStep(); drawActiveStatic(); drawActiveDynamic();} }
void skipStep() { stopTone(); if(++activeStep>=session.stepCount){playCompletionTune(); screen=SUMMARY; screenDirty=true;} else {beginStep(); drawActiveStatic(); drawActiveDynamic();} }
void endSession() { addCurrentStepCalories(); stopTone(); playCompletionTune(); screen=SUMMARY; screenDirty=true; }
void updateActive() {
  if(!stepDuration)return;
  uint32_t elapsed=millis()-stepStarted, remaining=stepDuration>elapsed?stepDuration-elapsed:0;
  uint32_t secondsLeft=(remaining+999)/1000;
  if(secondsLeft<=10&&secondsLeft>0) {
    if(secondsLeft!=lastBeepSecond) { lastBeepSecond=secondsLeft; startTone(700+(10-secondsLeft)*100,100); }
  } else if(secondsLeft==0) { stopTone(); finishStep(); }
}

void startTone(uint16_t frequency, uint16_t durationMs) {
  playToneBlocking(frequency, durationMs);
}

void stopTone() { toneFrequency=0; toneUntil=0; shutdownI2S(); }

void audioSetVolume(uint8_t volume) {
  audioVolume = constrain(volume, 0, 21);
  if (!audioVolume) stopTone();
}

void setScreenBrightness(uint8_t brightness) {
  screenBrightness = constrain(brightness, 1, 255);
  ledcWrite(LCD_BACK_LIGHT_PIN, ((uint32_t)4095 * screenBrightness) / 255);
}

void initI2S() {
  if (i2sInstalled) shutdownI2S();
  i2sReady = false;
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) return;
  i2sInstalled = true;
  if (i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN) != ESP_OK) {
    shutdownI2S();
    return;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2sReady = true;
}

void shutdownI2S() {
  if (!i2sInstalled) {
    i2sReady = false;
    dac_output_voltage(DAC_CHANNEL_2, 128);
    dac_output_disable(DAC_CHANNEL_2);
    return;
  }
  if (i2sReady) i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
  dac_output_voltage(DAC_CHANNEL_2, 128);
  dac_output_disable(DAC_CHANNEL_2);
  i2sInstalled = false;
  i2sReady = false;
}

void playStartupTune() {
  delay(120);
  playToneBlocking(523, 180);
  delay(35);
  playToneBlocking(659, 180);
  delay(35);
  playToneBlocking(784, 300);
  delay(20);
}

void playStepTune() {
  playToneBlocking(523, 90);
  delay(20);
  playToneBlocking(659, 90);
  delay(20);
  playToneBlocking(784, 130);
}

void playCompletionTune() {
  const uint16_t notes[] = {659, 784, 880, 784, 659, 523};
  for (uint8_t note = 0; note < 6; note++) {
    playToneBlocking(notes[note], 140);
    if (note < 5) delay(25);
  }
}

void playToneBlocking(uint16_t frequency, uint16_t durationMs) {
  if (!audioVolume || !frequency || !durationMs) return;
  initI2S();
  if (!i2sReady) return;
  static uint32_t samples[256];
  uint32_t totalFrames = (uint32_t)I2S_SAMPLE_RATE * durationMs / 1000;
  uint32_t frame = 0;
  const uint32_t phaseStep = ((uint64_t)frequency << 32) / I2S_SAMPLE_RATE;
  uint32_t phase = 0;
  while (frame < totalFrames) {
    uint32_t frames = min((uint32_t)256, totalFrames - frame);
    for (uint32_t i = 0; i < frames; i++) {
      int16_t dacSampleValue = 128 + ((int16_t)SINE_WAVE[phase >> 26] * audioVolume) / 127;
      phase += phaseStep;
      uint8_t dacSample = (uint8_t)dacSampleValue;
      uint16_t dacWord = (uint16_t)dacSample << 8;
      samples[i] = ((uint32_t)dacWord << 16) | dacWord;
    }
    size_t written = 0;
    if (i2s_write(I2S_NUM_0, samples, frames * sizeof(uint32_t), &written, pdMS_TO_TICKS(20)) != ESP_OK) break;
    frame += frames;
  }
  delay(50);
  i2s_zero_dma_buffer(I2S_NUM_0);
  shutdownI2S();
}

void serviceTone() {
}

void drawScreen() { tft.fillScreen(BG); if(screen==HOME)drawHome(); if(screen==PROFILE)drawProfile(); if(screen==SESSION_LIST)drawSessionList(); if(screen==BUILDER)drawBuilder(); if(screen==STEP_CONFIG)drawStepConfig(); if(screen==SUMMARY)drawSummary(); if(screen==SETTINGS)drawSettings(); if(screen==CALIBRATION)drawCalibration(); }
void title(const char *text, const char *sub=nullptr) { tft.setTextColor(INK,BG);tft.setTextSize(2);tft.drawString(text,16,14);if(sub){tft.setTextColor(MUTED,BG);tft.setTextSize(1);tft.drawString(sub,16,38);} }
void button(int16_t x,int16_t y,int16_t w,int16_t h,const char *text,uint16_t color);
void button(const Rect &bounds,const char *text,uint16_t color=PANEL) { button(bounds.x,bounds.y,bounds.width,bounds.height,text,color); }
void button(const Rect &bounds,String text,uint16_t color=PANEL) { button(bounds,text.c_str(),color); }
void button(int16_t x,int16_t y,int16_t w,int16_t h,const char *text,uint16_t color=PANEL) { tft.fillRoundRect(x,y,w,h,6,color);tft.setTextColor(INK,color);tft.setTextSize(1);int16_t tw=tft.textWidth(text);tft.drawString(text,x+(w-tw)/2,y+(h-8)/2); }
void button(int16_t x,int16_t y,int16_t w,int16_t h,String text,uint16_t color=PANEL) { button(x,y,w,h,text.c_str(),color); }
void drawHome() { uint8_t savedSessions = 0; for (uint8_t slot = 0; slot < MAX_SESSIONS; slot++) savedSessions += sessionPresent[slot]; title("GYM SESSION","Build a workout that fits your day");tft.setTextColor(MUTED,BG);tft.setTextSize(1);tft.drawString(APP_VERSION,SCREEN_W - tft.textWidth(APP_VERSION) - 8,16);tft.setTextColor(ACCENT,BG);tft.drawString("GET READY TO TRAIN",16,66);button(HOME_PROFILE,"PROFILE  |  age / body data");button(HOME_BUILD,"BUILD SESSION");button(HOME_START,"START WORKOUT",GREEN);tft.setTextColor(MUTED,BG);tft.drawString(String(savedSessions)+" sessions saved",16,288);button(HOME_SETTINGS,"SETTINGS",PANEL); }
void drawSessionList() {
  title(selectingWorkout ? "CHOOSE SESSION" : "YOUR SESSIONS", selectingWorkout ? "Select a workout to start" : "Create, edit, or delete a workout");
  uint8_t firstSlot = sessionPage * 5;
  for (uint8_t row = 0; row < 5; row++) {
    uint8_t slot = firstSlot + row;
    if (slot >= MAX_SESSIONS) break;
    if (sessionPresent[slot]) {
      button(sessionRow(row),String("SESSION ")+String(slot + 1),PANEL);
      if (!selectingWorkout) button(sessionDelete(row),"X",RED);
    } else {
      tft.setTextColor(MUTED,BG); tft.setTextSize(1); tft.drawString("EMPTY SLOT",20,64 + row * 34);
    }
  }
  if (!selectingWorkout) button(SESSION_NEW,"+ NEW SESSION", GREEN);
  button(SESSION_BACK,"BACK", ACCENT);
  button(SESSION_PREVIOUS,"<");
  String pageLabel = String(sessionPage + 1) + "/2";
  tft.setTextColor(MUTED,BG); tft.setTextSize(1); tft.drawString(pageLabel,158,290);
  button(SESSION_NEXT,">");
}
void drawSettings() { title("SETTINGS","Adjust sound and display");tft.setTextColor(MUTED,BG);tft.setTextSize(1);tft.drawString("VOLUME",78,70);button(SETTINGS_VOLUME_DOWN,"-");button(SETTINGS_VOLUME_UP,"+");tft.setTextColor(INK,BG);tft.setTextSize(2);String volumeLabel=volumeIndex?String(volumeIndex)+" / 3":"MUTE";tft.drawString(volumeLabel,120-tft.textWidth(volumeLabel)/2,94);tft.setTextColor(MUTED,BG);tft.setTextSize(1);tft.drawString("SCREEN BRIGHTNESS",62,140);button(SETTINGS_BRIGHTNESS_DOWN,"-");button(SETTINGS_BRIGHTNESS_UP,"+");tft.setTextColor(INK,BG);tft.setTextSize(2);String brightnessLabel=String(max(1,(int)(((uint16_t)screenBrightness*100+127)/255)))+"%";tft.drawString(brightnessLabel,120-tft.textWidth(brightnessLabel)/2,164);button(SETTINGS_CALIBRATE,"CALIBRATE TOUCH", PANEL);button(SETTINGS_BACK,"BACK", ACCENT); }
void drawCalibration() {
  title("CALIBRATE TOUCH","Tap each target once");
  tft.setTextColor(MUTED,BG); tft.setTextSize(1);
  tft.drawString(String("TARGET ")+String(calibrationPoint + 1)+" / 4",88,38);
  for (uint8_t point = 0; point < 4; point++) {
    uint16_t color = point == calibrationPoint ? ACCENT : PANEL;
    int16_t targetX = calibrationTargetX[point], targetY = calibrationTargetY[point];
    tft.drawLine(targetX - 10, targetY, targetX + 10, targetY, color);
    tft.drawLine(targetX, targetY - 10, targetX, targetY + 10, color);
    tft.fillCircle(targetX, targetY, 3, color);
  }
  button(CALIBRATION_CANCEL,"CANCEL");
}
void drawSettingsVolume() {
  tft.fillRect(76,90,88,26,BG);
  tft.setTextColor(INK,BG); tft.setTextSize(2);
  String volumeLabel=volumeIndex?String(volumeIndex)+" / 3":"MUTE";
  tft.drawString(volumeLabel,120-tft.textWidth(volumeLabel)/2,94);
}
void drawSettingsBrightness() {
  tft.fillRect(76,160,88,26,BG);
  tft.setTextColor(INK,BG); tft.setTextSize(2);
  String brightnessLabel=String(max(1,(int)(((uint16_t)screenBrightness*100+127)/255)))+"%";
  tft.drawString(brightnessLabel,120-tft.textWidth(brightnessLabel)/2,164);
}
void drawProfile() { title("YOUR PROFILE","Used for a simple MET-based");tft.setTextColor(MUTED,BG);tft.setTextSize(1);tft.drawString("calorie estimate",16,48);profileRow("AGE",String(profile.age)+" yr",PROFILE_AGE_DOWN,PROFILE_AGE_UP);profileRow("WEIGHT",String(profile.weightKg)+" kg",PROFILE_WEIGHT_DOWN,PROFILE_WEIGHT_UP);profileRow("HEIGHT",String(profile.heightCm)+" cm",PROFILE_HEIGHT_DOWN,PROFILE_HEIGHT_UP);button(PROFILE_SAVE,"SAVE",ACCENT); }
void drawProfileValue(int16_t y) {
  tft.fillRect(84,y+14,76,28,BG);
  tft.setTextColor(INK,BG); tft.setTextSize(2);
  if (y == 76) tft.drawString(String(profile.age)+" yr",84,y+17);
  else if (y == 132) tft.drawString(String(profile.weightKg)+" kg",84,y+17);
  else tft.drawString(String(profile.heightCm)+" cm",84,y+17);
}
void profileRow(const char *label,String value,const Rect &decrease,const Rect &increase) { tft.setTextColor(MUTED,BG);tft.setTextSize(1);tft.drawString(label,84,decrease.y+6);tft.setTextColor(INK,BG);tft.setTextSize(2);tft.drawString(value,84,decrease.y+17);button(decrease,"-");button(increase,"+"); }
void drawBuilder() {
  title("SESSION BUILDER","Tap a step to configure it");
  tft.setTextColor(MUTED,BG); tft.setTextSize(1);
  uint8_t totalPages = max((uint8_t)1, (uint8_t)((session.stepCount + 5) / 6));
  uint8_t currentPage = builderOffset / 6 + 1;
  for(uint8_t row=0;row<6;row++) {
    uint8_t index=builderOffset+row;
    if(index>=session.stepCount) break;
    Step &step=session.steps[index];
    String label=String(index+1)+". "+(step.type?"REST":exercises[step.id].name);
    button(builderStep(row),label,step.type?ORANGE:PANEL);
  }
  if(!session.stepCount) { tft.setTextColor(MUTED,BG); tft.drawString("No steps yet",10,85); }
  button(BUILDER_ADD_EXERCISE,"+ EXERCISE"); button(BUILDER_ADD_REST,"+ REST");
  button(BUILDER_PREVIOUS,"<");
  String pageLabel = String(currentPage)+"/"+String(totalPages);
  tft.setTextColor(MUTED,BG); tft.setTextSize(1);
  tft.drawString(pageLabel,50+(50-tft.textWidth(pageLabel))/2,280);
  button(BUILDER_NEXT,">"); button(BUILDER_SAVE,"SAVE",ACCENT);
}

void drawStepConfig() {
  Step &step=session.steps[selectedStep];
  title("CONFIGURE STEP",String("Step "+String(selectedStep+1)+" / "+String(session.stepCount)).c_str());
  if(step.type) button(STEP_EXERCISE,"REST",ORANGE);
  else button(STEP_EXERCISE,exercises[step.id].name,ACCENT);
  String value;
  if(step.type || exercises[step.id].kind==TIME) value=formatTime(step.value);
  else value=editingWeight && exerciseHasWeight(step.id)?String(step.weight)+" kg":String(step.value)+" reps";
  tft.setTextColor(INK,BG); tft.setTextSize(3); tft.drawString(value,10,108);
  if(!step.type) {
    tft.setTextColor(MUTED,BG); tft.setTextSize(1);
    tft.drawString(exerciseHasWeight(step.id) ? (editingWeight?"WEIGHT  |  tap value to set reps":"REPS  |  tap value to set weight") : "REPS  |  BODYWEIGHT",10,140);
  }
  button(STEP_DECREASE,"-"); button(STEP_INCREASE,"+"); button(STEP_REMOVE,"REMOVE");
  tft.setTextColor(MUTED,BG); tft.setTextSize(1);
  tft.drawString(step.type?"Rest duration":"Tap the name to select exercise",10,210);
  button(STEP_BACK,"BACK"); button(STEP_SAVE,"SAVE",ACCENT);
}
void drawStepValue() {
  Step &step=session.steps[selectedStep];
  String value;
  if(step.type || exercises[step.id].kind==TIME) value=formatTime(step.value);
  else value=editingWeight && exerciseHasWeight(step.id)?String(step.weight)+" kg":String(step.value)+" reps";
  tft.fillRect(8,104,224,32,BG);
  tft.setTextColor(INK,BG); tft.setTextSize(3); tft.drawString(value,10,108);
}
void drawActiveStatic() {
  Step &step=session.steps[activeStep];
  tft.fillScreen(BG);
  title(step.type?"REST":exercises[step.id].name,(String(activeStep+1)+" / "+String(session.stepCount)).c_str());
  tft.setTextColor(MUTED,BG); tft.setTextSize(1);
  if (!stepDuration) {
    tft.drawString("TARGET",12,90);
    tft.setTextColor(INK,BG); tft.setTextSize(2);
    tft.drawString("REPS "+String(step.value),12,106);
    if (exerciseHasWeight(step.id)) tft.drawString(String(step.weight)+" kg",12,130);
  } else {
    tft.drawString("TARGET",12,90);
    tft.setTextColor(INK,BG); tft.setTextSize(2);
    tft.drawString(formatTime(step.value),12,106);
    tft.setTextColor(MUTED,BG); tft.setTextSize(1);
    tft.drawString("CURRENT",12,136);
  }
  tft.setTextColor(MUTED,BG); tft.setTextSize(1);
  tft.drawString("TOTAL "+formatTime((millis()-sessionStarted)/1000),120,38);
  if (stepDuration == 0) {
    button(ACTIVE_FINISH,"FINISH SET",GREEN);
    button(ACTIVE_SKIP_REPS,"SKIP",PANEL);
    button(ACTIVE_END_REPS,"END SESSION",RED);
  } else {
    button(ACTIVE_SKIP_TIMED,"SKIP",PANEL);
    button(ACTIVE_END_TIMED,"END SESSION",RED);
  }
}

void drawActiveDynamic() {
  Step &step=session.steps[activeStep];
  const Animation32 &animation = step.type ? REST_ANIMATION : *exercises[step.id].animation;
  uint8_t frameCount = animation.frameCount;
  uint8_t frame = 0;
  if (frameCount > 1) {
    uint8_t cycleLength = frameCount * 2 - 2;
    uint8_t cyclePosition = (millis() / animation.frameDurationMs) % cycleLength;
    frame = cyclePosition < frameCount ? cyclePosition : cycleLength - cyclePosition;
  }
  activeArt.fillSprite(BG);
  drawExerciseAnimation(activeArt, animation, frame);
  tft.startWrite(); tft.setAddrWindow(120,92,108,112); tft.pushColors((uint16_t *)activeArt.getPointer(),108*112,false); tft.endWrite();

  if (stepDuration) {
    uint32_t elapsed=millis()-stepStarted;
    uint32_t remaining=stepDuration>elapsed?stepDuration-elapsed:0;
    tft.fillRect(10,146,100,30,BG);
    tft.setTextColor(remaining<=10000?ORANGE:INK,BG); tft.setTextSize(2); tft.drawString(formatTime((remaining+999)/1000),12,150);
  } else {
    activeValue.fillSprite(BG);
    activeValue.setTextColor(MUTED,BG); activeValue.setTextSize(1);
    activeValue.drawString("    Set "+formatTime((millis()-stepStarted)/1000),0,4);
    tft.startWrite(); tft.setAddrWindow(120,54,108,32); tft.pushColors((uint16_t *)activeValue.getPointer(),108*32,false); tft.endWrite();
  }
  tft.fillRect(120,36,108,12,BG);
  tft.setTextColor(MUTED,BG); tft.setTextSize(1);
  tft.drawString("Session "+formatTime((millis()-sessionStarted)/1000),120,38);
}

void drawSummary() { title("SESSION COMPLETE","Nice work. Session completed!.");tft.setTextColor(ACCENT,BG);tft.setTextSize(1);tft.drawString("TOTAL TIME",22,84);tft.setTextColor(INK,BG);tft.setTextSize(4);tft.drawString(formatTime((millis()-sessionStarted)/1000),22,104);tft.setTextColor(ACCENT,BG);tft.setTextSize(1);tft.drawString("ESTIMATED CALORIES",22,166);tft.setTextColor(INK,BG);tft.setTextSize(4);tft.drawString(String(totalCalories)+" kcal",22,186);button(SUMMARY_DONE,"DONE",ACCENT); }
String formatTime(uint32_t seconds) { char text[12];snprintf(text,sizeof(text),"%02lu:%02lu:%02lu",seconds/3600,(seconds/60)%60,seconds%60);return String(text); }
String sessionKey(uint8_t slot) { return String("slot") + String(slot); }
bool sessionSlotExists(uint8_t slot) { size_t length = prefs.getBytesLength(sessionKey(slot).c_str()); return slot < MAX_SESSIONS && (length == sizeof(session) || length == sizeof(LegacyCurrentSessionData) || length == sizeof(LegacySessionData)); }
void saveSession() { prefs.putBytes(sessionKey(selectedSession).c_str(), &session, sizeof(session)); sessionPresent[selectedSession] = true; }
void saveProfile() { prefs.putBytes("profile", &profile, sizeof(profile)); }
void loadSessionSlot(uint8_t slot) {
  String key = sessionKey(slot);
  size_t length = prefs.getBytesLength(key.c_str());
  if (length == sizeof(session)) prefs.getBytes(key.c_str(), &session, sizeof(session));
  else if (length == sizeof(LegacyCurrentSessionData)) {
    LegacyCurrentSessionData legacy;
    prefs.getBytes(key.c_str(), &legacy, sizeof(legacy));
    session.stepCount = min(legacy.stepCount, (uint8_t)50);
    memcpy(session.steps, legacy.steps, sizeof(session.steps));
    if (prefs.getBytesLength("profile") != sizeof(profile)) {
      profile.age = legacy.age; profile.weightKg = legacy.weightKg; profile.heightCm = legacy.heightCm;
      saveProfile();
    }
    saveSession();
  } else if (length == sizeof(LegacySessionData)) {
    LegacySessionData legacy;
    prefs.getBytes(key.c_str(), &legacy, sizeof(legacy));
    session.stepCount = min(legacy.stepCount, (uint8_t)50);
    memcpy(session.steps, legacy.steps, sizeof(legacy.steps));
    if (prefs.getBytesLength("profile") != sizeof(profile)) {
      profile.age = legacy.age; profile.weightKg = legacy.weightKg; profile.heightCm = legacy.heightCm;
      saveProfile();
    }
    saveSession();
  }
  if (session.stepCount > 50) session.stepCount = 0;
}
void deleteSessionSlot(uint8_t slot) { if (slot >= MAX_SESSIONS) return; prefs.remove(sessionKey(slot).c_str()); sessionPresent[slot] = false; if (selectedSession == slot) { selectedSession = 0; if (sessionPresent[0]) loadSessionSlot(0); } }
void createSession() {
  for (uint8_t slot = 0; slot < MAX_SESSIONS; slot++) {
    if (sessionPresent[slot]) continue;
    selectedSession = slot;
    session = {2, {{0, 0, 10, 40}, {1, 0, 30, 0}}};
    saveSession();
    builderOffset = 0;
    screen = BUILDER;
    return;
  }
}
void loadSession() {
  for (uint8_t slot = 0; slot < MAX_SESSIONS; slot++) sessionPresent[slot] = sessionSlotExists(slot);
  if (!sessionPresent[0] && prefs.getBytesLength("data") == sizeof(LegacyCurrentSessionData)) {
    LegacyCurrentSessionData legacy;
    prefs.getBytes("data", &legacy, sizeof(legacy));
    session.stepCount = min(legacy.stepCount, (uint8_t)50);
    memcpy(session.steps, legacy.steps, sizeof(session.steps));
    profile.age = legacy.age; profile.weightKg = legacy.weightKg; profile.heightCm = legacy.heightCm;
    saveProfile();
    selectedSession = 0;
    saveSession();
    prefs.remove("data");
  } else if (!sessionPresent[0] && prefs.getBytesLength("data") == sizeof(LegacySessionData)) {
    LegacySessionData legacy;
    prefs.getBytes("data", &legacy, sizeof(legacy));
    session.stepCount = min(legacy.stepCount, (uint8_t)50);
    memcpy(session.steps, legacy.steps, sizeof(legacy.steps));
    profile.age = legacy.age; profile.weightKg = legacy.weightKg; profile.heightCm = legacy.heightCm;
    saveProfile();
    selectedSession = 0;
    saveSession();
    prefs.remove("data");
  } else if (sessionPresent[0]) {
    selectedSession = 0;
    loadSessionSlot(0);
  }
  if (prefs.getBytesLength("profile") == sizeof(profile)) prefs.getBytes("profile", &profile, sizeof(profile));
  volumeIndex=prefs.getUChar("volume",3);if(volumeIndex>3)volumeIndex=3;screenBrightness=prefs.getUChar("brightness",218);if(!screenBrightness)screenBrightness=1;touchMinX=prefs.getUShort("touchMinX",DEFAULT_TOUCH_MIN_X);touchMaxX=prefs.getUShort("touchMaxX",DEFAULT_TOUCH_MAX_X);touchMinY=prefs.getUShort("touchMinY",DEFAULT_TOUCH_MIN_Y);touchMaxY=prefs.getUShort("touchMaxY",DEFAULT_TOUCH_MAX_Y);if(touchMaxX<=touchMinX||touchMaxY<=touchMinY){touchMinX=DEFAULT_TOUCH_MIN_X;touchMaxX=DEFAULT_TOUCH_MAX_X;touchMinY=DEFAULT_TOUCH_MIN_Y;touchMaxY=DEFAULT_TOUCH_MAX_Y;}audioSetVolume(volumeLevels[volumeIndex]);setScreenBrightness(screenBrightness);if(session.stepCount>50)session.stepCount=0;
}
