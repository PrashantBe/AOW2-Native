#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using Bytes = std::vector<std::uint8_t>;

// Reads every entry of the game JAR into memory.
class Jar {
public:
    explicit Jar(const std::string& path);
    const Bytes* find(const std::string& name) const;
    // Value of a "Key: value" line in META-INF/MANIFEST.MF, or nullptr.
    const std::string* property(const std::string& key) const;

private:
    std::map<std::string, Bytes> files_;
    std::map<std::string, std::string> manifest_;
};
