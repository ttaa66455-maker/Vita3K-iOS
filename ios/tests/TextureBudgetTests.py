"""Execute production texture eviction against live bindings and cache metadata."""
import pathlib
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[2]
cpp = (repo / 'vita3k/renderer/src/vulkan/texture.cpp').read_text()
start = cpp.index('void VKTextureCache::trim_textures(')
end = cpp.index('\nvoid VKTextureCache::prepare_staging_buffer(', start)
code = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <map>
#include <vector>
#define LOG_INFO(...) ((void)0)
using TextureGxmDataRepr = uint32_t;
struct Image { bool image = true; int view = 0; };
struct Entry { Image texture; uint64_t memory_needed = 32ULL * 1024 * 1024; uint64_t last_used_frame = 1; };
struct Info { uint32_t texture; uint32_t texture_size = 4; };
struct Queue {
    struct Item { Info content; };
    std::array<Item, 16> items;
    std::vector<uint32_t> retired;
    void set_as_lru(Info *info) { retired.push_back(info->texture); }
};
struct VKContext {
    uint64_t frame_timestamp = 200;
    struct Binding { int imageView = 0; };
    Binding vertex_textures[16]{}, fragment_textures[16]{};
};
struct VKTextureCache {
    bool is_texture_transfer_ready = false;
    std::array<Entry, 16> textures;
    Entry *current_texture = nullptr;
    Queue texture_queue;
    std::map<uint32_t, Info *> texture_lookup;
    struct State {
        struct Frame {
            struct Destroy {
                std::vector<int> retained_views;
                void add_image(Image &image) {
                    retained_views.push_back(image.view);
                    image.image = false;
                    image.view = 0;
                }
            } destroy_queue;
        } slot;
        Frame &frame() { return slot; }
    } state;
    VKTextureCache() {
        for (size_t i = 0; i < textures.size(); ++i) {
            textures[i].texture.view = i + 1;
            texture_queue.items[i].content.texture = i + 1;
            texture_lookup[i + 1] = &texture_queue.items[i].content;
        }
    }
    void trim_textures(const VKContext &, bool);
};
'''
code += cpp[start:end]
code += r'''
int main() {
    VKTextureCache cache;
    VKContext context;
    context.vertex_textures[0].imageView = 1;
    context.fragment_textures[0].imageView = 2;
    cache.current_texture = &cache.textures[2];
    cache.textures[3].last_used_frame = 190;
    cache.textures[4].last_used_frame = 300;
    cache.textures[5].last_used_frame = ~uint64_t{0};
    cache.is_texture_transfer_ready = true;
    cache.trim_textures(context, true);
    assert(cache.texture_lookup.size() == 16);
    cache.is_texture_transfer_ready = false;
    cache.trim_textures(context, false);
    assert(cache.state.slot.destroy_queue.retained_views.size() == 8);
    assert(cache.texture_lookup.size() == 8);
    cache.trim_textures(context, false);
    assert(cache.texture_lookup.size() == 6);
    assert(cache.texture_queue.retired.size() == 10);
    for (size_t i = 0; i < 6; ++i) assert(cache.textures[i].texture.image);
    for (size_t i = 6; i < 16; ++i) {
        assert(!cache.texture_lookup.contains(i + 1));
        assert(cache.texture_queue.items[i].content.texture_size == 0);
        assert(cache.textures[i].memory_needed == 0);
    }
    cache.trim_textures(context, true); // pressure still cannot destroy bound/in-flight textures
    assert(cache.texture_lookup.size() == 6);
    VKTextureCache warm;
    for (auto &entry : warm.textures) entry.last_used_frame = 100;
    warm.trim_textures(context, false);
    assert(warm.texture_lookup.size() == 16);
    warm.trim_textures(context, true);
    assert(warm.texture_lookup.size() == 8); // age60 under pressure, capped at8 per frame
    // A lookup miss after eviction allows the original cache path to allocate
    // and upload again. The old image remains in the deferred destruction queue.
    auto &info = cache.texture_queue.items[6].content;
    cache.texture_lookup[info.texture] = &info;
    info.texture_size = 4;
    cache.textures[6] = Entry{Image{true, 77}, 4, context.frame_timestamp};
    cache.trim_textures(context, true);
    assert(cache.texture_lookup.contains(info.texture));
    assert(cache.textures[6].texture.view == 77);

    // Candidate order differs from age order. Evict only the eight oldest,
    // then stop at the budget on the next call, even with fewer than eight left.
    VKTextureCache ordered;
    const std::array<uint64_t, 16> ages{9, 3, 16, 1, 12, 7, 14, 5, 10, 2, 15, 8, 11, 4, 13, 6};
    VKContext unbound;
    for (size_t i = 0; i < ages.size(); ++i) ordered.textures[i].last_used_frame = ages[i];
    ordered.trim_textures(unbound, false);
    assert((ordered.texture_queue.retired == std::vector<uint32_t>{4, 10, 2, 14, 8, 16, 6, 12}));
    ordered.trim_textures(unbound, false);
    assert((ordered.texture_queue.retired == std::vector<uint32_t>{4, 10, 2, 14, 8, 16, 6, 12, 1, 9}));

    // Over-budget caches can have no eligible candidates, or just one.
    VKTextureCache limited;
    for (auto &entry : limited.textures) entry.last_used_frame = unbound.frame_timestamp;
    limited.trim_textures(unbound, false);
    assert(limited.texture_queue.retired.empty());
    limited.textures[15].last_used_frame = 1;
    limited.trim_textures(unbound, false);
    assert((limited.texture_queue.retired == std::vector<uint32_t>{16}));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp)
    (path / 'test.cpp').write_text(code)
    subprocess.run([sys.argv[1], '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Texture budget preserves live bindings and defers cold image destruction')
