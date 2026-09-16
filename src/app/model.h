#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace derandomizer {

struct Block {
    std::wstring label;
    std::uint32_t weight{};
    int firstPattern{};
    int patternCount{};
};

struct Prediction {
    int pattern{};
    std::uint32_t draw{};
    std::wstring block;
    int blockIndex{};
};

struct SearchResult {
    std::uint32_t seed{};
    Prediction prediction;
    std::uint64_t draws{};
    std::uint32_t pathStart{};
    std::uint32_t pathStride{};
};

class SFMT19937 {
public:
    explicit SFMT19937(std::uint32_t seed);
    std::uint32_t next();

private:
    void generateAll();
    std::uint32_t state_[624]{};
    int index_ = 624;
};

std::vector<Block> blocksFor(int nightlord, bool deepOfNight);
std::optional<Prediction> predict(int nightlord, bool deepOfNight, std::uint32_t seed);
std::optional<SearchResult> findSeedRandom(int pattern, bool deepOfNight,
                                           std::uint32_t pathStart,
                                           std::uint32_t pathStride);
std::optional<int> nightlordOf(int pattern);
bool isReachable(int pattern, bool deepOfNight);
std::wstring nightlordName(int nightlord);
std::wstring patternDescription(int pattern);
std::wstring modeName(bool deepOfNight);

}  // namespace derandomizer
