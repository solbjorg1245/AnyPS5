#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUESTATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUESTATE_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <map>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AgcDriver {

class Registers {
public:
    using key_type = std::uint32_t;
    using mapped_type = std::uint32_t;
    using value_type = std::pair<const std::uint32_t, std::uint32_t>;

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Registers::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = value_type;
        struct Arrow {
            value_type entry;
            const value_type* operator->() const { return &entry; }
        };
        const_iterator() = default;
        const_iterator(const Registers* owner, std::size_t index) : owner(owner), index(index) {}
        value_type operator*() const { return {static_cast<std::uint32_t>(index), owner->values[index]}; }
        Arrow operator->() const { return {**this}; }
        const_iterator& operator++() {
            index = owner->nextPresent(index + 1);
            return *this;
        }
        const_iterator operator++(int) {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const const_iterator& other) const { return index == other.index; }
        bool operator!=(const const_iterator& other) const { return index != other.index; }

    private:
        const Registers* owner = nullptr;
        std::size_t index = 0;
    };
    using iterator = const_iterator;

    Registers() = default;
    Registers(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> entries) {
        for (const auto& [offset, value] : entries) emplace(offset, value);
    }

    const_iterator begin() const { return {this, nextPresent(0)}; }
    const_iterator end() const { return {this, End}; }
    const_iterator find(std::uint32_t offset) const { return contains(offset) ? const_iterator{this, offset} : end(); }
    const_iterator lower_bound(std::uint32_t offset) const { return {this, nextPresent(offset)}; }
    const_iterator upper_bound(std::uint32_t offset) const { return {this, nextPresent(static_cast<std::size_t>(offset) + 1)}; }
    bool contains(std::uint32_t offset) const { return offset < values.size() && ((present[offset / 64] >> (offset % 64)) & 1u) != 0; }
    std::size_t count(std::uint32_t offset) const { return contains(offset) ? 1 : 0; }
    std::size_t size() const { return entries; }
    bool empty() const { return entries == 0; }
    void clear() {
        values.clear();
        present.clear();
        entries = 0;
    }
    std::pair<const_iterator, bool> emplace(std::uint32_t offset, std::uint32_t value) {
        if (contains(offset)) return {const_iterator{this, offset}, false};
        mark(offset) = value;
        return {const_iterator{this, offset}, true};
    }
    std::pair<const_iterator, bool> insert_or_assign(std::uint32_t offset, std::uint32_t value) {
        if (contains(offset)) {
            values[offset] = value;
            return {const_iterator{this, offset}, false};
        }
        mark(offset) = value;
        return {const_iterator{this, offset}, true};
    }
    // Stores like insert_or_assign and reports whether the register file changed: a register that
    // was absent, or a different value (Pm4's writeRegister bumps QueueState::stateSerial on it).
    bool assign(std::uint32_t offset, std::uint32_t value) {
        if (contains(offset)) {
            auto& stored = values[offset];
            if (stored == value) return false;
            stored = value;
            return true;
        }
        mark(offset) = value;
        return true;
    }
    // The value of a present register, or null: one bounds and presence test (the state hash's read).
    const std::uint32_t* Find(std::uint32_t offset) const { return contains(offset) ? &values[offset] : nullptr; }
    std::uint32_t& operator[](std::uint32_t offset) {
        if (contains(offset)) return values[offset];
        return mark(offset) = 0;
    }
    std::uint32_t& at(std::uint32_t offset) {
        if (!contains(offset)) throw std::out_of_range("register is not set");
        return values[offset];
    }
    const std::uint32_t& at(std::uint32_t offset) const {
        if (!contains(offset)) throw std::out_of_range("register is not set");
        return values[offset];
    }
    std::size_t erase(std::uint32_t offset) {
        if (!contains(offset)) return 0;
        present[offset / 64] &= ~(std::uint64_t{1} << (offset % 64));
        --entries;
        return 1;
    }
    bool operator==(const Registers& other) const {
        auto a = begin();
        auto b = other.begin();
        for (; a != end() && b != other.end(); ++a, ++b) {
            if ((*a).first != (*b).first || (*a).second != (*b).second) return false;
        }
        return a == end() && b == other.end();
    }

private:
    static constexpr std::size_t End = ~std::size_t{0};
    std::uint32_t& mark(std::uint32_t offset) {
        if (offset >= values.size()) {
            const auto words = static_cast<std::size_t>(offset) / 64 + 1;
            values.resize(words * 64, 0);
            present.resize(words, 0);
        }
        present[offset / 64] |= std::uint64_t{1} << (offset % 64);
        ++entries;
        return values[offset];
    }
    std::size_t nextPresent(std::size_t index) const {
        for (std::size_t word = index / 64; word < present.size(); ++word) {
            auto bits = present[word];
            if (word == index / 64) bits &= ~std::uint64_t{0} << (index % 64);
            if (bits != 0) return word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
        }
        return End;
    }
    std::vector<std::uint32_t> values;
    std::vector<std::uint64_t> present;
    std::size_t entries = 0;
};

inline Registers InitialContextRegisters() {
    Registers result{
        {0x200, 0}, {0x201, 0}, {0x202, 0xcc0010}, {0x203, 0},
        {0x204, 0}, {0x205, 0}, {0x206, 1087}, {0x207, 0},
        {0x0, 0}, {0x2, 0}, {0x3, 0}, {0x4, 0}, {0x8, 0}, {0x9, 0x3f800000}, {0xa, 0}, {0xb, 0},
        {0x80, 0}, {0x83, 0xffff}, {0x8c, 0xaa99aaaa}, {0x8d, 0}, {0x8e, 0}, {0x8f, 0},
        {0xc, 0}, {0xd, 0x40004000}, {0x81, 0x80000000}, {0x82, 0x40004000},
        {0x90, 0x80000000}, {0x91, 0x40004000},
        {0x105, 0}, {0x106, 0}, {0x107, 0}, {0x108, 0},
        {0x1b1, 0}, {0x1b6, 0}, {0x1c3, 0}, {0x1c4, 0}, {0x1c5, 0},
        {0x1ff, 0}, {0x292, 2}, {0x293, 0}, {0x29b, 0},
        {0x2ce, 0}, {0x2d3, 0}, {0x2d5, 0}, {0x2d6, 0}, {0x2db, 0},
        {0x2dc, 0xaa00}, {0x2e4, 0}, {0x2f8, 0}, {0x2f9, 0x2d},
        {0x30e, 0xffffffff}, {0x30f, 0xffffffff}, {0x313, 0x6000},
        {0x318, 0}, {0x31b, 0}, {0x31c, 0}, {0x31d, 0},
        {0x390, 0}, {0x3b0, 0}, {0x3b8, 0}
    };
    for (std::uint32_t i = 0; i < 8; ++i) result.emplace(0x1e0 + i, 0x20010001);
    for (std::uint32_t i = 0; i < 16; ++i) {
        result.emplace(0x94 + 2 * i, 0x80000000);
        result.emplace(0x95 + 2 * i, 0x40004000);
        result.emplace(0xb4 + 2 * i, 0);
        result.emplace(0xb5 + 2 * i, 0);
        for (std::uint32_t j = 0; j < 6; ++j) result.emplace(0x10f + 6 * i + j, j % 2 == 0 ? 0x3f800000 : 0);
    }
    return result;
}

// The draw state registers (APS5_FAST_STATE, Execution/src/Driver/Draw/FastState.cpp), as
// [first, first + count) per bank (0 context, 1 shader, 2 user config): Graphics::DrawKeyRegisters
// without the shader user words and the merged-stage user-data pointers (StateUserRegisters, which
// change from draw to draw), plus the registers the draw precheck reads beyond that table
// (StateExtraRegisters: VGT_MULTI_PRIM_IB_RESET_INDX). FastState.cpp checks at compile time that
// the tables split DrawKeyRegisters exactly.
struct StateRegisterRange {
    std::uint8_t bank;
    std::uint32_t first;
    std::uint32_t count;
};
inline constexpr std::array<StateRegisterRange, 46> StateKeyRegisters{{
    {0, 0x000, 1}, {0, 0x002, 1}, {0, 0x005, 1}, {0, 0x007, 1}, {0, 0x00a, 4}, {0, 0x010, 6}, {0, 0x01a, 5},
    {0, 0x080, 4}, {0, 0x08c, 4}, {0, 0x090, 2}, {0, 0x094, 2}, {0, 0x0b4, 2}, {0, 0x103, 1}, {0, 0x105, 4}, {0, 0x10b, 3}, {0, 0x10f, 6},
    {0, 0x191, 32}, {0, 0x1b3, 2}, {0, 0x1b6, 1}, {0, 0x1c3, 3}, {0, 0x1e0, 8}, {0, 0x1ff, 1},
    {0, 0x200, 8}, {0, 0x292, 2}, {0, 0x29b, 1}, {0, 0x2ab, 1}, {0, 0x2ce, 1}, {0, 0x2d5, 2}, {0, 0x2db, 2}, {0, 0x2de, 6}, {0, 0x2f8, 2}, {0, 0x30e, 2}, {0, 0x313, 1},
    {0, 0x318, 0x78}, {0, 0x390, 8}, {0, 0x3a8, 0x18},
    // The program addresses and RSRC words of the pixel, geometry-back, vertex/geometry-front, hull
    // and local programs.
    {1, 0x008, 4}, {1, 0x088, 2}, {1, 0x08a, 2}, {1, 0x0c8, 2}, {1, 0x108, 2}, {1, 0x10b, 1}, {1, 0x148, 2},
    {2, 0x242, 1}, {2, 0x24b, 1}, {2, 0x25b, 1},
}};
// The pixel user words, the geometry-back user pointer, the vertex/geometry-front user words, the
// hull user pointer and the hull/local user words: the draw key mixes them per draw.
inline constexpr std::array<StateRegisterRange, 5> StateUserRegisters{{{1, 0x00c, 32}, {1, 0x082, 2}, {1, 0x08c, 32}, {1, 0x102, 2}, {1, 0x10c, 32}}};
inline constexpr std::array<StateRegisterRange, 1> StateExtraRegisters{{{0, 0x103, 1}}};

// One bit per register offset below 1024, per bank.
using StateRegisterMask = std::array<std::array<std::uint64_t, 16>, 3>;
template<std::size_t N>
constexpr StateRegisterMask MakeStateRegisterMask(const std::array<StateRegisterRange, N>& ranges) {
    StateRegisterMask mask{};
    for (const auto& range : ranges) {
        for (std::uint32_t offset = range.first; offset < range.first + range.count; ++offset) mask[range.bank][offset / 64] |= std::uint64_t{1} << (offset % 64);
    }
    return mask;
}
inline constexpr StateRegisterMask StateKeyMask = MakeStateRegisterMask(StateKeyRegisters);
inline bool StateKeyRegister(std::uint32_t bank, std::uint32_t offset) {
    return bank < 3 && offset < 1024 && ((StateKeyMask[bank][offset / 64] >> (offset % 64)) & 1u) != 0;
}

inline std::uint64_t StateMix(std::uint64_t hash, std::uint64_t value) {
    hash = (hash ^ value) * 0x9fb21c651e98df25ull;
    return hash ^ (hash >> 28u);
}

namespace DriverDetail {
struct FastStateEntry;
}

// FastState.cpp's memo of a queue's draw state at one stateSerial: the shader registry its key
// looked the programs up in, and the state's entry.
struct QueueStateMemo {
    std::uint64_t serial = ~std::uint64_t{0};
    std::shared_ptr<const void> registry;
    std::shared_ptr<DriverDetail::FastStateEntry> entry;
};

struct QueueState {
    Registers shader;
    Registers context = InitialContextRegisters();
    Registers userConfig{{0x24a, 0}, {0x24b, 0}};
    std::optional<Registers> savedContext;
    std::array<std::uint32_t, 0x3000> constantRam{};
    std::uint64_t indexBase = 0;
    std::uint64_t drawIndirectBase = 0;
    std::uint64_t dispatchIndirectBase = 0;
    std::uint32_t indexBufferSize = 0;
    std::uint32_t indexType = 0;
    std::uint32_t instanceCount = 1;
    std::vector<std::string> markers;
    // Bumped by every store that changes a StateKeyRegisters register (Pm4's writeRegister), by
    // CLEAR_STATE and by every context push or pop: an unchanged serial means unchanged state
    // registers. The memo travels with the registers it describes, so a copied or reset queue
    // state never takes another one's.
    std::uint64_t stateSerial = 0;
    mutable QueueStateMemo fastState;

    void ClearContext() {
        context = InitialContextRegisters();
        ++stateSerial;
    }
};

// The hash of the present StateKeyRegisters registers with their banks and offsets (an absent
// register is left out, so presence counts): equal register states give equal hashes.
inline std::uint64_t StateRegisterHash(const QueueState& queue) {
    const std::array<const Registers*, 3> banks{&queue.context, &queue.shader, &queue.userConfig};
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    for (const auto& range : StateKeyRegisters) {
        const auto& bank = *banks[range.bank];
        for (std::uint32_t offset = range.first; offset < range.first + range.count; ++offset) {
            if (const auto* value = bank.Find(offset)) hash = StateMix(hash, (static_cast<std::uint64_t>(range.bank) << 48u) | (static_cast<std::uint64_t>(offset) << 32u) | *value);
        }
    }
    return hash;
}

}

#endif
