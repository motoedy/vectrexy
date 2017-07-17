#pragma once

#include <cstdint>
#include <map>
#include <optional>

struct Breakpoint {
    uint16_t address = 0;
    bool enabled = true;
    bool autoDelete = false;
};

class Breakpoints {
public:
    Breakpoint* Add(uint16_t address) {
        auto& bp = m_breakpoints[address];
        bp.address = address;
        return &bp;
    }

    std::optional<Breakpoint> Remove(uint16_t address) {
        auto iter = m_breakpoints.find(address);
        if (iter != m_breakpoints.end()) {
            auto bp = iter->second;
            m_breakpoints.erase(iter);
            return bp;
        }
        return {};
    }

    std::optional<Breakpoint> RemoveAtIndex(size_t index) {
        auto iter = GetBreakpointIterAtIndex(index);
        if (iter != m_breakpoints.end()) {
            auto bp = iter->second;
            m_breakpoints.erase(iter);
            return bp;
        }
        return {};
    }

    Breakpoint* Get(uint16_t address) {
        auto iter = m_breakpoints.find(address);
        if (iter != m_breakpoints.end()) {
            return &iter->second;
        }
        return nullptr;
    }

    Breakpoint* GetAtIndex(size_t index) {
        auto iter = GetBreakpointIterAtIndex(index);
        if (iter != m_breakpoints.end()) {
            return &iter->second;
        }
        return nullptr;
    }

    std::optional<size_t> GetIndex(uint16_t address) {
        auto iter = m_breakpoints.find(address);
        if (iter != m_breakpoints.end()) {
            return std::distance(m_breakpoints.begin(), iter);
        }
        return {};
    }

    size_t Num() const { return m_breakpoints.size(); }

private:
    std::map<uint16_t, Breakpoint> m_breakpoints;

    using IterType = decltype(m_breakpoints.begin());
    IterType GetBreakpointIterAtIndex(size_t index) {
        auto iter = m_breakpoints.begin();
        if (iter != m_breakpoints.end())
            std::advance(iter, index);
        return iter;
    }
};