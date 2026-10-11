#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include <chrono>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/memory/Casts.hpp>
#include "../shared.hpp"

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;

#define UP CUniquePointer
#define SP CSharedPointer

const static auto           SLEEP_DURATIONS = std::array{1, 10};

static std::optional<pid_t> waitForSleepChild(pid_t parent) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (std::chrono::steady_clock::now() < deadline) {
        std::istringstream pids{Tests::execAndGet(std::format("pgrep -x -P {} sleep", parent))};
        pid_t              pid = 0;
        std::string        extra;
        if (pids >> pid && pid > 0 && !(pids >> extra) && Tests::processAlive(pid))
            return pid;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return std::nullopt;
}

TEST_CASE(processSpawning) {
    pid_t hyprlandPid = 0;
    for (const auto& instance : instances()) {
        if (instance.id == HIS) {
            hyprlandPid = sc<pid_t>(instance.pid);
            break;
        }
    }
    ASSERT(hyprlandPid > 0, true);

    for (const auto duration : SLEEP_DURATIONS) {
        // Note: POSIX sleep does not support fractional seconds, so
        // can't sleep for less than 1 second.
        const auto spawnedAt = std::chrono::steady_clock::now();
        OK(getFromSocket(std::format("/dispatch hl.dsp.exec_cmd('sleep {}')", duration)));

        // The dispatch returns before the child execs sleep. Observe that
        // exact compositor's live child instead of querying all sleep processes.
        const auto sleepPid = waitForSleepChild(hyprlandPid);
        if (!sleepPid) {
            NLog::log("{}Sleep was not observed as a live child of Hyprland pid {}", Colors::RED, hyprlandPid);
            continue;
        }

        std::ifstream status(std::format("/proc/{}/status", *sleepPid));
        pid_t         parentPid = 0;
        for (std::string line; std::getline(status, line);) {
            if (line.starts_with("PPid:")) {
                std::istringstream{line.substr(5)} >> parentPid;
                break;
            }
        }
        // The executable may be wrapped or renamed by its package. The exact
        // instance PID, rather than its process name, identifies the compositor.
        NLog::log("{}Expecting that sleep's parent is compositor pid {}", Colors::YELLOW, hyprlandPid);
        EXPECT(parentPid, hyprlandPid);

        const auto deadline = spawnedAt + std::chrono::seconds(duration + 5);
        while (Tests::processAlive(*sleepPid) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Ensure that sleep ran for its requested duration and was reaped.
        EXPECT(std::chrono::steady_clock::now() - spawnedAt >= std::chrono::seconds(duration), true);
        EXPECT(Tests::processAlive(*sleepPid), false);

        return;
    }

    FAIL_TEST_SILENT();
}
