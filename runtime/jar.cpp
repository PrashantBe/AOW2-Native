#include "jar.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <zlib.h>

namespace {
std::uint32_t le(const Bytes& data, std::size_t at, int size) {
    if (at + size > data.size()) {
        throw std::runtime_error("JAR is truncated");
    }
    std::uint32_t value = 0;
    for (int i = size - 1; i >= 0; --i) {
        value = value << 8 | data[at + i];
    }
    return value;
}
} // namespace

Jar::Jar(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open " + path);
    }
    const Bytes zip((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (zip.size() < 22) {
        throw std::runtime_error(path + " is not a JAR");
    }
    // The central directory is located from the end record, which sits before an optional comment.
    std::size_t end = zip.size() - 22;
    while (le(zip, end, 4) != 0x06054b50) {
        if (end == 0 || zip.size() - end > 66000) {
            throw std::runtime_error(path + " is not a JAR");
        }
        --end;
    }
    const auto count = le(zip, end + 10, 2);
    std::size_t at = le(zip, end + 16, 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (le(zip, at, 4) != 0x02014b50) {
            throw std::runtime_error("JAR directory is damaged");
        }
        const auto method = le(zip, at + 10, 2);
        const auto packed = le(zip, at + 20, 4);
        const auto size = le(zip, at + 24, 4);
        const auto nameLength = le(zip, at + 28, 2);
        const auto local = le(zip, at + 42, 4);
        const std::string name(zip.begin() + at + 46, zip.begin() + at + 46 + nameLength);
        at += 46 + nameLength + le(zip, at + 30, 2) + le(zip, at + 32, 2);

        const std::size_t start = local + 30 + le(zip, local + 26, 2) + le(zip, local + 28, 2);
        if (start + packed > zip.size()) {
            throw std::runtime_error("JAR entry " + name + " is truncated");
        }
        Bytes data(size);
        if (method == 0) {
            data.assign(zip.begin() + start, zip.begin() + start + packed);
        } else if (method == 8) {
            z_stream stream{};
            stream.next_in = const_cast<Bytef*>(zip.data() + start);
            stream.avail_in = packed;
            stream.next_out = data.data();
            stream.avail_out = size;
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                throw std::runtime_error("zlib failed to start");
            }
            const int result = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (result != Z_STREAM_END && !(size == 0 && result == Z_BUF_ERROR)) {
                throw std::runtime_error("JAR entry " + name + " is damaged");
            }
        } else {
            throw std::runtime_error("JAR entry " + name + " uses an unsupported compression");
        }
        files_[name] = std::move(data);
    }

    if (const auto* manifest = find("META-INF/MANIFEST.MF")) {
        std::string line;
        const auto flush = [&]() {
            const auto colon = line.find(": ");
            if (colon != std::string::npos) {
                manifest_[line.substr(0, colon)] = line.substr(colon + 2);
            }
            line.clear();
        };
        for (std::size_t i = 0; i < manifest->size(); ++i) {
            const char ch = static_cast<char>((*manifest)[i]);
            if (ch == '\r') {
                continue;
            }
            if (ch != '\n') {
                line += ch;
                continue;
            }
            // A line starting with a space continues the previous one.
            if (i + 1 < manifest->size() && (*manifest)[i + 1] == ' ') {
                ++i;
                continue;
            }
            flush();
        }
        flush();
    }
}

const Bytes* Jar::find(const std::string& name) const {
    const auto found = files_.find(name);
    return found == files_.end() ? nullptr : &found->second;
}

const std::string* Jar::property(const std::string& key) const {
    const auto found = manifest_.find(key);
    return found == manifest_.end() ? nullptr : &found->second;
}
