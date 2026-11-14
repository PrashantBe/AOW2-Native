#pragma once

#include "midi.hpp"
#include "vm.hpp"

#include <atomic>

namespace vm {

extern bool verbose; // Log resource, property and platform calls to stderr.
extern double timeScale; // Game clock speed. Above 1 runs the game faster than real time, for testing.
// Milliseconds on the game's clock.
std::int64_t gameMillis();

void defineCore(VM& vm);

Array* newByteArray(VM& vm, const Bytes& data);
Bytes bytesOf(Array* array, int offset, int length);
const Bytes* findResource(VM& vm, std::string name);
// A java.io.InputStream over a JAR entry, or nullptr when the entry is missing.
Object* openResource(VM& vm, const std::string& name);
Bytes drain(VM& vm, Object* stream);

// Pixels are 0xAARRGGBB.
struct Surface {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
    void resize(int w, int h, std::uint32_t fill) {
        width = w;
        height = h;
        pixels.assign(static_cast<std::size_t>(w) * h, fill);
    }
};

// State shared between the phone API and the program that shows it on screen.
struct Host {
    int width = 640;
    int height = 360;
    std::string saveDirectory = "saves";

    // The finished frame. Guarded by frameMutex because the game paints on its own thread.
    std::mutex frameMutex;
    Surface frame;
    std::uint64_t frameCount = 0;

    std::atomic<bool> quit{false};
    std::atomic<bool> repaintPending{false};
    Object* midlet = nullptr;
    Object* canvas = nullptr;  // The Canvas being shown, if any.
    Object* current = nullptr; // The Displayable being shown, canvas or form.
    midi::Synth* synth = nullptr; // Music output, or nullptr when running without sound.
};

void defineMidp(VM& vm, Host& host);
// Runs the canvas's paint method if a repaint was requested. Call with the VM lock held.
void paintIfNeeded(VM& vm, Host& host);

} // namespace vm
