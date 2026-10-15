#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <linux/input.h>
#include <string_view>
#include <vector>

namespace Windchime {
constexpr int Rate = 48000;
constexpr int NoteCount = 10;
constexpr double Pi = 3.14159265358979323846;
inline bool parseConfig(std::string_view line, int &density, bool &enabled) {
    if (!line.starts_with("C "))
        return false;
    size_t split = line.find(' ', 2);
    if (split == line.npos)
        return false;
    auto result = std::from_chars(line.data() + 2, line.data() + split, density);
    auto on = line.substr(split + 1);
    if (result.ec != std::errc() || result.ptr != line.data() + split || density < 1 ||
        density > 10 || (on != "0" && on != "1"))
        return false;
    enabled = on == "1";
    return true;
}
inline int letterIndex(unsigned code) {
    constexpr std::array<unsigned, 26> letters = {
        KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
        KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
    for (int i = 0; i < 26; i++)
        if (letters[i] == code)
            return i;
    return -1;
}
inline int noteForLetter(int i) {
    return (i * 7 + 3) % NoteCount;
}
struct LetterGate {
    int every = 5, count = 0;
    void reset() {
        count = 0;
    }
    void configure(int n) {
        if (n != every) {
            every = std::clamp(n, 1, 10);
            reset();
        }
    }
    int press(unsigned code, int value, bool shortcut) {
        if (shortcut) {
            reset();
            return -1;
        }
        if (code == KEY_BACKSPACE || code == KEY_DELETE || code == KEY_LEFT || code == KEY_RIGHT ||
            code == KEY_UP || code == KEY_DOWN || code == KEY_HOME || code == KEY_END ||
            code == KEY_ESC) {
            reset();
            return -1;
        }
        int i = letterIndex(code);
        if (i < 0 || value != 1)
            return -1; // Physical press only; no held-key storms.
        if (++count < every)
            return -1;
        count = 0;
        return noteForLetter(i);
    }
};
struct NoteQueue {
    struct Item {
        int note;
        double strength;
        int64_t when;
    };
    std::deque<Item> items;
    int spacing = 150;
    int64_t last = -100000;
    void clear() {
        items.clear();
        last = -100000;
    }
    void push(int note, double strength, int64_t now) {
        // Keep a short musical tail, never a long replay of stale typing.
        while (!items.empty() && now - items.front().when > 500)
            items.pop_front();
        if (items.size() >= 4)
            items.pop_front();
        items.push_back({note, std::clamp(strength, 0.1, 1.0), now});
    }
    bool pop(int64_t now, Item &out) {
        while (!items.empty() && now - items.front().when > 500)
            items.pop_front();
        if (items.empty() || now - last < spacing)
            return false;
        out = items.front();
        items.pop_front();
        last = now;
        return true;
    }
};
class Mixer {
    struct Voice {
        int note;
        size_t pos;
        float left, right;
    };
    std::array<std::vector<float>, NoteCount> bank;
    std::vector<Voice> voices;
    float gain = .18f;
    unsigned strikes = 0;

  public:
    Mixer() {
        voices.reserve(12);
        rebuild(0, 3.0);
    }
    void volume(float v) {
        gain = std::clamp(v, 0.f, 1.f);
    }
    void clear() {
        voices.clear();
    }
    bool active() const {
        return !voices.empty();
    }
    void rebuild(int tone, double decay) {
        clear();
        decay = std::clamp(decay, 1.0, 6.0);
        constexpr int notes[NoteCount] = {60, 62, 64, 67, 69, 72, 74, 76, 79, 81};
        constexpr double ratio[4] = {1.0, 2.756, 5.404, 8.933};
        const double bright = tone == 2 ? .6 : (tone == 1 ? .38 : .22);
        const size_t length = size_t(Rate * decay);
        for (int n = 0; n < NoteCount; n++) {
            auto &s = bank[n];
            s.resize(length);
            double f = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0);
            for (size_t i = 0; i < length; i++) {
                double t = double(i) / Rate, value = 0;
                for (int p = 0; p < 4; p++) {
                    double amp = p == 0 ? .72 : bright / std::pow(1.9, p - 1);
                    double damp = std::exp(-t * (p == 0 ? 5.0 : 8.0 + 4 * p) / decay);
                    value += amp * damp * std::sin(2 * Pi * f * ratio[p] * t);
                }
                double attack = 1 - std::exp(-t / 0.0035);
                double end = std::min(1.0, (length - i) / double(Rate * .08));
                s[i] = float(value * attack * end * .95);
            }
        }
    }
    void strike(int note, double strength) {
        if (note < 0 || note >= NoteCount)
            return;
        if (voices.size() == 12)
            voices.erase(voices.begin());
        double pan = .5 + .22 * std::sin(++strikes * 2.399963);
        voices.push_back(
            {note, 0, float(strength * std::sqrt(1 - pan)), float(strength * std::sqrt(pan))});
    }
    void render(float *dst, int frames) {
        std::fill(dst, dst + frames * 2, 0.f);
        for (auto &v : voices) {
            const auto &s = bank[v.note];
            int available = std::min<size_t>(frames, s.size() - v.pos);
            for (int i = 0; i < available; i++) {
                float a = s[v.pos++] * gain;
                dst[2 * i] += a * v.left;
                dst[2 * i + 1] += a * v.right;
            }
        }
        voices.erase(std::remove_if(voices.begin(), voices.end(),
                                    [&](auto &v) { return v.pos >= bank[v.note].size(); }),
                     voices.end());
        for (int i = 0; i < frames * 2; i++)
            dst[i] = dst[i] / (1 + std::abs(dst[i]));
    }
};
} // namespace Windchime
