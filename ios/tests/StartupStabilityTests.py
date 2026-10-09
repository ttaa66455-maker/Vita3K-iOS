"""Run production session startup/cleanup and shader warmup with host test doubles."""
import pathlib
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[2]
session = (repo / 'vita3k/app/src/session_controller.cpp').read_text()
batch = (repo / 'vita3k/renderer/src/batch.cpp').read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) ((void)0)
constexpr int Success = 0;
enum class AppSessionPhase { Idle, Launching, Running, Stopping };
enum class AppSessionStopReason { UserRequest, LaunchFailure };
namespace renderer {
struct FrameHost {
    int prepared = 0, finalized = 0, destroyed = 0;
    void prepare_for_render_thread() { ++prepared; }
    void finalize_render_thread_start() { ++finalized; }
    void destroy_render_context() { ++destroyed; }
};
struct State {
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    bool requested = false, completed = false, abort = false;
    void set_app(const char *, const char *) {}
    void cleanup() {}
    void join() {
        { std::lock_guard lock(mutex); abort = true; }
        changed.notify_all();
        if (worker.joinable()) worker.join();
    }
    ~State() { join(); }
};
void start_render_thread(State &state, int &, int &, int &, int &) {
    state.worker = std::thread([&] {
        std::unique_lock lock(state.mutex);
        state.changed.wait(lock, [&] { return state.abort || state.requested; });
        if (state.requested) { state.completed = true; state.changed.notify_all(); }
    });
}
}
struct EmuEnvState {
    std::unique_ptr<renderer::State> renderer = std::make_unique<renderer::State>();
    struct Motion { void refresh_device_motion_support() {} void clear_device_motion_support() {} } motion;
    struct { std::string title_id, app_path; } io;
    std::string self_name;
    int display = 0, gxm = 0, mem = 0, cfg = 0;
    bool load_ok = true, run_ok = true, throw_run = false, overlay_ready = false;
    int cleanups = 0, resets = 0, destroys = 0, updates = 0;
};
int load_app(int32_t &, EmuEnvState &env, int) { return env.load_ok ? Success : -1; }
void prepare_game_launch_overlay(EmuEnvState &env) { env.overlay_ready = true; }
int run_app(EmuEnvState &env, int32_t, int) {
    assert(env.overlay_ready);
    if (env.throw_run) throw std::runtime_error("synthetic module startup failure");
    if (!env.run_ok) return -1;
#ifdef VITA3K_PLATFORM_IOS
    // Model a library's synchronous GXM request during module_start. The old
    // ordering times out because no consumer exists until run_app returns.
    auto &state = *env.renderer;
    std::unique_lock lock(state.mutex);
    state.requested = true;
    state.changed.notify_all();
    if (!state.changed.wait_for(lock, std::chrono::seconds(1), [&] { return state.completed; }))
        return -1;
#endif
    return Success;
}
void update_app_time_used(EmuEnvState &env, const std::string &) { ++env.updates; }
void shutdown_app_runtime(EmuEnvState &env) { ++env.cleanups; env.renderer->join(); }
void reset_app_state(EmuEnvState &env) { ++env.resets; }
void destroy(EmuEnvState &env) { ++env.destroys; env.renderer.reset(); }
void abort_game_launch(EmuEnvState &) {}
const char *to_string(AppSessionPhase) { return "phase"; }
struct AppSessionController {
    EmuEnvState &emuenv;
    std::mutex mutex;
    std::atomic<AppSessionPhase> current_phase{AppSessionPhase::Launching};
    bool renderer_initialized = true, runtime_initialized = true;
    int active_launch_request = 0;
    std::optional<std::reference_wrapper<renderer::FrameHost>> frame_host;
    void set_phase(AppSessionPhase phase) { current_phase = phase; }
    void reset_session_tracking() { current_phase = AppSessionPhase::Idle; frame_host.reset(); }
    bool load_and_run();
    void stop(AppSessionStopReason reason);
};
'''
code += function(session, 'bool AppSessionController::load_and_run()')
code += function(session, 'void AppSessionController::stop(')
code += r'''
int main() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        EmuEnvState env;
        renderer::FrameHost host;
        AppSessionController controller{env};
        controller.frame_host = host;
        env.load_ok = scenario != 1;
        env.run_ok = scenario != 2;
        env.throw_run = scenario == 3;
        try {
            assert(controller.load_and_run() == (scenario == 0));
            assert(scenario != 3);
        } catch (const std::runtime_error &) { assert(scenario == 3); }
        assert(controller.current_phase == (scenario == 0 ? AppSessionPhase::Running : AppSessionPhase::Launching));
#ifdef VITA3K_PLATFORM_IOS
        assert(host.prepared == (scenario == 1 ? 0 : 1));
        assert(host.finalized == host.prepared);
#else
        assert(host.prepared == (scenario == 0 ? 1 : 0));
#endif
        controller.stop(scenario == 0 ? AppSessionStopReason::UserRequest : AppSessionStopReason::LaunchFailure);
        assert(env.cleanups == 1 && env.resets == 1 && env.destroys == 1);
        assert(env.updates == (scenario == 0 ? 1 : 0));
        assert(!env.renderer && host.destroyed == 1);
        controller.stop(AppSessionStopReason::UserRequest);
        assert(env.cleanups == 1 && host.destroyed == 1);
    }
}
'''

# Execute the actual warmup loop with deterministic time/headroom. The fake
# clock can advance during a shader load; no real sleeps or GPU are required.
start = batch.index('        auto next_progress_frame = ')
end = batch.index('\n        state.precompile_queue.clear();', start)
warmup = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <vector>
#define LOG_INFO(...) ((void)0)
namespace test_clock {
inline std::chrono::steady_clock::time_point time;
struct Clock { static auto now() { return time; } };
}
namespace vita3k_ios {
struct MemoryMonitor { static inline int pressure = 0; static int get_memory_pressure() { return pressure; } };
}
struct Overlay { void set_progress(int, int) {} };
struct State {
    std::vector<int> precompile_queue{0,1,2,3,4};
    std::atomic<bool> render_abort{false};
    int precompile_progress = 0, loaded = 0;
    bool current = true, become_constrained = false;
    std::chrono::seconds load_time{0};
    bool set_current() { return current; }
    void precompile_shader(int) {
        ++loaded;
        test_clock::time += load_time;
        if (become_constrained) vita3k_ios::MemoryMonitor::pressure = 50;
    }
    void render_frame(int, int, int) {}
    void swap_window() {}
};
void warm(State &state) {
    const int total = state.precompile_queue.size();
    std::shared_ptr<Overlay> progress_overlay;
    int display = 0, gxm = 0, mem = 0;
'''
warmup += batch[start:end].replace('std::chrono::steady_clock::now()', 'test_clock::Clock::now()')
warmup += r'''
}
int main() {
    State all;
    warm(all);
    assert(all.loaded == 5 && all.precompile_progress == 5);
    State pressure;
    vita3k_ios::MemoryMonitor::pressure = 50;
    warm(pressure);
    State deadline;
    vita3k_ios::MemoryMonitor::pressure = 0;
    deadline.load_time = std::chrono::seconds(1);
    warm(deadline);
    State growing;
    growing.become_constrained = true;
    warm(growing);
#ifdef VITA3K_PLATFORM_IOS
    assert(pressure.loaded == 0);
    assert(deadline.loaded == 2 && deadline.precompile_progress == 2);
    assert(growing.loaded == 1);
#else
    assert(pressure.loaded == 5 && deadline.loaded == 5 && growing.loaded == 5);
#endif
    State cancelled;
    cancelled.render_abort = true;
    warm(cancelled);
    assert(cancelled.loaded == 0);
    State unavailable;
    unavailable.current = false;
    warm(unavailable);
    assert(unavailable.loaded == 0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp)
    for name, source in [('startup', code), ('warmup', warmup)]:
        cpp = path / (name + '.cpp')
        cpp.write_text(source)
        for defines in ([], ['-DVITA3K_PLATFORM_IOS']):
            binary = path / name
            subprocess.run([sys.argv[1], '-std=c++20', '-pthread', '-UNDEBUG',
                            *defines, str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)
print('Startup GPU dependency, failed-launch cleanup and bounded warmup passed (iOS/desktop)')
