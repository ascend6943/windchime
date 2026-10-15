#include "core.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
using namespace Windchime;
static void wav(const char *path, const std::vector<float> &samples) {
    std::ofstream f(path, std::ios::binary);
    uint32_t bytes = samples.size() * sizeof(int16_t), length = 36 + bytes, fmtSize = 16,
             rate = Rate, byteRate = Rate * 4;
    uint16_t format = 1, channels = 2, align = 4, bits = 16;
    f.write("RIFF", 4);
    f.write((char *)&length, 4);
    f.write("WAVEfmt ", 8);
    f.write((char *)&fmtSize, 4);
    f.write((char *)&format, 2);
    f.write((char *)&channels, 2);
    f.write((char *)&rate, 4);
    f.write((char *)&byteRate, 4);
    f.write((char *)&align, 2);
    f.write((char *)&bits, 2);
    f.write("data", 4);
    f.write((char *)&bytes, 4);
    for (float x : samples) {
        int16_t y = int16_t(std::clamp(x, -1.f, 1.f) * 32767);
        f.write((char *)&y, 2);
    }
}
int main(int argc, char **argv) {
    int density;
    bool enabled;
    assert(parseConfig("C 10 1", density, enabled) && density == 10 && enabled);
    assert(parseConfig("C 1 0", density, enabled) && density == 1 && !enabled);
    for (auto bad : {"C 0 1", "C 11 1", "C 5 2", "C 5 1 junk", "C 999999999999999999999999999999 1",
                     "C -1 1", "C  5 1", "", "X 5 1"})
        assert(!parseConfig(bad, density, enabled));
    LetterGate g;
    for (int i = 0; i < 4; i++)
        assert(g.press(KEY_A, 1, false) == -1);
    assert(g.press(KEY_B, 1, false) == noteForLetter(1));
    assert(g.press(KEY_A, 2, false) == -1);
    assert(g.count == 0);
    g.configure(1);
    assert(g.press(KEY_Z, 1, false) == noteForLetter(25));
    assert(g.press(KEY_A, 1, true) == -1);
    assert(g.press(KEY_SPACE, 1, false) == -1);
    g.configure(5);
    g.press(KEY_A, 1, false);
    g.press(KEY_B, 1, false);
    g.press(KEY_BACKSPACE, 1, false);
    assert(g.count == 0);
    for (int d = 1; d <= 10; d++) {
        g.configure(d);
        g.reset();
        int notes = 0;
        for (int i = 0; i < 100; i++)
            notes += g.press(KEY_A, 1, false) >= 0;
        assert(notes == 100 / d);
    }
    NoteQueue q;
    NoteQueue::Item n;
    for (int i = 0; i < 20; i++)
        q.push(i % 10, .8, 0);
    assert(q.items.size() == 4);
    assert(q.pop(0, n));
    assert(!q.pop(149, n));
    assert(q.pop(150, n));
    assert(!q.pop(700, n));
    assert(q.items.empty());
    q.push(0, .8, 1000);
    q.clear();
    assert(!q.pop(1000, n));
    Mixer m;
    m.volume(.7);
    for (int i = 0; i < 24; i++)
        m.strike(i % 10, 1);
    std::vector<float> block(Rate * 2);
    m.render(block.data(), Rate);
    float peak = 0;
    for (float x : block) {
        assert(std::isfinite(x));
        assert(std::abs(x) < 1);
        peak = std::max(peak, std::abs(x));
    }
    assert(peak > .01);
    for (int i = 0; i < 6; i++)
        m.render(block.data(), Rate);
    assert(!m.active());
    m.volume(0);
    m.strike(0, .8);
    m.render(block.data(), Rate);
    for (float x : block)
        assert(x == 0);
    m.clear();
    assert(!m.active());
    if (argc == 2) {
        m.volume(.3);
        m.rebuild(0, 3);
        std::vector<float> out(Rate * 2 * 6), part(Rate * 2 * 6 / 10);
        int melody[] = {0, 3, 1, 4, 6, 2};
        for (int i = 0; i < 10; i++) {
            if (i < 6)
                m.strike(melody[i], .8);
            m.render(part.data(), Rate * 6 / 10);
            std::copy(part.begin(), part.end(), out.begin() + i * part.size());
        }
        wav(argv[1], out);
    }
    puts("PASS: protocol bounds, density, shortcut filtering, editing, bounded playback, finite "
         "audio, overlap limiting, decay and mute.");
}
