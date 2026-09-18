#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

struct Rect {
  int16_t x, y, width, height;
  bool contains(uint16_t pointX, uint16_t pointY) const { return pointX >= x && pointX < x + width && pointY >= y && pointY < y + height; }
};

enum ParamKind : uint8_t { REPS = 0, TIME = 1 };
enum ExerciseParam : uint8_t { PARAM_REPS = 1 << 0, PARAM_TIME = 1 << 1, PARAM_WEIGHT = 1 << 2 };

struct Frame32 {
  const uint32_t *rows;
  uint8_t rowCount;
};

struct Animation32 {
  Frame32 frames[3];
  uint8_t frameCount;
  uint16_t frameDurationMs;
};

struct ExerciseDef {
  const char *name;
  ParamKind kind;
  uint8_t parameters;
  uint8_t met;
  uint8_t defaultValue;
  uint8_t defaultWeight;
  const Animation32 *animation;
};

extern const ExerciseDef exercises[];
extern const uint8_t EXERCISE_COUNT;
extern const Animation32 REST_ANIMATION;

uint8_t exerciseDefaultValue(uint8_t exerciseId);
uint8_t exerciseDefaultWeight(uint8_t exerciseId);
bool exerciseHasWeight(uint8_t exerciseId);
void drawExerciseAnimation(TFT_eSprite &sprite, const Animation32 &animation, uint8_t frame);
