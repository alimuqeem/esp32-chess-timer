// Chess clock logic. Pure C++ (no Arduino, no display) so tests/test_clock.cpp can run it on the Mac.
//
// Two players, numbered by where they sit: 0 = bottom of the screen, 1 = top (upside down).
// Rule of the clock: you tap YOUR OWN side after your move; that stops your clock, adds the
// increment, and starts your opponent's. Taps on the side whose clock is not running are ignored.
#pragma once
#include <stdint.h>
#include <stdio.h>

enum class Phase : uint8_t { Setup, Ready, Running, Paused, Flagged };

struct Preset {
  const char *label;  // "5+3"
  uint16_t baseSec, incSec;
  char group;  // B bullet, Z blitz, R rapid, C classical (colours the tile)
};

// 3 columns x 4 rows on the setup screen: one row per group.
static const Preset PRESETS[12] = {
    {"1+0", 60, 0, 'B'},    {"1+1", 60, 1, 'B'},     {"2+1", 120, 1, 'B'},
    {"3+0", 180, 0, 'Z'},   {"3+2", 180, 2, 'Z'},    {"5+0", 300, 0, 'Z'},
    {"10+0", 600, 0, 'R'},  {"15+10", 900, 10, 'R'}, {"25+10", 1500, 10, 'R'},
    {"30+0", 1800, 0, 'C'}, {"60+0", 3600, 0, 'C'},  {"90+30", 5400, 30, 'C'},
};
static const int N_PRESETS = 12;

static const int64_t US = 1000000;

struct ChessClock {
  Phase phase = Phase::Setup;
  int preset = 5;  // index into PRESETS (5+0 until a game picks another)
  int64_t baseUs = 0, incUs = 0;
  int64_t left[2] = {0, 0};  // time left for each player, as of turnStart for the running one
  int active = 0;            // whose clock runs (valid while Running/Paused)
  int64_t turnStart = 0;     // timestamp the running clock last started (us)
  uint32_t moves[2] = {0, 0};
  int flagged = -1;  // who ran out of time

  // Choose a preset and wait for the first tap.
  void select(int idx) {
    if (idx < 0 || idx >= N_PRESETS) return;
    preset = idx;
    rematch();
  }
  // Same time control again.
  void rematch() {
    baseUs = (int64_t)PRESETS[preset].baseSec * US;
    incUs = (int64_t)PRESETS[preset].incSec * US;
    left[0] = left[1] = baseUs;
    moves[0] = moves[1] = 0;
    active = 0;
    flagged = -1;
    phase = Phase::Ready;
  }
  void menu() { phase = Phase::Setup; }

  // Time left for p at `now`, never below zero.
  int64_t remaining(int p, int64_t now) const {
    int64_t t = left[p];
    if (phase == Phase::Running && p == active) t -= now - turnStart;
    return t < 0 ? 0 : t;
  }

  // Called often. Flags the running player the instant their time hits zero. True if that happened.
  bool tick(int64_t now) {
    if (phase != Phase::Running || remaining(active, now) > 0) return false;
    left[active] = 0;
    flagged = active;
    phase = Phase::Flagged;
    return true;
  }

  // A player tapped their own side (zone 0 = bottom, 1 = top). True if the game state changed.
  bool tap(int zone, int64_t now) {
    tick(now);  // a tap after time ran out must not rescue the player
    if (phase == Phase::Ready) {  // the first tap starts the opponent's clock; no move yet
      active = 1 - zone;
      turnStart = now;
      phase = Phase::Running;
      return true;
    }
    if (phase != Phase::Running || zone != active) return false;
    left[active] = remaining(active, now) + incUs;
    moves[active]++;
    active = 1 - active;
    turnStart = now;
    return true;
  }

  void pause(int64_t now) {
    if (phase != Phase::Running) return;
    tick(now);
    if (phase != Phase::Running) return;
    left[active] = remaining(active, now);
    phase = Phase::Paused;
  }
  void resume(int64_t now) {
    if (phase != Phase::Paused) return;
    turnStart = now;
    phase = Phase::Running;
  }
};

// ----------------------------------------------------------------- time text ----

enum class TimeStyle : uint8_t { Hours, Minutes, Tenths };

// What to show for `us` of time left. Rounded UP, like a real clock: it reads 0:01 until the very end
// and 00:00 only once the flag has fallen. Under 20 s it switches to seconds with tenths ("9.4").
inline TimeStyle formatTime(int64_t us, char *out, size_t n) {
  if (us <= 0) {
    snprintf(out, n, "00:00");
    return TimeStyle::Minutes;
  }
  if (us <= 20 * US) {
    int64_t tenths = (us + 99999) / 100000;
    snprintf(out, n, "%d.%d", (int)(tenths / 10), (int)(tenths % 10));
    return TimeStyle::Tenths;
  }
  int64_t s = (us + US - 1) / US;
  if (s >= 3600) {
    snprintf(out, n, "%d:%02d:%02d", (int)(s / 3600), (int)((s / 60) % 60), (int)(s % 60));
    return TimeStyle::Hours;
  }
  snprintf(out, n, "%02d:%02d", (int)(s / 60), (int)(s % 60));
  return TimeStyle::Minutes;
}
