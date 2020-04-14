#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

struct SourceLocation {
    bool operator==(const SourceLocation& rhs) const {
        return file == rhs.file && line == rhs.line;
    }
    bool operator!=(const SourceLocation& rhs) const { return !(*this == rhs); }

    // TODO: expensive to store the same file string for multiple entries. Make a table of file
    // names, and store a index or pointer to it.
    std::string file;
    uint32_t line;
};

namespace std {
    template <>
    struct hash<SourceLocation> {
        std::size_t operator()(const SourceLocation& location) const {
            std::string s = location.file + std::to_string(location.line);
            return std::hash<std::string>{}(s);
        }
    };
} // namespace std

struct Symbol {
    std::string name;
    uint16_t address{};
};

class DebugSymbols {
public:
    void AddSourceLocation(uint16_t address, SourceLocation location);
    SourceLocation* GetSourceLocation(uint16_t address);
    std::optional<uint16_t> GetAddressBySourceLocation(const SourceLocation& location) const;

    void AddSymbol(Symbol symbol);
    const Symbol* GetSymbolByName(const std::string& name) const;
    const Symbol* GetSymbolByAddress(uint16_t address) const;

private:
    // Store source location info for every possible address
    // Address -> Source Location
    std::array<SourceLocation, 64 * 1024> m_sourceLocations;

    // Address -> Symbol
    // Note that multiple addresses may map to the same symbol name,
    // i.e. constants from headers included from multiple cpp files
    std::unordered_map<uint16_t, Symbol> m_symbolsByAddress;

    // Source Location -> first address of instruction for this location
    std::unordered_map<SourceLocation, uint16_t> m_locationToAddress;
};