#pragma once

#include "acnet/economy.hpp"
#include "acnet/world.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace acserver {

constexpr std::size_t kGciResidentCount = 4;

struct GciResidentState {
    bool exists = false;
    acnet::InventoryState inventory;
    acnet::AccountLedger ledger;
    acnet::PlayerAppearance appearance;
    acnet::CustomPattern pattern;
};

struct GciTownState {
    std::vector<std::pair<acnet::TileAddress, acnet::TileState>> tiles;
    std::array<GciResidentState, kGciResidentCount> residents{};
    std::uint8_t weather = 0;
    std::uint8_t weather_intensity = 0;
};

bool decode_gci_town(const std::vector<std::uint8_t>& bytes,
                     GciTownState& state,
                     std::string& error);
bool encode_gci_town(std::vector<std::uint8_t>& bytes,
                     const GciTownState& state,
                     std::string& error);

} // namespace acserver
