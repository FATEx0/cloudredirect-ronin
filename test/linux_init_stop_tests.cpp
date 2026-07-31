#include "init_stop.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;
using LinuxInitStop::PollOutcome;
using LinuxInitStop::PollUntilReady;
using LinuxInitStop::StopSignal;

static int g_failures = 0;

#define CHECK(cond, message) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s\n", message); \
        ++g_failures; \
    } \
} while (0)

static long long ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

// A live process (no stop requested) must still burn the whole window.
static void TestWaitSleepsFullIntervalWithoutStop() {
    StopSignal stop;
    CHECK(!stop.Requested(), "a fresh signal must not report a stop");
    auto start = std::chrono::steady_clock::now();
    bool stopped = stop.WaitFor(100ms);
    auto elapsed = ElapsedMs(start);
    CHECK(!stopped, "WaitFor must report no stop when none was requested");
    CHECK(elapsed >= 95, "WaitFor must sleep the requested interval");
}

static void TestWaitReturnsAtOnceAfterRequest() {
    StopSignal stop;
    stop.Request();
    CHECK(stop.Requested(), "Request must be observable");
    auto start = std::chrono::steady_clock::now();
    bool stopped = stop.WaitFor(5000ms);
    CHECK(stopped, "WaitFor must report an already-requested stop");
    CHECK(ElapsedMs(start) < 100, "an already-requested stop must not sleep");
}

static void TestWaitWakesOnConcurrentRequest() {
    StopSignal stop;
    std::thread waker([&stop] {
        std::this_thread::sleep_for(50ms);
        stop.Request();
    });
    auto start = std::chrono::steady_clock::now();
    bool stopped = stop.WaitFor(10000ms);
    auto elapsed = ElapsedMs(start);
    waker.join();
    CHECK(stopped, "a concurrent Request must end the wait");
    CHECK(elapsed < 1000, "a concurrent Request must end the wait promptly");
}

static void TestPollStopsProbingWhenReady() {
    StopSignal stop;
    int probes = 0;
    auto outcome = PollUntilReady(stop, 240, 20ms, [&probes] {
        return ++probes == 3;
    });
    CHECK(outcome == PollOutcome::Ready, "poll must report the awaited condition");
    CHECK(probes == 3, "poll must stop probing once the condition holds");
}

static void TestPollProbesFirstWithoutSleeping() {
    StopSignal stop;
    auto start = std::chrono::steady_clock::now();
    auto outcome = PollUntilReady(stop, 240, 500ms, [] { return true; });
    CHECK(outcome == PollOutcome::Ready, "an immediately ready probe must succeed");
    CHECK(ElapsedMs(start) < 100, "poll must probe before it sleeps");
}

// The live-client path: with no stop request the loop must spend every attempt
// (this is the 240 x 500ms = 120s attach ceiling, scaled down for the test).
static void TestPollBurnsFullCeilingWithoutStop() {
    StopSignal stop;
    int probes = 0;
    auto start = std::chrono::steady_clock::now();
    auto outcome = PollUntilReady(stop, 6, 50ms, [&probes] {
        ++probes;
        return false;
    });
    auto elapsed = ElapsedMs(start);
    CHECK(outcome == PollOutcome::TimedOut, "poll must time out when never ready");
    CHECK(probes == 6, "poll must use every attempt of the window");
    CHECK(elapsed >= 280, "poll must wait attempts x interval before giving up");
}

static void TestPollCancelsPromptlyOnStop() {
    StopSignal stop;
    std::atomic<int> probes{0};
    std::thread waker([&stop] {
        std::this_thread::sleep_for(50ms);
        stop.Request();
    });
    auto start = std::chrono::steady_clock::now();
    // 240 x 500ms is the real attach ceiling: 120s if the stop is ignored.
    auto outcome = PollUntilReady(stop, 240, 500ms, [&probes] {
        ++probes;
        return false;
    });
    auto elapsed = ElapsedMs(start);
    waker.join();
    CHECK(outcome == PollOutcome::Stopped, "poll must report the stop request");
    CHECK(elapsed < 2000, "poll must abandon the window as soon as a stop arrives");
    CHECK(probes.load() <= 2, "poll must not keep probing after a stop");
}

static void TestProcessStopIsShared() {
    CHECK(&LinuxInitStop::ProcessStop() == &LinuxInitStop::ProcessStop(),
          "the process-wide signal must be a single instance");
    CHECK(!LinuxInitStop::ProcessStop().Requested(),
          "the process-wide signal must start unrequested");
}

int main() {
    TestWaitSleepsFullIntervalWithoutStop();
    TestWaitReturnsAtOnceAfterRequest();
    TestWaitWakesOnConcurrentRequest();
    TestPollStopsProbingWhenReady();
    TestPollProbesFirstWithoutSleeping();
    TestPollBurnsFullCeilingWithoutStop();
    TestPollCancelsPromptlyOnStop();
    TestProcessStopIsShared();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("linux init stop tests passed");
    return 0;
}
