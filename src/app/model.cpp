#include "model.h"

#include <array>
#include <numeric>

namespace derandomizer {
namespace {

const std::array<const wchar_t*, 10> kNightlords = {
    L"Gladius",
    L"Adel",
    L"Gnoster",
    L"Maris",
    L"Libra",
    L"Fulghor",
    L"Caligo",
    L"Heolstor",
    L"Harmonia",
    L"Straghess",
};

const std::array<const wchar_t*, 6> kMapKinds = {
    L"Standard", L"Mountaintop", L"Crater", L"Rotten Woods", L"Great Hollow", L"Hidden City",
};

void addBlock(std::vector<Block>& out, const wchar_t* label,
              std::uint32_t weight, int first, int count) {
    out.push_back(Block{label, weight, first, count});
}

}  // namespace

SFMT19937::SFMT19937(std::uint32_t seed) {
    state_[0] = seed;
    for (int i = 1; i < 624; ++i) {
        const std::uint32_t prev = state_[i - 1];
        state_[i] = 1812433253u * (prev ^ (prev >> 30)) + static_cast<std::uint32_t>(i);
    }

    constexpr std::array<std::uint32_t, 4> parity = {1u, 0u, 0u, 331998852u};
    std::uint32_t parityValue = 0;
    for (int i = 0; i < 4; ++i) parityValue ^= state_[i] & parity[i];
    for (int shift : {16, 8, 4, 2, 1}) parityValue ^= parityValue >> shift;
    if ((parityValue & 1u) == 0) {
        for (int i = 0; i < 4; ++i) {
            std::uint32_t bit = 1;
            for (int j = 0; j < 32; ++j, bit <<= 1) {
                if ((bit & parity[i]) != 0) {
                    state_[i] ^= bit;
                    return;
                }
            }
        }
    }
}

void SFMT19937::generateAll() {
    // This is the ordering used by Nightreign, not the reference SFMT loop.
    int n = 0;
    int n2 = 488;
    int n3 = 616;
    int n4 = 620;
    for (;;) {
        state_[n + 3] = state_[n + 3] ^ (state_[n + 3] << 8) ^
            (state_[n + 2] >> 24) ^ (state_[n3 + 3] >> 8) ^
            ((state_[n2 + 3] >> 11) & 0xBFFFFFF6u) ^ (state_[n4 + 3] << 18);
        state_[n + 2] = state_[n + 2] ^ (state_[n + 2] << 8) ^
            (state_[n + 1] >> 24) ^ (state_[n3 + 3] << 24) ^
            (state_[n3 + 2] >> 8) ^ ((state_[n2 + 2] >> 11) & 0xBFFAFFFFu) ^
            (state_[n4 + 2] << 18);
        state_[n + 1] = state_[n + 1] ^ (state_[n + 1] << 8) ^
            (state_[n] >> 24) ^ (state_[n3 + 2] << 24) ^
            (state_[n3 + 1] >> 8) ^ ((state_[n2 + 1] >> 11) & 0xDDFECB7Fu) ^
            (state_[n4 + 1] << 18);
        state_[n] = state_[n] ^ (state_[n] << 8) ^
            (state_[n3 + 1] << 24) ^ (state_[n3] >> 8) ^
            ((state_[n2] >> 11) & 0xDFFFFFEFu) ^ (state_[n4] << 18);

        std::swap(n3, n4);
        n += 4;
        n2 += 4;
        if (n2 >= 624) n2 = 0;
        if (n >= 624) break;
    }
}

std::uint32_t SFMT19937::next() {
    if (index_ >= 624) {
        generateAll();
        index_ = 0;
    }
    return state_[index_++];
}

std::vector<Block> blocksFor(int nightlord, bool deepOfNight) {
    std::vector<Block> blocks;
    if (nightlord < 0 || nightlord > 9) return blocks;

    if (nightlord < 8) {
        const int base = nightlord * 40;
        addBlock(blocks, L"Standard", deepOfNight ? 7600u : 8400u, base, 20);
        addBlock(blocks, L"Mountaintop", deepOfNight ? 600u : 400u, base + 20, 5);
        addBlock(blocks, L"Crater", deepOfNight ? 600u : 400u, base + 25, 5);
        addBlock(blocks, L"Rotten Woods", deepOfNight ? 600u : 400u, base + 30, 5);
        addBlock(blocks, L"Hidden City", deepOfNight ? 600u : 400u, base + 35, 5);
        if (deepOfNight) {
            const int dlcBase = 1000 + nightlord * 10;
            addBlock(blocks, L"Deep of Night (DLC)", 3800u, dlcBase, 5);
            addBlock(blocks, L"Great Hollow (DLC)", 1800u, dlcBase + 5, 5);
        }
        return blocks;
    }

    const int base = nightlord == 8 ? 1080 : 1140;
    addBlock(blocks, L"Standard (DLC)", deepOfNight ? 5800u : 7652u, base, 20);
    addBlock(blocks, L"Mountaintop (DLC)", deepOfNight ? 600u : 323u, base + 20, 5);
    addBlock(blocks, L"Crater (DLC)", deepOfNight ? 600u : 323u, base + 25, 5);
    addBlock(blocks, L"Rotten Woods (DLC)", deepOfNight ? 600u : 323u, base + 30, 5);
    addBlock(blocks, L"Great Hollow (DLC)", deepOfNight ? 1800u : 1056u, base + 35, 20);
    addBlock(blocks, L"Hidden City (DLC)", deepOfNight ? 600u : 323u, base + 55, 5);
    return blocks;
}

std::optional<Prediction> predict(int nightlord, bool deepOfNight, std::uint32_t seed) {
    const auto blocks = blocksFor(nightlord, deepOfNight);
    if (blocks.empty()) return std::nullopt;

    SFMT19937 rng(seed);
    const std::uint32_t value = rng.next();
    const std::uint32_t total = std::accumulate(
        blocks.begin(), blocks.end(), 0u,
        [](std::uint32_t sum, const Block& block) { return sum + block.weight; });
    const std::uint32_t pick = value % total;
    std::uint32_t accumulated = 0;
    for (const auto& block : blocks) {
        accumulated += block.weight;
        if (accumulated > pick) {
            const int index = static_cast<int>(value % static_cast<std::uint32_t>(block.patternCount));
            return Prediction{block.firstPattern + index, value, block.label, index};
        }
    }
    return std::nullopt;
}

std::optional<SearchResult> findSeedRandom(int pattern, bool deepOfNight,
                                           std::uint32_t pathStart,
                                           std::uint32_t pathStride) {
    const auto owner = nightlordOf(pattern);
    if (!owner || !isReachable(pattern, deepOfNight)) return std::nullopt;
    pathStride |= 1u;
    std::uint32_t seed = pathStart;
    constexpr std::uint64_t space = std::uint64_t{1} << 32;
    for (std::uint64_t drawn = 1; drawn <= space; ++drawn) {
        const auto result = predict(*owner, deepOfNight, seed);
        if (result && result->pattern == pattern) {
            return SearchResult{seed, *result, drawn, pathStart, pathStride};
        }
        seed += pathStride;
    }
    return std::nullopt;
}

std::optional<int> nightlordOf(int pattern) {
    if (pattern >= 0 && pattern <= 319) return pattern / 40;
    if (pattern >= 1000 && pattern <= 1079) return (pattern - 1000) / 10;
    if (pattern >= 1080 && pattern <= 1139) return 8;
    if (pattern >= 1140 && pattern <= 1199) return 9;
    return std::nullopt;
}

bool isReachable(int pattern, bool deepOfNight) {
    const auto owner = nightlordOf(pattern);
    if (!owner) return false;
    for (const auto& block : blocksFor(*owner, deepOfNight)) {
        if (pattern >= block.firstPattern &&
            pattern < block.firstPattern + block.patternCount) return true;
    }
    return false;
}

std::wstring nightlordName(int nightlord) {
    if (nightlord < 0 || nightlord >= static_cast<int>(kNightlords.size())) return L"Unknown";
    return kNightlords[nightlord];
}

std::wstring patternDescription(int pattern) {
    const auto owner = nightlordOf(pattern);
    if (!owner) return L"Unassigned pattern";

    int base = 0;
    std::wstring kind;
    if (*owner < 8 && pattern < 1000) {
        base = *owner * 40;
        const int offset = pattern - base;
        if (offset < 20) kind = kMapKinds[0];
        else kind = kMapKinds[1 + (offset - 20) / 5];
    } else if (*owner < 8) {
        base = 1000 + *owner * 10;
        kind = pattern - base < 5 ? L"Deep of Night (DLC)" : L"Great Hollow (DLC)";
    } else {
        base = *owner == 8 ? 1080 : 1140;
        const int offset = pattern - base;
        if (offset < 20) kind = kMapKinds[0];
        else if (offset < 35) kind = kMapKinds[1 + (offset - 20) / 5];
        else if (offset < 55) kind = kMapKinds[4];
        else kind = kMapKinds[5];
        kind += L" (DLC)";
    }
    return nightlordName(*owner) + L" / " + kind + L" / block #" +
           std::to_wstring(pattern - base);
}

std::wstring modeName(bool deepOfNight) {
    return deepOfNight ? L"Deep of Night" : L"Normal";
}

}  // namespace derandomizer
