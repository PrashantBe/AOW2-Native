// Runs the original Art of War 2 MIDlet from its JAR in an SDL2 window.

#include "natives.hpp"

#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <png.h>
#include <stdexcept>
#include <string>
#include <deque>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using vm::Value;

constexpr int AudioRate = 22050;

// Key codes the Nokia build of the game expects.
constexpr int KeyUp = -1, KeyDown = -2, KeyLeft = -3, KeyRight = -4, KeyFire = -5;
constexpr int KeySoftLeft = -6, KeySoftRight = -7;

struct Options {
    std::string jar;
    bool headless = false;
    bool commands = false; // Read commands such as "tap 20 340" from standard input.
    double seconds = 0; // Headless run time.
    unsigned long long minFrames = 0; // Headless runs fail when fewer frames were painted.
    int scale = 1;
    bool fullscreen = false;
    bool mute = false;
    std::string musicWav; // Render the music to this file and exit.
    struct Shot { double at; std::string path; };
    struct Input { double at; char kind; int a, b; }; // kind: p press, r release, d drag, k key down, u key up
    std::vector<Shot> shots;
    std::vector<Input> inputs;
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
        "Usage: aow2 <ArtOfWar2__Nokia_5800.jar> [options]\n"
        "  --saves <dir>           where saved games are kept (default: saves)\n"
        "  --size <w>x<h>          screen size reported to the game (default: 640x360)\n"
        "  --scale <n>             window size multiplier\n"
        "  --fullscreen            fill the display, keeping the game's shape\n"
        "  --verbose               log platform calls\n"
        "Testing without a window:\n"
        "  --headless <seconds>    run for this long on the game clock, then exit\n"
        "  --speed <factor>        run the game clock faster than real time\n"
        "  --shot <seconds>:<png>  save the screen at a time\n"
        "  --tap <seconds>:<x>,<y> touch and release a point\n"
        "  --key <seconds>:<code>  press and release a key code\n"
        "  --min-frames <n>        exit with an error if fewer frames were painted\n"
        "  --commands              read tap, press, release, drag, key, shot and quit\n"
        "                          commands from standard input, one per line\n");
    std::exit(2);
}

Options parse(int argc, char** argv, vm::Host& host) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (arg == "--verbose") vm::verbose = true;
        else if (arg == "--commands") options.commands = true;
        else if (arg == "--min-frames") options.minFrames = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--speed") vm::timeScale = std::max(0.1, std::atof(value().c_str()));
        else if (arg == "--saves") host.saveDirectory = value();
        else if (arg == "--scale") options.scale = std::max(1, std::atoi(value().c_str()));
        else if (arg == "--fullscreen") options.fullscreen = true;
        else if (arg == "--mute") options.mute = true;
        else if (arg == "--music-wav") options.musicWav = value();
        else if (arg == "--size") {
            if (std::sscanf(value().c_str(), "%dx%d", &host.width, &host.height) != 2) usage();
        } else if (arg == "--headless") {
            options.headless = true;
            options.seconds = std::atof(value().c_str());
        } else if (arg == "--shot") {
            const auto text = value();
            const auto colon = text.find(':');
            if (colon == std::string::npos) usage();
            options.shots.push_back({std::atof(text.c_str()), text.substr(colon + 1)});
        } else if (arg == "--tap") {
            double at;
            int x, y;
            if (std::sscanf(value().c_str(), "%lf:%d,%d", &at, &x, &y) != 3) usage();
            options.inputs.push_back({at, 'p', x, y});
            options.inputs.push_back({at + 0.15, 'r', x, y});
        } else if (arg == "--key") {
            double at;
            int code;
            if (std::sscanf(value().c_str(), "%lf:%d", &at, &code) != 2) usage();
            options.inputs.push_back({at, 'k', code, 0});
            options.inputs.push_back({at + 0.15, 'u', code, 0});
        } else if (!arg.empty() && arg[0] != '-' && options.jar.empty()) {
            options.jar = arg;
        } else {
            usage();
        }
    }
    if (options.jar.empty()) usage();
    std::stable_sort(options.inputs.begin(), options.inputs.end(),
                     [](const auto& left, const auto& right) { return left.at < right.at; });
    return options;
}

class Game {
public:
    Game(const std::string& jarPath, vm::Host& host) : jar_(jarPath), machine_(jar_), host_(host) {
        vm::defineCore(machine_);
        vm::defineMidp(machine_, host_);
        const auto* entry = jar_.property("MIDlet-1");
        if (!entry) throw std::runtime_error("The JAR has no MIDlet-1 entry");
        auto name = entry->substr(entry->rfind(',') + 1);
        name.erase(0, name.find_first_not_of(' '));
        std::replace(name.begin(), name.end(), '.', '/');
        run([&] {
            auto* cls = machine_.findClass(name);
            machine_.initClass(cls);
            host_.midlet = machine_.newInstance(cls);
            machine_.pin(host_.midlet);
            machine_.call(host_.midlet, "<init>:()V");
            machine_.call(host_.midlet, "startApp:()V");
        });
    }

    // Delivers an input event to the canvas on screen.
    void send(const char* sig, std::vector<Value> args) {
        run([&] {
            if (host_.canvas) machine_.call(host_.canvas, sig, std::move(args));
        });
    }

    void paint() {
        if (host_.repaintPending) run([&] { vm::paintIfNeeded(machine_, host_); });
    }

    void inject(const Options::Input& input) {
        switch (input.kind) {
        case 'p': send("pointerPressed:(II)V", {vm::I(input.a), vm::I(input.b)}); break;
        case 'r': send("pointerReleased:(II)V", {vm::I(input.a), vm::I(input.b)}); break;
        case 'd': send("pointerDragged:(II)V", {vm::I(input.a), vm::I(input.b)}); break;
        case 'k': send("keyPressed:(I)V", {vm::I(input.a)}); break;
        case 'u': send("keyReleased:(I)V", {vm::I(input.a)}); break;
        }
    }

private:
    template <class F> void run(F&& body) {
        machine_.lock.lock();
        try {
            body();
        } catch (const vm::JavaThrow& thrown) {
            std::fprintf(stderr, "Uncaught exception from the game: %s\n", machine_.describe(thrown.ex).c_str());
        } catch (...) {
            machine_.lock.unlock();
            throw;
        }
        machine_.lock.unlock();
    }

    Jar jar_;
    vm::VM machine_;
    vm::Host& host_;
};

void savePng(const vm::Surface& surface, const std::string& path) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(surface.width);
    image.height = static_cast<png_uint_32>(surface.height);
    image.format = PNG_FORMAT_BGRA;
    if (!png_image_write_to_file(&image, path.c_str(), 0, surface.pixels.data(), 0, nullptr)) {
        std::fprintf(stderr, "Could not write %s\n", path.c_str());
    }
}

double elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Feeds scripted input and saves screenshots as their times come up.
struct Script {
    Options& options;
    Game& game;
    vm::Host& host;
    std::size_t nextInput = 0;
    std::size_t nextShot = 0;
    void step(double time) {
        while (nextInput < options.inputs.size() && options.inputs[nextInput].at <= time) {
            game.inject(options.inputs[nextInput++]);
        }
        while (nextShot < options.shots.size() && options.shots[nextShot].at <= time) {
            std::lock_guard<std::mutex> guard(host.frameMutex);
            savePng(host.frame, options.shots[nextShot++].path);
        }
    }
};

// Commands arriving on standard input while the game runs.
class CommandQueue {
public:
    void start() {
        std::thread([this] {
            std::string line;
            while (std::getline(std::cin, line)) push(line);
            push("quit");
        }).detach();
    }
    bool pop(std::string& line) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (lines_.empty()) return false;
        line = std::move(lines_.front());
        lines_.pop_front();
        return true;
    }

private:
    void push(std::string line) {
        std::lock_guard<std::mutex> guard(mutex_);
        lines_.push_back(std::move(line));
    }
    std::mutex mutex_;
    std::deque<std::string> lines_;
};

void runCommand(const std::string& line, Game& game, vm::Host& host) {
    char word[16] = {};
    char path[512] = {};
    int x = 0, y = 0;
    if (std::sscanf(line.c_str(), "%15s", word) != 1) return;
    const std::string command = word;
    if (command == "quit") {
        host.quit = true;
    } else if (command == "shot" && std::sscanf(line.c_str(), "%*s %511s", path) == 1) {
        game.paint();
        std::lock_guard<std::mutex> guard(host.frameMutex);
        savePng(host.frame, path);
    } else if (command == "key" && std::sscanf(line.c_str(), "%*s %d", &x) == 1) {
        game.inject({0, 'k', x, 0});
        game.inject({0, 'u', x, 0});
    } else if (std::sscanf(line.c_str(), "%*s %d %d", &x, &y) == 2
               && (command == "tap" || command == "press" || command == "release" || command == "drag")) {
        if (command != "release" && command != "drag") game.inject({0, 'p', x, y});
        if (command == "drag") game.inject({0, 'd', x, y});
        if (command != "press" && command != "drag") game.inject({0, 'r', x, y});
    } else {
        std::fprintf(stderr, "Unknown command: %s\n", line.c_str());
        return;
    }
    std::printf("ok %s\n", line.c_str());
    std::fflush(stdout);
}

int runHeadless(Options& options, Game& game, vm::Host& host) {
    Script script{options, game, host};
    CommandQueue queue;
    if (options.commands) queue.start();
    const auto start = std::chrono::steady_clock::now();
    const auto gameTime = [&] { return elapsed(start) * vm::timeScale; };
    while (!host.quit && gameTime() < options.seconds) {
        script.step(gameTime());
        for (std::string line; queue.pop(line);) runCommand(line, game, host);
        game.paint();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    script.step(1e9);
    std::fprintf(stderr, "Ran %.1f s of game time, %llu frames painted\n", gameTime(),
                 static_cast<unsigned long long>(host.frameCount));
    return host.frameCount < options.minFrames ? 1 : 0;
}

int runWindow(Options& options, Game& game, vm::Host& host) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        throw std::runtime_error(SDL_GetError());
    }
    if (host.synth) {
        SDL_AudioSpec want{}, have{};
        want.freq = AudioRate;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        want.userdata = host.synth;
        want.callback = [](void* synth, Uint8* stream, int bytes) {
            static_cast<midi::Synth*>(synth)->render(reinterpret_cast<std::int16_t*>(stream), bytes / 2);
        };
        // The game keeps running without sound if the device cannot be opened.
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 || SDL_OpenAudio(&want, &have) != 0) {
            std::fprintf(stderr, "No sound: %s\n", SDL_GetError());
        } else {
            SDL_PauseAudio(0);
        }
    }
    auto* window = SDL_CreateWindow("Art of War 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    host.width * options.scale, host.height * options.scale,
                                    options.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE);
    auto* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer && window) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    // The game's screen keeps its shape. Any spare room on the display is left black.
    SDL_RenderSetLogicalSize(renderer, host.width, host.height);
    auto* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, host.width, host.height);
    if (!texture) throw std::runtime_error(SDL_GetError());

    SDL_GameController* pad = nullptr;
    float cursorX = host.width / 2.0f, cursorY = host.height / 2.0f;
    bool cursorShown = false, touching = false, mouseDown = false;
    std::uint64_t shownFrame = ~0ull;
    Script script{options, game, host};
    const auto start = std::chrono::steady_clock::now();
    auto last = start;

    const auto key = [&](int code, bool down) {
        game.send(down ? "keyPressed:(I)V" : "keyReleased:(I)V", {vm::I(code)});
    };
    const auto touch = [&](const char* sig) {
        game.send(sig, {vm::I(static_cast<int>(cursorX)), vm::I(static_cast<int>(cursorY))});
    };

    while (!host.quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: host.quit = true; break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!pad) pad = SDL_GameControllerOpen(event.cdevice.which);
                break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mouseDown = event.type == SDL_MOUSEBUTTONDOWN;
                    cursorShown = false;
                    game.send(mouseDown ? "pointerPressed:(II)V" : "pointerReleased:(II)V",
                              {vm::I(event.button.x), vm::I(event.button.y)});
                }
                break;
            case SDL_MOUSEMOTION:
                if (mouseDown) game.send("pointerDragged:(II)V", {vm::I(event.motion.x), vm::I(event.motion.y)});
                break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                if (event.key.repeat) break;
                const bool down = event.type == SDL_KEYDOWN;
                const auto symbol = event.key.keysym.sym;
                if (symbol == SDLK_ESCAPE) host.quit = true;
                else if (symbol == SDLK_UP) key(KeyUp, down);
                else if (symbol == SDLK_DOWN) key(KeyDown, down);
                else if (symbol == SDLK_LEFT) key(KeyLeft, down);
                else if (symbol == SDLK_RIGHT) key(KeyRight, down);
                else if (symbol == SDLK_RETURN || symbol == SDLK_SPACE) key(KeyFire, down);
                else if (symbol == SDLK_F1) key(KeySoftLeft, down);
                else if (symbol == SDLK_F2) key(KeySoftRight, down);
                else if (symbol >= SDLK_0 && symbol <= SDLK_9) key(symbol, down);
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
                const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: key(KeyUp, down); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: key(KeyDown, down); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: key(KeyLeft, down); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key(KeyRight, down); break;
                case SDL_CONTROLLER_BUTTON_A:
                    cursorShown = true;
                    touching = down;
                    touch(down ? "pointerPressed:(II)V" : "pointerReleased:(II)V");
                    break;
                case SDL_CONTROLLER_BUTTON_B: key(KeySoftRight, down); break;
                case SDL_CONTROLLER_BUTTON_X: key(KeySoftLeft, down); break;
                case SDL_CONTROLLER_BUTTON_Y: key(KeyFire, down); break;
                case SDL_CONTROLLER_BUTTON_START: key(KeySoftLeft, down); break;
                // Phone number keys used by the game's panels: 1 rally point, 3 destroy, 0 info.
                // Holding R2 turns L1 and R1 into 7 and 9, which some selection panels use.
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                case SDL_CONTROLLER_BUTTON_BACK: {
                    static int held[SDL_CONTROLLER_BUTTON_MAX] = {};
                    const int button = event.cbutton.button;
                    if (down) {
                        const bool shift = pad && SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
                        held[button] = button == SDL_CONTROLLER_BUTTON_BACK ? '0'
                                     : button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? (shift ? '7' : '1')
                                     : (shift ? '9' : '3');
                    }
                    if (held[button]) key(held[button], down);
                    if (!down) held[button] = 0;
                    break;
                }
                default: break;
                }
                break;
            }
            default: break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - last).count();
        last = now;
        if (pad) {
            // The stick steers a pointer that stands in for a finger on the touch screen.
            const auto axis = [&](SDL_GameControllerAxis which) {
                const float value = SDL_GameControllerGetAxis(pad, which) / 32767.0f;
                return std::abs(value) < 0.2f ? 0.0f : value;
            };
            const float dx = axis(SDL_CONTROLLER_AXIS_LEFTX), dy = axis(SDL_CONTROLLER_AXIS_LEFTY);
            if (dx != 0 || dy != 0) {
                const bool slow = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
                const float speed = (slow ? 110.0f : 420.0f) * seconds;
                cursorX = std::clamp(cursorX + dx * speed, 0.0f, host.width - 1.0f);
                cursorY = std::clamp(cursorY + dy * speed, 0.0f, host.height - 1.0f);
                cursorShown = true;
                if (touching) touch("pointerDragged:(II)V");
            }
        }

        script.step(elapsed(start));
        game.paint();
        {
            std::lock_guard<std::mutex> guard(host.frameMutex);
            if (host.frameCount != shownFrame) {
                SDL_UpdateTexture(texture, nullptr, host.frame.pixels.data(), host.width * 4);
                shownFrame = host.frameCount;
            }
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        if (cursorShown) {
            const int x = static_cast<int>(cursorX), y = static_cast<int>(cursorY);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            const SDL_Rect shadow[] = {{x - 9, y - 2, 19, 5}, {x - 2, y - 9, 5, 19}};
            SDL_RenderFillRects(renderer, shadow, 2);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            const SDL_Rect cross[] = {{x - 8, y - 1, 17, 3}, {x - 1, y - 8, 3, 17}};
            SDL_RenderFillRects(renderer, cross, 2);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(4);
    }
    SDL_Quit();
    return 0;
}

// Writes the game's music, rendered by the built-in synthesizer, to a 16-bit mono WAV file.
int renderMusic(const std::string& jarPath, const std::string& path, double seconds) {
    const Jar jar(jarPath);
    const auto* data = jar.find("s0m");
    const auto song = data ? midi::parse(*data) : nullptr;
    if (!song) {
        std::fprintf(stderr, "No playable music in %s\n", jarPath.c_str());
        return 1;
    }
    midi::Synth synth(AudioRate);
    synth.play(song, 1);
    std::vector<std::int16_t> samples(static_cast<std::size_t>(seconds * AudioRate));
    synth.render(samples.data(), static_cast<int>(samples.size()));
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return 1;
    const auto word = [&](std::uint32_t value, int bytes) { std::fwrite(&value, 1, bytes, file); };
    const auto size = static_cast<std::uint32_t>(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, file); word(36 + size, 4); std::fwrite("WAVEfmt ", 1, 8, file);
    word(16, 4); word(1, 2); word(1, 2); word(AudioRate, 4); word(AudioRate * 2, 4); word(2, 2); word(16, 2);
    std::fwrite("data", 1, 4, file); word(size, 4);
    std::fwrite(samples.data(), 2, samples.size(), file);
    std::fclose(file);
    std::fprintf(stderr, "Wrote %.0f s of music to %s\n", seconds, path.c_str());
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        vm::Host host;
        auto options = parse(argc, argv, host);
        if (!options.musicWav.empty()) {
            return renderMusic(options.jar, options.musicWav, 30);
        }
        // Music plays only in a window, and not when muted.
        static midi::Synth synth(AudioRate);
        if (!options.headless && !options.mute) host.synth = &synth;
        Game game(options.jar, host);
        const int result = options.headless ? runHeadless(options, game, host) : runWindow(options, game, host);
        std::fflush(nullptr);
        // Game threads are still running, so leave without unwinding them.
        std::_Exit(result);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        std::_Exit(1);
    }
}
