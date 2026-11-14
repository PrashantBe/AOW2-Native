// MIDP 2.0: the screen, images, saved records, sound players and the MIDlet itself.

#include "natives.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <png.h>

namespace vm {

namespace {
Host* host = nullptr;
Object* musicOwner = nullptr; // The player whose song the synthesizer is playing. Only compared, never used.
Surface screen; // What the canvas paints on. Copied to Host::frame after each paint.

struct ImageData : Native {
    Surface surface;
    bool isMutable = false;
};

struct GraphicsData : Native {
    Object* owner = nullptr; // The image being drawn on, or nullptr for the screen.
    Surface* target = nullptr;
    std::uint32_t color = 0xFF000000;
    int clipX = 0, clipY = 0, clipW = 0, clipH = 0;
    int translateX = 0, translateY = 0;
    void trace(Marker& out) override { out.push_back(owner); }
};

// Commands, listeners and form contents of any Displayable or Item.
struct WidgetData : Native {
    std::vector<Object*> items;
    std::vector<Object*> commands;
    Object* listener = nullptr;
    Object* itemListener = nullptr;
    std::u16string text;
    int constraints = 0;
    std::vector<char> selected;
    void trace(Marker& out) override {
        out.insert(out.end(), items.begin(), items.end());
        out.insert(out.end(), commands.begin(), commands.end());
        out.push_back(listener);
        out.push_back(itemListener);
    }
};

struct PlayerData : Native {
    int state = 100;
    int loops = 1;
    int level = 100;
    std::shared_ptr<const midi::Song> song;
    std::string type;
    std::vector<Object*> listeners;
    void trace(Marker& out) override { out.insert(out.end(), listeners.begin(), listeners.end()); }
};

struct ControlData : Native {
    Object* player = nullptr;
    void trace(Marker& out) override { out.push_back(player); }
};

struct RecordStoreData : Native {
    std::string name;
};

struct Store {
    std::vector<std::pair<int, Bytes>> records;
    int nextId = 1;
};
std::map<std::string, Store> stores;

Instance* instance(Object* object) {
    return static_cast<Instance*>(object);
}

WidgetData& widget(VM& vm, Object* object) {
    if (!object) vm.throwNew("java/lang/NullPointerException");
    if (!instance(object)->native) instance(object)->native = std::make_unique<WidgetData>();
    return vm.payload<WidgetData>(object);
}

// ---------------------------------------------------------------- drawing

struct Rect {
    int x0, y0, x1, y1; // Half-open.
};

Rect clipOf(const GraphicsData& g) {
    return {std::max(0, g.clipX), std::max(0, g.clipY),
            std::min(g.target->width, g.clipX + g.clipW), std::min(g.target->height, g.clipY + g.clipH)};
}

void fill(GraphicsData& g, int x, int y, int w, int h) {
    const auto clip = clipOf(g);
    const int x0 = std::max(clip.x0, x + g.translateX), x1 = std::min(clip.x1, x + g.translateX + w);
    const int y0 = std::max(clip.y0, y + g.translateY), y1 = std::min(clip.y1, y + g.translateY + h);
    for (int row = y0; row < y1; ++row) {
        std::fill(g.target->pixels.begin() + row * g.target->width + x0,
                  g.target->pixels.begin() + row * g.target->width + std::max(x0, x1), g.color);
    }
}

void line(GraphicsData& g, int x0, int y0, int x1, int y1) {
    const auto clip = clipOf(g);
    x0 += g.translateX; x1 += g.translateX;
    y0 += g.translateY; y1 += g.translateY;
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int stepX = x0 < x1 ? 1 : -1, stepY = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        if (x0 >= clip.x0 && x0 < clip.x1 && y0 >= clip.y0 && y0 < clip.y1) {
            g.target->pixels[y0 * g.target->width + x0] = g.color;
        }
        if (x0 == x1 && y0 == y1) break;
        const int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += stepX; }
        if (twice <= dx) { error += dx; y0 += stepY; }
    }
}

std::uint32_t blend(std::uint32_t under, std::uint32_t over) {
    const std::uint32_t alpha = over >> 24;
    if (alpha == 255) return over;
    const std::uint32_t rest = 255 - alpha;
    const std::uint32_t red = ((over >> 16 & 255) * alpha + (under >> 16 & 255) * rest) / 255;
    const std::uint32_t green = ((over >> 8 & 255) * alpha + (under >> 8 & 255) * rest) / 255;
    const std::uint32_t blue = ((over & 255) * alpha + (under & 255) * rest) / 255;
    return 0xFF000000 | red << 16 | green << 8 | blue;
}

// Draws a w by h region of the source, turned by a MIDP Sprite transform, with its top-left at (dx, dy).
void blit(GraphicsData& g, const Surface& source, int sx, int sy, int w, int h, int transform, int dx, int dy) {
    const bool turned = transform >= 4;
    const int outW = turned ? h : w, outH = turned ? w : h;
    dx += g.translateX;
    dy += g.translateY;
    const auto clip = clipOf(g);
    const int x0 = std::max(clip.x0, dx), x1 = std::min(clip.x1, dx + outW);
    const int y0 = std::max(clip.y0, dy), y1 = std::min(clip.y1, dy + outH);
    for (int y = y0; y < y1; ++y) {
        auto* out = g.target->pixels.data() + y * g.target->width;
        const int v = y - dy;
        for (int x = x0; x < x1; ++x) {
            const int u = x - dx;
            int px, py;
            switch (transform) {
            case 1: px = u; py = h - 1 - v; break;         // Mirrored and turned 180 degrees.
            case 2: px = w - 1 - u; py = v; break;         // Mirrored.
            case 3: px = w - 1 - u; py = h - 1 - v; break; // Turned 180 degrees.
            case 4: px = v; py = u; break;                 // Mirrored and turned 270 degrees.
            case 5: px = v; py = h - 1 - u; break;         // Turned 90 degrees.
            case 6: px = w - 1 - v; py = u; break;         // Turned 270 degrees.
            case 7: px = w - 1 - v; py = h - 1 - u; break; // Mirrored and turned 90 degrees.
            default: px = u; py = v; break;
            }
            px += sx;
            py += sy;
            if (px < 0 || py < 0 || px >= source.width || py >= source.height) continue;
            const std::uint32_t pixel = source.pixels[py * source.width + px];
            if (pixel >> 24) out[x] = blend(out[x], pixel);
        }
    }
}

void anchor(int flags, int w, int h, int& x, int& y) {
    if (flags & 1) x -= w / 2;
    else if (flags & 8) x -= w;
    if (flags & 2) y -= h / 2;
    else if (flags & 32) y -= h;
}

bool decodePng(const std::uint8_t* data, std::size_t size, Surface& out) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data, size)) return false;
    image.format = PNG_FORMAT_BGRA;
    out.resize(static_cast<int>(image.width), static_cast<int>(image.height), 0);
    if (!png_image_finish_read(&image, nullptr, out.pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }
    return true;
}

Object* newImage(VM& vm, std::unique_ptr<ImageData> data) {
    vm.charge(data->surface.pixels.size() * 4);
    return vm.make("javax/microedition/lcdui/Image", std::move(data));
}

Object* newGraphics(VM& vm, Surface* target, Object* owner) {
    auto data = std::make_unique<GraphicsData>();
    data->owner = owner;
    data->target = target;
    data->clipW = target->width;
    data->clipH = target->height;
    return vm.make("javax/microedition/lcdui/Graphics", std::move(data));
}

void defineGraphics(VM& vm) {
    const char* image = "javax/microedition/lcdui/Image";
    vm.defStatic(image, "createImage:(II)Ljavax/microedition/lcdui/Image;", [](VM& vm, Value* a) {
        if (a[0].i <= 0 || a[1].i <= 0) vm.throwNew("java/lang/IllegalArgumentException");
        auto data = std::make_unique<ImageData>();
        data->surface.resize(a[0].i, a[1].i, 0xFFFFFFFF);
        data->isMutable = true;
        return A(newImage(vm, std::move(data)));
    });
    vm.defStatic(image, "createImage:(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;", [](VM& vm, Value* a) {
        const auto name = narrow(vm.str(a[0].a));
        const auto* bytes = findResource(vm, name);
        auto data = std::make_unique<ImageData>();
        if (!bytes || !decodePng(bytes->data(), bytes->size(), data->surface)) {
            if (verbose) std::fprintf(stderr, "[runtime] image %s could not be loaded\n", name.c_str());
            vm.throwNew("java/io/IOException", name);
        }
        return A(newImage(vm, std::move(data)));
    });
    vm.defStatic(image, "createImage:([BII)Ljavax/microedition/lcdui/Image;", [](VM& vm, Value* a) {
        auto* bytes = static_cast<Array*>(a[0].a);
        if (!bytes) vm.throwNew("java/lang/NullPointerException");
        if (a[1].i < 0 || a[2].i < 0 || a[1].i > bytes->length - a[2].i) vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
        auto data = std::make_unique<ImageData>();
        if (!decodePng(reinterpret_cast<std::uint8_t*>(bytes->b()) + a[1].i, a[2].i, data->surface)) {
            vm.throwNew("java/lang/IllegalArgumentException");
        }
        return A(newImage(vm, std::move(data)));
    });
    vm.defStatic(image, "createRGBImage:([IIIZ)Ljavax/microedition/lcdui/Image;", [](VM& vm, Value* a) {
        auto* pixels = static_cast<Array*>(a[0].a);
        if (!pixels) vm.throwNew("java/lang/NullPointerException");
        if (a[1].i <= 0 || a[2].i <= 0) vm.throwNew("java/lang/IllegalArgumentException");
        if (static_cast<std::int64_t>(a[1].i) * a[2].i > pixels->length) vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
        auto data = std::make_unique<ImageData>();
        data->surface.resize(a[1].i, a[2].i, 0);
        for (std::size_t i = 0; i < data->surface.pixels.size(); ++i) {
            const auto pixel = static_cast<std::uint32_t>(pixels->i()[i]);
            data->surface.pixels[i] = a[3].i ? pixel : pixel | 0xFF000000;
        }
        return A(newImage(vm, std::move(data)));
    });
    vm.def(image, "getWidth:()I", [](VM& vm, Value* a) { return I(vm.payload<ImageData>(a[0].a).surface.width); });
    vm.def(image, "getHeight:()I", [](VM& vm, Value* a) { return I(vm.payload<ImageData>(a[0].a).surface.height); });
    vm.def(image, "isMutable:()Z", [](VM& vm, Value* a) { return I(vm.payload<ImageData>(a[0].a).isMutable); });
    vm.def(image, "getGraphics:()Ljavax/microedition/lcdui/Graphics;", [](VM& vm, Value* a) {
        auto& data = vm.payload<ImageData>(a[0].a);
        if (!data.isMutable) vm.throwNew("java/lang/IllegalStateException");
        return A(newGraphics(vm, &data.surface, a[0].a));
    });
    vm.def(image, "getRGB:([IIIIIII)V", [](VM& vm, Value* a) {
        auto& surface = vm.payload<ImageData>(a[0].a).surface;
        auto* out = static_cast<Array*>(a[1].a);
        if (!out) vm.throwNew("java/lang/NullPointerException");
        const int offset = a[2].i, scan = a[3].i, x = a[4].i, y = a[5].i, w = a[6].i, h = a[7].i;
        if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > surface.width || y + h > surface.height) {
            vm.throwNew("java/lang/IllegalArgumentException");
        }
        for (int row = 0; row < h; ++row) {
            for (int column = 0; column < w; ++column) {
                const std::int64_t index = offset + static_cast<std::int64_t>(row) * scan + column;
                if (index < 0 || index >= out->length) vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
                out->i()[index] = static_cast<std::int32_t>(surface.pixels[(y + row) * surface.width + x + column]);
            }
        }
        return Value();
    });

    const char* graphics = "javax/microedition/lcdui/Graphics";
    vm.def(graphics, "setColor:(I)V", [](VM& vm, Value* a) {
        vm.payload<GraphicsData>(a[0].a).color = 0xFF000000 | static_cast<std::uint32_t>(a[1].i);
        return Value();
    });
    vm.def(graphics, "setColor:(III)V", [](VM& vm, Value* a) {
        vm.payload<GraphicsData>(a[0].a).color = 0xFF000000 | static_cast<std::uint32_t>((a[1].i & 255) << 16 | (a[2].i & 255) << 8 | (a[3].i & 255));
        return Value();
    });
    vm.def(graphics, "getColor:()I", [](VM& vm, Value* a) {
        return I(static_cast<std::int32_t>(vm.payload<GraphicsData>(a[0].a).color & 0xFFFFFF));
    });
    vm.def(graphics, "setClip:(IIII)V", [](VM& vm, Value* a) {
        auto& g = vm.payload<GraphicsData>(a[0].a);
        g.clipX = a[1].i + g.translateX;
        g.clipY = a[2].i + g.translateY;
        g.clipW = std::max(0, a[3].i);
        g.clipH = std::max(0, a[4].i);
        return Value();
    });
    vm.def(graphics, "clipRect:(IIII)V", [](VM& vm, Value* a) {
        auto& g = vm.payload<GraphicsData>(a[0].a);
        const int x0 = std::max(g.clipX, a[1].i + g.translateX), y0 = std::max(g.clipY, a[2].i + g.translateY);
        const int x1 = std::min(g.clipX + g.clipW, a[1].i + g.translateX + a[3].i);
        const int y1 = std::min(g.clipY + g.clipH, a[2].i + g.translateY + a[4].i);
        g.clipX = x0;
        g.clipY = y0;
        g.clipW = std::max(0, x1 - x0);
        g.clipH = std::max(0, y1 - y0);
        return Value();
    });
    vm.def(graphics, "getClipX:()I", [](VM& vm, Value* a) {
        auto& g = vm.payload<GraphicsData>(a[0].a);
        return I(g.clipX - g.translateX);
    });
    vm.def(graphics, "getClipY:()I", [](VM& vm, Value* a) {
        auto& g = vm.payload<GraphicsData>(a[0].a);
        return I(g.clipY - g.translateY);
    });
    vm.def(graphics, "getClipWidth:()I", [](VM& vm, Value* a) { return I(vm.payload<GraphicsData>(a[0].a).clipW); });
    vm.def(graphics, "getClipHeight:()I", [](VM& vm, Value* a) { return I(vm.payload<GraphicsData>(a[0].a).clipH); });
    vm.def(graphics, "translate:(II)V", [](VM& vm, Value* a) {
        auto& g = vm.payload<GraphicsData>(a[0].a);
        g.translateX += a[1].i;
        g.translateY += a[2].i;
        return Value();
    });
    vm.def(graphics, "fillRect:(IIII)V", [](VM& vm, Value* a) {
        fill(vm.payload<GraphicsData>(a[0].a), a[1].i, a[2].i, a[3].i, a[4].i);
        return Value();
    });
    vm.def(graphics, "drawRect:(IIII)V", [](VM& vm, Value* a) {
        auto& g = vm.payload<GraphicsData>(a[0].a);
        const int x = a[1].i, y = a[2].i, w = a[3].i, h = a[4].i;
        if (w < 0 || h < 0) return Value();
        fill(g, x, y, w + 1, 1);
        fill(g, x, y + h, w + 1, 1);
        fill(g, x, y, 1, h + 1);
        fill(g, x + w, y, 1, h + 1);
        return Value();
    });
    vm.def(graphics, "drawLine:(IIII)V", [](VM& vm, Value* a) {
        line(vm.payload<GraphicsData>(a[0].a), a[1].i, a[2].i, a[3].i, a[4].i);
        return Value();
    });
    vm.def(graphics, "drawImage:(Ljavax/microedition/lcdui/Image;III)V", [](VM& vm, Value* a) {
        auto& g = vm.payload<GraphicsData>(a[0].a);
        auto& source = vm.payload<ImageData>(a[1].a).surface;
        int x = a[2].i, y = a[3].i;
        anchor(a[4].i, source.width, source.height, x, y);
        blit(g, source, 0, 0, source.width, source.height, 0, x, y);
        return Value();
    });
    vm.def(graphics, "drawRegion:(Ljavax/microedition/lcdui/Image;IIIIIIII)V", [](VM& vm, Value* a) {
        auto& g = vm.payload<GraphicsData>(a[0].a);
        auto& source = vm.payload<ImageData>(a[1].a).surface;
        const int sx = a[2].i, sy = a[3].i, w = a[4].i, h = a[5].i, transform = a[6].i;
        if (sx < 0 || sy < 0 || w < 0 || h < 0 || sx + w > source.width || sy + h > source.height
            || transform < 0 || transform > 7) {
            vm.throwNew("java/lang/IllegalArgumentException");
        }
        int x = a[7].i, y = a[8].i;
        anchor(a[9].i, transform >= 4 ? h : w, transform >= 4 ? w : h, x, y);
        blit(g, source, sx, sy, w, h, transform, x, y);
        return Value();
    });
    vm.def(graphics, "drawString:(Ljava/lang/String;III)V", [](VM& vm, Value* a) {
        if (verbose) std::fprintf(stderr, "[runtime] system font text is not drawn: %s\n", narrow(vm.str(a[1].a)).c_str());
        return Value();
    });
    vm.def(graphics, "setFont:(Ljavax/microedition/lcdui/Font;)V", [](VM&, Value*) { return Value(); });

    const char* font = "javax/microedition/lcdui/Font";
    vm.defStatic(font, "getDefaultFont:()Ljavax/microedition/lcdui/Font;", [](VM& vm, Value*) {
        static Object* shared = nullptr;
        if (!shared) {
            shared = vm.make("javax/microedition/lcdui/Font", nullptr);
            vm.pin(shared);
        }
        return A(shared);
    });
    vm.defStatic(font, "getFont:(III)Ljavax/microedition/lcdui/Font;", [](VM& vm, Value*) {
        return A(vm.make("javax/microedition/lcdui/Font", nullptr));
    });
    vm.def(font, "getHeight:()I", [](VM&, Value*) { return I(20); });
    vm.def(font, "stringWidth:(Ljava/lang/String;)I", [](VM& vm, Value* a) {
        return I(10 * static_cast<int>(vm.str(a[1].a).size()));
    });
}

// ---------------------------------------------------------------- display

void defineDisplay(VM& vm) {
    const char* displayable = "javax/microedition/lcdui/Displayable";
    const char* canvas = "javax/microedition/lcdui/Canvas";
    const char* screenClass = "javax/microedition/lcdui/Screen";
    vm.defineNative(displayable);
    vm.defineNative(canvas, displayable);
    vm.defineNative(screenClass, displayable);
    vm.defineNative("javax/microedition/lcdui/CommandListener");
    vm.defineNative("javax/microedition/lcdui/ItemStateListener");

    vm.def(displayable, "getWidth:()I", [](VM&, Value*) { return I(host->width); });
    vm.def(displayable, "getHeight:()I", [](VM&, Value*) { return I(host->height); });
    vm.def(displayable, "isShown:()Z", [](VM&, Value* a) { return I(a[0].a == host->current); });
    vm.def(displayable, "addCommand:(Ljavax/microedition/lcdui/Command;)V", [](VM& vm, Value* a) {
        widget(vm, a[0].a).commands.push_back(a[1].a);
        return Value();
    });
    vm.def(displayable, "removeCommand:(Ljavax/microedition/lcdui/Command;)V", [](VM& vm, Value* a) {
        auto& commands = widget(vm, a[0].a).commands;
        commands.erase(std::remove(commands.begin(), commands.end(), a[1].a), commands.end());
        return Value();
    });
    vm.def(displayable, "setCommandListener:(Ljavax/microedition/lcdui/CommandListener;)V", [](VM& vm, Value* a) {
        widget(vm, a[0].a).listener = a[1].a;
        return Value();
    });

    vm.def(canvas, "<init>:()V", [](VM&, Value*) { return Value(); });
    vm.def(canvas, "setFullScreenMode:(Z)V", [](VM&, Value*) { return Value(); });
    vm.def(canvas, "repaint:()V", [](VM&, Value*) { host->repaintPending = true; return Value(); });
    vm.def(canvas, "repaint:(IIII)V", [](VM&, Value*) { host->repaintPending = true; return Value(); });
    vm.def(canvas, "serviceRepaints:()V", [](VM& vm, Value*) { paintIfNeeded(vm, *host); return Value(); });
    vm.def(canvas, "hasPointerEvents:()Z", [](VM&, Value*) { return I(1); });
    vm.def(canvas, "hasPointerMotionEvents:()Z", [](VM&, Value*) { return I(1); });
    vm.def(canvas, "isDoubleBuffered:()Z", [](VM&, Value*) { return I(1); });
    vm.def(canvas, "getGameAction:(I)I", [](VM&, Value* a) {
        switch (a[1].i) {
        case -1: case '2': return I(1); // UP
        case -2: case '8': return I(6); // DOWN
        case -3: case '4': return I(2); // LEFT
        case -4: case '6': return I(5); // RIGHT
        case -5: case '5': return I(8); // FIRE
        default: return I(0);
        }
    });
    vm.def(canvas, "getKeyCode:(I)I", [](VM&, Value* a) {
        switch (a[1].i) {
        case 1: return I(-1);
        case 6: return I(-2);
        case 2: return I(-3);
        case 5: return I(-4);
        case 8: return I(-5);
        default: return I(0);
        }
    });

    const char* display = "javax/microedition/lcdui/Display";
    vm.defStatic(display, "getDisplay:(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;", [](VM& vm, Value*) {
        static Object* shared = nullptr;
        if (!shared) {
            shared = vm.make("javax/microedition/lcdui/Display", nullptr);
            vm.pin(shared);
        }
        return A(shared);
    });
    vm.def(display, "getCurrent:()Ljavax/microedition/lcdui/Displayable;", [](VM&, Value*) { return A(host->current); });
    vm.def(display, "setCurrent:(Ljavax/microedition/lcdui/Displayable;)V", [](VM& vm, Value* a) {
        auto* next = a[1].a;
        if (next == host->current) return Value();
        vm.pin(next);
        vm.unpin(host->current);
        host->current = next;
        const bool isCanvas = next && vm.instanceOf(next->cls, vm.findClass("javax/microedition/lcdui/Canvas"));
        if (verbose) std::fprintf(stderr, "[runtime] now showing %s\n", next ? next->cls->name.c_str() : "nothing");
        host->canvas = isCanvas ? next : nullptr;
        if (isCanvas) {
            vm.call(next, "showNotify:()V");
            host->repaintPending = true;
        }
        return Value();
    });
    vm.def(display, "vibrate:(I)Z", [](VM&, Value*) { return I(0); });
    vm.def(display, "flashBacklight:(I)Z", [](VM&, Value*) { return I(0); });
    vm.def(display, "isColor:()Z", [](VM&, Value*) { return I(1); });
    vm.def(display, "numColors:()I", [](VM&, Value*) { return I(1 << 24); });

    // Forms are used only by the online and name-entry screens. They keep their state but are not drawn.
    const char* command = "javax/microedition/lcdui/Command";
    vm.def(command, "<init>:(Ljava/lang/String;II)V", [](VM& vm, Value* a) {
        widget(vm, a[0].a).text = a[1].a ? vm.str(a[1].a) : u"";
        return Value();
    });
    const char* item = "javax/microedition/lcdui/Item";
    vm.def(item, "setLayout:(I)V", [](VM&, Value*) { return Value(); });
    const char* form = "javax/microedition/lcdui/Form";
    vm.defineNative(form, screenClass);
    vm.def(form, "<init>:(Ljava/lang/String;)V", [](VM& vm, Value* a) { widget(vm, a[0].a); return Value(); });
    vm.def(form, "append:(Ljavax/microedition/lcdui/Item;)I", [](VM& vm, Value* a) {
        auto& items = widget(vm, a[0].a).items;
        items.push_back(a[1].a);
        return I(static_cast<int>(items.size()) - 1);
    });
    vm.def(form, "get:(I)Ljavax/microedition/lcdui/Item;", [](VM& vm, Value* a) {
        auto& items = widget(vm, a[0].a).items;
        if (static_cast<std::uint32_t>(a[1].i) >= items.size()) vm.throwNew("java/lang/IndexOutOfBoundsException");
        return A(items[a[1].i]);
    });
    vm.def(form, "size:()I", [](VM& vm, Value* a) { return I(static_cast<int>(widget(vm, a[0].a).items.size())); });
    vm.def(form, "setItemStateListener:(Ljavax/microedition/lcdui/ItemStateListener;)V", [](VM& vm, Value* a) {
        widget(vm, a[0].a).itemListener = a[1].a;
        return Value();
    });
    const char* textField = "javax/microedition/lcdui/TextField";
    vm.defineNative(textField, item);
    vm.def(textField, "<init>:(Ljava/lang/String;Ljava/lang/String;II)V", [](VM& vm, Value* a) {
        auto& data = widget(vm, a[0].a);
        data.text = a[2].a ? vm.str(a[2].a) : u"";
        data.constraints = a[4].i;
        return Value();
    });
    vm.def(textField, "getString:()Ljava/lang/String;", [](VM& vm, Value* a) { return A(vm.newString(widget(vm, a[0].a).text)); });
    vm.def(textField, "setString:(Ljava/lang/String;)V", [](VM& vm, Value* a) {
        widget(vm, a[0].a).text = a[1].a ? vm.str(a[1].a) : u"";
        return Value();
    });
    vm.def(textField, "getConstraints:()I", [](VM& vm, Value* a) { return I(widget(vm, a[0].a).constraints); });
    vm.def(textField, "setConstraints:(I)V", [](VM& vm, Value* a) { widget(vm, a[0].a).constraints = a[1].i; return Value(); });
    const char* textBox = "javax/microedition/lcdui/TextBox";
    vm.defineNative(textBox, screenClass);
    vm.def(textBox, "<init>:(Ljava/lang/String;Ljava/lang/String;II)V", [](VM& vm, Value* a) {
        widget(vm, a[0].a).text = a[2].a ? vm.str(a[2].a) : u"";
        return Value();
    });
    vm.def(textBox, "getString:()Ljava/lang/String;", [](VM& vm, Value* a) { return A(vm.newString(widget(vm, a[0].a).text)); });
    const char* choice = "javax/microedition/lcdui/ChoiceGroup";
    vm.defineNative(choice, item);
    vm.def(choice, "<init>:(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V", [](VM& vm, Value* a) {
        auto* labels = static_cast<Array*>(a[3].a);
        widget(vm, a[0].a).selected.assign(labels ? labels->length : 0, 0);
        return Value();
    });
    vm.def(choice, "isSelected:(I)Z", [](VM& vm, Value* a) {
        auto& selected = widget(vm, a[0].a).selected;
        return I(static_cast<std::uint32_t>(a[1].i) < selected.size() && selected[a[1].i]);
    });
    vm.def(choice, "setSelectedIndex:(IZ)V", [](VM& vm, Value* a) {
        auto& selected = widget(vm, a[0].a).selected;
        if (static_cast<std::uint32_t>(a[1].i) < selected.size()) selected[a[1].i] = a[2].i != 0;
        return Value();
    });

    const char* midlet = "javax/microedition/midlet/MIDlet";
    vm.def(midlet, "<init>:()V", [](VM&, Value*) { return Value(); });
    vm.def(midlet, "getAppProperty:(Ljava/lang/String;)Ljava/lang/String;", [](VM& vm, Value* a) {
        const auto key = narrow(vm.str(a[1].a));
        const auto* value = vm.jar.property(key);
        if (verbose) std::fprintf(stderr, "[runtime] app property %s -> %s\n", key.c_str(), value ? value->c_str() : "null");
        return A(value ? vm.newString(*value) : nullptr);
    });
    vm.def(midlet, "notifyDestroyed:()V", [](VM&, Value*) { host->quit = true; return Value(); });
    vm.def(midlet, "notifyPaused:()V", [](VM&, Value*) { return Value(); });
    vm.def(midlet, "resumeRequest:()V", [](VM&, Value*) { return Value(); });
    vm.def(midlet, "platformRequest:(Ljava/lang/String;)Z", [](VM& vm, Value* a) {
        if (verbose) std::fprintf(stderr, "[runtime] ignored request to open %s\n", narrow(vm.str(a[1].a)).c_str());
        return I(0);
    });
    vm.def(midlet, "checkPermission:(Ljava/lang/String;)I", [](VM&, Value*) { return I(0); });
}

// ---------------------------------------------------------------- saved records

std::string storePath(const std::string& name) {
    std::string file;
    for (const unsigned char ch : name) {
        file += std::isalnum(ch) || ch == '-' || ch == '_' ? static_cast<char>(ch) : '_';
    }
    return host->saveDirectory + "/" + file + ".rms";
}

bool loadStore(const std::string& name) {
    if (stores.count(name)) return true;
    std::ifstream file(storePath(name), std::ios::binary);
    if (!file) return false;
    const Bytes data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::size_t at = 8;
    const auto word = [&]() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4 && at < data.size(); ++i) value |= static_cast<std::uint32_t>(data[at++]) << (8 * i);
        return value;
    };
    if (data.size() < 16 || std::memcmp(data.data(), "AOW2RMS1", 8) != 0) return false;
    Store store;
    store.nextId = static_cast<int>(word());
    for (auto count = word(); count > 0 && at < data.size(); --count) {
        const int id = static_cast<int>(word());
        const auto size = word();
        if (at + size > data.size()) return false; // A damaged file is treated as missing.
        store.records.emplace_back(id, Bytes(data.begin() + at, data.begin() + at + size));
        at += size;
    }
    stores[name] = std::move(store);
    return true;
}

void saveStore(const std::string& name) {
    const auto& store = stores[name];
    Bytes data(8);
    std::memcpy(data.data(), "AOW2RMS1", 8);
    const auto word = [&](std::uint32_t value) {
        for (int i = 0; i < 4; ++i) data.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    word(static_cast<std::uint32_t>(store.nextId));
    word(static_cast<std::uint32_t>(store.records.size()));
    for (const auto& [id, record] : store.records) {
        word(static_cast<std::uint32_t>(id));
        word(static_cast<std::uint32_t>(record.size()));
        data.insert(data.end(), record.begin(), record.end());
    }
    // Write beside the old file, then swap, so a crash never leaves a half-written save.
    std::error_code ignored;
    std::filesystem::create_directories(host->saveDirectory, ignored);
    const auto path = storePath(name);
    {
        std::ofstream file(path + ".tmp", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) {
            std::fprintf(stderr, "Could not write %s\n", path.c_str());
            return;
        }
    }
    std::filesystem::rename(path + ".tmp", path, ignored);
}

Bytes* findRecord(VM& vm, Object* object, int id) {
    auto& store = stores[vm.payload<RecordStoreData>(object).name];
    for (auto& [recordId, record] : store.records) {
        if (recordId == id) return &record;
    }
    vm.throwNew("javax/microedition/rms/InvalidRecordIDException");
}

void defineRecords(VM& vm) {
    const char* base = "javax/microedition/rms/RecordStoreException";
    vm.defineNative(base, "java/lang/Exception");
    for (const char* name : {"javax/microedition/rms/RecordStoreNotFoundException",
                             "javax/microedition/rms/RecordStoreFullException",
                             "javax/microedition/rms/RecordStoreNotOpenException",
                             "javax/microedition/rms/InvalidRecordIDException"}) {
        vm.defineNative(name, base);
    }
    const char* store = "javax/microedition/rms/RecordStore";
    vm.defStatic(store, "openRecordStore:(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;", [](VM& vm, Value* a) {
        const auto name = narrow(vm.str(a[0].a));
        if (!loadStore(name)) {
            if (verbose) std::fprintf(stderr, "[runtime] record store %s %s\n", name.c_str(), a[1].i ? "created" : "not found");
            if (!a[1].i) vm.throwNew("javax/microedition/rms/RecordStoreNotFoundException", name);
            stores[name] = Store();
            saveStore(name);
        }
        auto data = std::make_unique<RecordStoreData>();
        data->name = name;
        return A(vm.make("javax/microedition/rms/RecordStore", std::move(data)));
    });
    vm.defStatic(store, "deleteRecordStore:(Ljava/lang/String;)V", [](VM& vm, Value* a) {
        const auto name = narrow(vm.str(a[0].a));
        if (!loadStore(name)) vm.throwNew("javax/microedition/rms/RecordStoreNotFoundException", name);
        stores.erase(name);
        std::error_code ignored;
        std::filesystem::remove(storePath(name), ignored);
        return Value();
    });
    vm.defStatic(store, "listRecordStores:()[Ljava/lang/String;", [](VM&, Value*) { return A(nullptr); });
    vm.def(store, "closeRecordStore:()V", [](VM&, Value*) { return Value(); });
    vm.def(store, "getNumRecords:()I", [](VM& vm, Value* a) {
        return I(static_cast<int>(stores[vm.payload<RecordStoreData>(a[0].a).name].records.size()));
    });
    vm.def(store, "getNextRecordID:()I", [](VM& vm, Value* a) {
        return I(stores[vm.payload<RecordStoreData>(a[0].a).name].nextId);
    });
    vm.def(store, "addRecord:([BII)I", [](VM& vm, Value* a) {
        const auto name = vm.payload<RecordStoreData>(a[0].a).name;
        auto& data = stores[name];
        auto* bytes = static_cast<Array*>(a[1].a);
        if (bytes && (a[2].i < 0 || a[3].i < 0 || a[2].i > bytes->length - a[3].i)) {
            vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
        }
        data.records.emplace_back(data.nextId, bytes ? bytesOf(bytes, a[2].i, a[3].i) : Bytes());
        saveStore(name);
        return I(data.nextId++);
    });
    vm.def(store, "setRecord:(I[BII)V", [](VM& vm, Value* a) {
        auto* record = findRecord(vm, a[0].a, a[1].i);
        auto* bytes = static_cast<Array*>(a[2].a);
        if (bytes && (a[3].i < 0 || a[4].i < 0 || a[3].i > bytes->length - a[4].i)) {
            vm.throwNew("java/lang/ArrayIndexOutOfBoundsException");
        }
        *record = bytes ? bytesOf(bytes, a[3].i, a[4].i) : Bytes();
        saveStore(vm.payload<RecordStoreData>(a[0].a).name);
        return Value();
    });
    vm.def(store, "getRecord:(I)[B", [](VM& vm, Value* a) {
        auto* record = findRecord(vm, a[0].a, a[1].i);
        return A(record->empty() ? nullptr : newByteArray(vm, *record));
    });
    vm.def(store, "getRecordSize:(I)I", [](VM& vm, Value* a) {
        return I(static_cast<int>(findRecord(vm, a[0].a, a[1].i)->size()));
    });
    vm.def(store, "deleteRecord:(I)V", [](VM& vm, Value* a) {
        findRecord(vm, a[0].a, a[1].i);
        const auto name = vm.payload<RecordStoreData>(a[0].a).name;
        auto& records = stores[name].records;
        records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& entry) { return entry.first == a[1].i; }),
                      records.end());
        saveStore(name);
        return Value();
    });
}

// ---------------------------------------------------------------- sound and network

void silence(Object* player) {
    if (player == musicOwner) {
        musicOwner = nullptr;
        if (host->synth) host->synth->stop();
    }
}

void defineMedia(VM& vm) {
    vm.defineNative("javax/microedition/media/MediaException", "java/lang/Exception");
    vm.defineNative("javax/microedition/media/Controllable");
    vm.defineNative("javax/microedition/media/Player", "java/lang/Object", {"javax/microedition/media/Controllable"});
    vm.defineNative("javax/microedition/media/PlayerListener");
    vm.defineNative("javax/microedition/media/Control");
    vm.defineNative("javax/microedition/media/control/VolumeControl", "java/lang/Object", {"javax/microedition/media/Control"});

    const char* manager = "javax/microedition/media/Manager";
    vm.defStatic(manager, "getSupportedContentTypes:(Ljava/lang/String;)[Ljava/lang/String;", [](VM& vm, Value*) {
        auto* types = vm.newArray(vm.findClass("[Ljava/lang/String;"), 2);
        vm.pin(types);
        types->a()[0] = vm.newString(std::string("audio/midi"));
        types->a()[1] = vm.newString(std::string("audio/x-wav"));
        vm.unpin(types);
        return A(types);
    });
    vm.defStatic(manager, "createPlayer:(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;", [](VM& vm, Value* a) {
        if (!a[0].a) vm.throwNew("java/lang/IllegalArgumentException");
        auto data = std::make_unique<PlayerData>();
        const auto bytes = drain(vm, a[0].a);
        data->type = a[1].a ? narrow(vm.str(a[1].a)) : "";
        data->song = midi::parse(bytes);
        if (verbose) {
            std::fprintf(stderr, "[runtime] sound loaded: %s, %zu bytes%s\n", data->type.c_str(), bytes.size(),
                         data->song ? "" : " (not playable)");
        }
        // The game then tries its next sound format.
        if (!data->song) vm.throwNew("javax/microedition/media/MediaException", "unsupported sound format");
        return A(vm.make("javax/microedition/media/Player$Impl", std::move(data)));
    });

    // MIDI music goes to the built-in synthesizer. The player also keeps the state the game reads back.
    const char* player = "javax/microedition/media/Player$Impl";
    vm.defineNative(player, "java/lang/Object", {"javax/microedition/media/Player"});
    vm.def(player, "realize:()V", [](VM& vm, Value* a) {
        auto& data = vm.payload<PlayerData>(a[0].a);
        if (data.state < 200) data.state = 200;
        return Value();
    });
    vm.def(player, "prefetch:()V", [](VM& vm, Value* a) {
        auto& data = vm.payload<PlayerData>(a[0].a);
        if (data.state < 300) data.state = 300;
        return Value();
    });
    vm.def(player, "start:()V", [](VM& vm, Value* a) {
        auto& data = vm.payload<PlayerData>(a[0].a);
        data.state = 400;
        musicOwner = a[0].a;
        if (host->synth) {
            host->synth->setVolume(data.level);
            host->synth->play(data.song, data.loops == 0 ? 1 : data.loops);
        }
        return Value();
    });
    vm.def(player, "stop:()V", [](VM& vm, Value* a) {
        auto& data = vm.payload<PlayerData>(a[0].a);
        if (data.state == 400) data.state = 300;
        silence(a[0].a);
        return Value();
    });
    vm.def(player, "deallocate:()V", [](VM& vm, Value* a) {
        auto& data = vm.payload<PlayerData>(a[0].a);
        if (data.state >= 300) data.state = 200;
        silence(a[0].a);
        return Value();
    });
    vm.def(player, "close:()V", [](VM& vm, Value* a) {
        vm.payload<PlayerData>(a[0].a).state = 0;
        silence(a[0].a);
        return Value();
    });
    vm.def(player, "getState:()I", [](VM& vm, Value* a) { return I(vm.payload<PlayerData>(a[0].a).state); });
    vm.def(player, "setLoopCount:(I)V", [](VM& vm, Value* a) { vm.payload<PlayerData>(a[0].a).loops = a[1].i; return Value(); });
    vm.def(player, "setMediaTime:(J)J", [](VM&, Value* a) { return J(a[1].j); });
    vm.def(player, "getMediaTime:()J", [](VM&, Value*) { return J(0); });
    vm.def(player, "getDuration:()J", [](VM&, Value*) { return J(-1); });
    vm.def(player, "addPlayerListener:(Ljavax/microedition/media/PlayerListener;)V", [](VM& vm, Value* a) {
        vm.payload<PlayerData>(a[0].a).listeners.push_back(a[1].a);
        return Value();
    });
    vm.def(player, "removePlayerListener:(Ljavax/microedition/media/PlayerListener;)V", [](VM& vm, Value* a) {
        auto& listeners = vm.payload<PlayerData>(a[0].a).listeners;
        listeners.erase(std::remove(listeners.begin(), listeners.end(), a[1].a), listeners.end());
        return Value();
    });
    vm.def(player, "getControl:(Ljava/lang/String;)Ljavax/microedition/media/Control;", [](VM& vm, Value* a) {
        auto data = std::make_unique<ControlData>();
        data->player = a[0].a;
        return A(vm.make("javax/microedition/media/control/VolumeControl$Impl", std::move(data)));
    });
    const char* volume = "javax/microedition/media/control/VolumeControl$Impl";
    vm.defineNative(volume, "java/lang/Object", {"javax/microedition/media/control/VolumeControl"});
    vm.def(volume, "setLevel:(I)I", [](VM& vm, Value* a) {
        auto& data = vm.payload<PlayerData>(vm.payload<ControlData>(a[0].a).player);
        data.level = std::clamp(a[1].i, 0, 100);
        if (host->synth && musicOwner == vm.payload<ControlData>(a[0].a).player) host->synth->setVolume(data.level);
        return I(data.level);
    });
    vm.def(volume, "getLevel:()I", [](VM& vm, Value* a) {
        return I(vm.payload<PlayerData>(vm.payload<ControlData>(a[0].a).player).level);
    });

    // There is no network. Every connection attempt fails the way a phone without coverage would.
    vm.defineNative("javax/microedition/io/ConnectionNotFoundException", "java/io/IOException");
    for (const char* sig : {"open:(Ljava/lang/String;)Ljavax/microedition/io/Connection;",
                            "open:(Ljava/lang/String;I)Ljavax/microedition/io/Connection;",
                            "open:(Ljava/lang/String;IZ)Ljavax/microedition/io/Connection;"}) {
        vm.defStatic("javax/microedition/io/Connector", sig, [](VM& vm, Value* a) -> Value {
            if (verbose) std::fprintf(stderr, "[runtime] refused connection to %s\n", narrow(vm.str(a[0].a)).c_str());
            vm.throwNew("javax/microedition/io/ConnectionNotFoundException");
        });
    }
}
} // namespace

void paintIfNeeded(VM& vm, Host& target) {
    if (!target.canvas || !target.repaintPending.exchange(false)) {
        return;
    }
    vm.call(target.canvas, "paint:(Ljavax/microedition/lcdui/Graphics;)V", {A(newGraphics(vm, &screen, nullptr))});
    std::lock_guard<std::mutex> guard(target.frameMutex);
    target.frame = screen;
    ++target.frameCount;
}

void defineMidp(VM& vm, Host& target) {
    host = &target;
    screen.resize(target.width, target.height, 0xFF000000);
    target.frame = screen;
    defineGraphics(vm);
    defineDisplay(vm);
    defineRecords(vm);
    defineMedia(vm);
}

} // namespace vm
