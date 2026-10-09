"""Exercise production dispatch, including every invalid enum value and cleanup."""
import pathlib
import re
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[2]
baseline = '--baseline' in sys.argv

def read(path):
    if baseline:
        return subprocess.check_output(['git', 'show', 'HEAD:' + path], cwd=repo, text=True)
    return (repo / path).read_text()

batch = read('vita3k/renderer/src/batch.cpp')
state = read('vita3k/renderer/src/state_set.cpp')
commands = read('vita3k/renderer/include/renderer/commands.h')
types = read('vita3k/renderer/include/renderer/types.h')
# Independent expected routing in the enum's ABI order. In particular,
# VertexStream precedes TwoSided/CullMode despite the old map initializer order.
command_names = [
    'create_context', 'create_render_target', 'memory_map', 'memory_unmap',
    'draw', 'transfer_copy', 'transfer_downscale', 'transfer_fill', 'nop',
    'set_state', 'set_context', 'sync_surface_data', 'mid_scene_flush',
    'signal_sync_object', 'wait_sync_object', 'notification', 'set_screen_filter',
    'new_frame', 'destroy_render_target', 'destroy_context',
]
state_names = [
    'region_clip', 'program', 'viewport', 'depth_bias', 'depth_func',
    'depth_write_enable', 'polygon_mode', 'point_line_width', 'stencil_func',
    'texture', 'stencil_ref', 'vertex_stream', 'two_sided', 'cull_mode',
    'uniform_buffer', 'fragment_program_enable', 'visibility_buffer', 'visibility_index',
]
code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <vector>
struct MemState {};
struct Config {};
struct FeatureState {};
'''
code += commands.replace('#pragma once', '')
code += '\nnamespace renderer {\n' + re.search(r'enum class GXMState .*?\n};', types, re.S)[0]
code += r'''
struct State {};
struct Context { std::function<void(Command *)> free_func; };
static unsigned errors, freed, observed;
static uint64_t checksum;
static std::vector<unsigned> order;
static bool recording = true;
#define LOG_ERROR(...) (++errors)
static void hit(unsigned id) {
    observed = id;
    checksum += id;
    if (recording) order.push_back(id);
}
void generic_command_free(Command *) { ++freed; }
'''
for i, name in enumerate(command_names):
    name = 'cmd_new_frame' if name == 'new_frame' else 'cmd_handle_' + name
    code += f'__attribute__((noinline)) void {name}(State &, MemState &, Config &, CommandHelper &, const FeatureState &, Context *) {{ hit({i}); }}\n'
for i, name in enumerate(state_names):
    code += f'__attribute__((noinline)) void cmd_set_state_{name}(State &, MemState &, Config &, CommandHelper &, Context *) {{ hit({i}); }}\n'
start = batch.index('static void process_batch(')
code += batch[start:batch.index('\nvoid process_batches(', start)]
start = state.index('COMMAND(handle_set_state) {')
code += state[start:state.index('\n} // namespace renderer', start)].replace(
    'COMMAND(handle_set_state)',
    'void dispatch_state(State &renderer, MemState &mem, Config &config, CommandHelper &helper, Context *render_context)')
code += r'''
} // namespace renderer
int main(int argc, char **) {
    using namespace renderer;
    State state;
    MemState mem;
    Config config;
    FeatureState features;
    Context ctx{[](Command *) { ++freed; }};
    Command command{};
    CommandList list{&command, &command, &ctx};
    for (unsigned id = 0; id < 256; ++id) {
        command.opcode = static_cast<CommandOpcode>(id);
        for (auto *context : {&ctx, static_cast<Context *>(nullptr)}) {
            list.context = context;
            const auto before = errors, released = freed;
            observed = 999;
            process_batch(state, features, mem, config, list);
            assert(freed == released + 1);
            assert(errors == before + (id >= 20));
            assert(observed == (id < 20 ? id : 999));
        }
    }
    for (unsigned id = 0; id < 65536; ++id) {
        auto value = static_cast<GXMState>(id);
        std::memcpy(command.data, &value, sizeof(value));
        CommandHelper helper(&command);
        const auto before = errors;
        observed = 999;
        dispatch_state(state, mem, config, helper, &ctx);
        assert(errors == before + (id >= 18));
        assert(observed == (id < 18 ? id : 999));
    }
    std::array<Command, 4> chain{};
    const unsigned ids[]{4, 255, 9, 17};
    for (unsigned i = 0; i < chain.size(); ++i) {
        chain[i].opcode = static_cast<CommandOpcode>(ids[i]);
        chain[i].next = i + 1 < chain.size() ? &chain[i + 1] : nullptr;
    }
    list = {chain.data(), &chain.back(), &ctx};
    order.clear();
    const auto released = freed;
    process_batch(state, features, mem, config, list);
    assert(freed == released + 4);
    assert((order == std::vector<unsigned>{4, 9, 17}));
    list.first = nullptr;
    process_batch(state, features, mem, config, list);
    assert(freed == released + 4);
    if (argc == 1) {
        std::cout << "All opcodes/states, unknown IDs, ordering and cleanup passed\n";
        return 0;
    }
    recording = false;
    ctx.free_func = [](Command *) {};
    std::array<Command, 4096> work{};
    uint32_t rng = 42;
    for (size_t i = 0; i < work.size(); ++i) {
        rng = rng * 1664525 + 1013904223;
        work[i].opcode = static_cast<CommandOpcode>((rng >> 16) % 20);
        auto value = static_cast<GXMState>((rng >> 16) % 18);
        std::memcpy(work[i].data, &value, sizeof(value));
        work[i].next = i + 1 < work.size() ? &work[i + 1] : nullptr;
    }
    list = {work.data(), &work.back(), &ctx};
    for (unsigned sample = 0; sample < 5; ++sample) {
        const auto begin = std::chrono::steady_clock::now();
        for (int rep = 0; rep < 1000; ++rep) {
            process_batch(state, features, mem, config, list);
            for (auto &cmd : work) {
                CommandHelper helper(&cmd);
                dispatch_state(state, mem, config, helper, &ctx);
            }
        }
        const auto end = std::chrono::steady_clock::now();
        std::cout << "ns/dispatch " << std::chrono::duration<double, std::nano>(end - begin).count() / (8192.0 * 1000) << '\n';
    }
    std::cout << "checksum " << checksum << '\n';
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp)
    (path / 'test.cpp').write_text(code)
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-UNDEBUG']
    if '--benchmark' in sys.argv:
        flags += ['-O3']
    else:
        flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    subprocess.run([sys.argv[1], *flags, str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test'), *(['bench'] if '--benchmark' in sys.argv else [])], check=True)
