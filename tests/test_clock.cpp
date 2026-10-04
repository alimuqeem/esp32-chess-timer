// Host tests for src/clock.h. Run: tools/board.sh test
#include <stdio.h>
#include <string.h>

#include "../src/clock.h"

static int failures = 0, checks = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    checks++;                                                    \
    if (!(cond)) {                                               \
      failures++;                                                \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                            \
  } while (0)

static const int64_t S = US;

static std::string fmt(int64_t us) {
  char b[16];
  formatTime(us, b, sizeof b);
  return b;
}

static int idx(const char *label) {
  for (int i = 0; i < N_PRESETS; i++)
    if (!strcmp(PRESETS[i].label, label)) return i;
  return -1;
}

static void test_ready_and_first_tap() {
  ChessClock c;
  CHECK(c.phase == Phase::Setup);
  c.select(idx("3+2"));
  CHECK(c.phase == Phase::Ready);
  CHECK(c.left[0] == 180 * S && c.left[1] == 180 * S && c.incUs == 2 * S);
  CHECK(c.tap(1, 1000));  // top player taps their side: the bottom player's clock starts
  CHECK(c.phase == Phase::Running && c.active == 0);
  CHECK(c.moves[0] == 0 && c.moves[1] == 0);        // starting is not a move
  CHECK(c.left[1] == 180 * S);                      // and earns no increment
}

static void test_first_tap_either_side() {
  ChessClock c;
  c.select(idx("3+0"));
  CHECK(c.tap(0, 0));  // bottom taps: the top clock runs instead
  CHECK(c.active == 1);
}

static void test_inactive_side_ignored() {
  ChessClock c;
  c.select(idx("10+0"));
  c.tap(1, 0);  // bottom runs
  CHECK(!c.tap(1, 5 * S));  // top taps while it is not their turn
  CHECK(c.active == 0 && c.moves[1] == 0 && c.remaining(0, 5 * S) == 595 * S);
}

static void test_increment_and_moves() {
  ChessClock c;
  c.select(idx("3+2"));
  c.tap(1, 0);
  CHECK(c.tap(0, 10 * S));  // bottom thought for 10 s
  CHECK(c.left[0] == 172 * S);  // 180 - 10 + 2
  CHECK(c.moves[0] == 1 && c.active == 1);
  CHECK(c.tap(1, 14 * S));  // top thought for 4 s
  CHECK(c.left[1] == 178 * S);  // 180 - 4 + 2
  CHECK(c.remaining(0, 14 * S) == 172 * S);  // bottom's clock is stopped while top moves
  CHECK(c.remaining(1, 14 * S) == 178 * S);
}

static void test_flag_exact() {
  ChessClock c;
  c.select(idx("1+0"));
  c.tap(1, 0);
  CHECK(!c.tick(60 * S - 1));  // one microsecond left
  CHECK(c.phase == Phase::Running);
  CHECK(c.tick(60 * S));
  CHECK(c.phase == Phase::Flagged && c.flagged == 0 && c.remaining(0, 99 * S) == 0);
  CHECK(!c.tap(0, 61 * S) && !c.tap(1, 61 * S));  // game over: taps do nothing
  CHECK(!c.tick(70 * S));                          // flagging happens once
}

static void test_late_tap_does_not_rescue() {
  ChessClock c;
  c.select(idx("1+1"));
  c.tap(1, 0);
  CHECK(!c.tap(0, 61 * S));  // tapped after the clock reached zero; the increment must not apply
  CHECK(c.phase == Phase::Flagged && c.flagged == 0 && c.left[0] == 0 && c.moves[0] == 0);
}

static void test_pause_resume() {
  ChessClock c;
  c.select(idx("10+0"));
  c.tap(1, 0);
  c.pause(10 * S);
  CHECK(c.phase == Phase::Paused);
  CHECK(c.remaining(0, 10 * S) == 590 * S);
  CHECK(c.remaining(0, 110 * S) == 590 * S);  // nothing runs while paused
  CHECK(!c.tap(0, 50 * S));                   // taps ignored while paused
  c.resume(110 * S);
  CHECK(c.phase == Phase::Running);
  CHECK(c.remaining(0, 115 * S) == 585 * S);
}

static void test_pause_after_expiry_flags() {
  ChessClock c;
  c.select(idx("1+0"));
  c.tap(1, 0);
  c.pause(61 * S);  // pause pressed too late
  CHECK(c.phase == Phase::Flagged && c.flagged == 0);
}

static void test_rematch_and_menu() {
  ChessClock c;
  c.select(idx("1+0"));
  c.tap(1, 0);
  c.tick(61 * S);
  c.rematch();
  CHECK(c.phase == Phase::Ready && c.left[0] == 60 * S && c.left[1] == 60 * S && c.flagged == -1);
  CHECK(c.moves[0] == 0 && c.moves[1] == 0);
  c.menu();
  CHECK(c.phase == Phase::Setup && c.preset == idx("1+0"));  // remembers the last choice
}

static void test_no_drift_over_many_moves() {
  ChessClock c;
  c.select(idx("15+10"));
  int64_t now = 1234567;  // arbitrary start
  c.tap(1, now);
  int64_t used[2] = {0, 0};
  for (int i = 0; i < 200; i++) {
    int64_t think = 700000 + (i % 7) * 123457;  // odd microsecond amounts
    int p = c.active;
    now += think;
    used[p] += think;
    CHECK(c.tap(p, now));
  }
  // Each player made 100 moves. Time left must be base - thinking + 100 increments, to the microsecond.
  CHECK(c.left[0] == 900 * S - used[0] + 100 * 10 * S);
  CHECK(c.left[1] == 900 * S - used[1] + 100 * 10 * S);
  CHECK(c.moves[0] == 100 && c.moves[1] == 100);
}

static void test_format() {
  CHECK(fmt(300 * S) == "05:00");
  CHECK(fmt(299 * S + 500000) == "05:00");  // rounds up: reads 05:00 until a full second has passed
  CHECK(fmt(299 * S) == "04:59");
  CHECK(fmt(60 * S) == "01:00");
  CHECK(fmt(59 * S + 900000) == "01:00");
  CHECK(fmt(3600 * S) == "1:00:00");
  CHECK(fmt(5400 * S) == "1:30:00");
  CHECK(fmt(3599 * S) == "59:59");
  CHECK(fmt(20 * S + 1) == "00:21");  // just over 20 s: still the mm:ss form
  CHECK(fmt(20 * S) == "20.0");
  CHECK(fmt(19 * S + 950000) == "20.0");
  CHECK(fmt(9 * S + 10000) == "9.1");
  CHECK(fmt(100000) == "0.1");
  CHECK(fmt(1) == "0.1");  // never shows 0.0 while time remains
  CHECK(fmt(0) == "00:00");
  CHECK(fmt(-5) == "00:00");
  char b[16];
  CHECK(formatTime(5400 * S, b, sizeof b) == TimeStyle::Hours);
  CHECK(formatTime(100 * S, b, sizeof b) == TimeStyle::Minutes);
  CHECK(formatTime(5 * S, b, sizeof b) == TimeStyle::Tenths);
}

static void test_presets_sane() {
  for (int i = 0; i < N_PRESETS; i++) {
    char want[16];
    int base = PRESETS[i].baseSec / 60;
    if (PRESETS[i].baseSec % 60) CHECK(false);
    snprintf(want, sizeof want, "%d+%d", base, PRESETS[i].incSec);
    CHECK(!strcmp(want, PRESETS[i].label));  // label always matches the numbers
  }
}

int main() {
  test_ready_and_first_tap();
  test_first_tap_either_side();
  test_inactive_side_ignored();
  test_increment_and_moves();
  test_flag_exact();
  test_late_tap_does_not_rescue();
  test_pause_resume();
  test_pause_after_expiry_flags();
  test_rematch_and_menu();
  test_no_drift_over_many_moves();
  test_format();
  test_presets_sane();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
