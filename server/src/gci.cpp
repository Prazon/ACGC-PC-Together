#include "acserver/gci.hpp"

#include <algorithm>
#include <limits>

namespace acserver {
namespace {

constexpr std::size_t kHeaderSize = 0x40;
constexpr std::size_t kBlockSize = 0x2000;
constexpr std::size_t kFileDataSize = 0x72000;
constexpr std::size_t kSaveMainOffset = kHeaderSize + 0x26000;
constexpr std::size_t kSaveBackupOffset = kHeaderSize + 0x4C000;
constexpr std::size_t kSaveAlignedSize = 0x26000;
constexpr std::size_t kSaveSize = 0x242A0;
constexpr std::size_t kChecksumOffset = 0x12;
constexpr std::size_t kPrivateOffset = 0x20;
constexpr std::size_t kPrivateSize = 0x2440;
constexpr std::size_t kPrivatePocketsOffset = 0x68;
constexpr std::size_t kPrivateGenderOffset = 0x14;
constexpr std::size_t kPrivateFaceOffset = 0x15;
constexpr std::size_t kPrivateConditionsOffset = 0x88;
constexpr std::size_t kPrivateWalletOffset = 0x8C;
constexpr std::size_t kPrivateLoanOffset = 0x90;
constexpr std::size_t kPrivateEquipmentOffset = 0x4A4;
constexpr std::size_t kPrivateExistsOffset = 0x1086;
constexpr std::size_t kPrivateClothingOffset = 0x108A;
constexpr std::size_t kPrivateBankOffset = 0x122C;
constexpr std::size_t kForegroundOffset = 0x137A8;
constexpr std::size_t kForegroundBlockSize = 0x200;
constexpr std::size_t kDepositOffset = 0x20F1C;
constexpr std::size_t kWeatherOffset = 0x20F19;
constexpr int kBlocksX = 5;
constexpr int kBlocksZ = 6;
constexpr int kUnits = 16;

std::uint16_t read_be16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) |
                                      bytes[offset + 1]);
}

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           bytes[offset + 3];
}

void write_be16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void write_be32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

bool valid_gci(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != kHeaderSize + kFileDataSize ||
        !(bytes[0] == 'G' && bytes[1] == 'A' && bytes[2] == 'F' &&
          (bytes[3] == 'E' || bytes[3] == 'U')) ||
        bytes[4] != '0' || bytes[5] != '1') return false;
    const std::uint16_t blocks = read_be16(bytes, 0x38);
    return blocks == kFileDataSize / kBlockSize;
}

std::uint16_t checksum(const std::vector<std::uint8_t>& bytes, std::size_t save_offset) {
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset < kSaveSize; offset += 2) {
        sum += read_be16(bytes, save_offset + offset);
    }
    return static_cast<std::uint16_t>(0U - (sum & 0xFFFFU));
}

std::size_t tile_item_offset(std::size_t save_offset, int block_x, int block_z, int unit_x, int unit_z) {
    const int block = block_z * kBlocksX + block_x;
    return save_offset + kForegroundOffset + static_cast<std::size_t>(block) * kForegroundBlockSize +
           static_cast<std::size_t>(unit_z * kUnits + unit_x) * 2;
}

std::size_t deposit_row_offset(std::size_t save_offset, int block_x, int block_z, int unit_z) {
    const int block = block_z * kBlocksX + block_x;
    return save_offset + kDepositOffset + static_cast<std::size_t>(block * kUnits + unit_z) * 2;
}

const acnet::TileState* find_tile(const GciTownState& state, std::int16_t x, std::int16_t z) {
    const auto found = std::find_if(state.tiles.begin(), state.tiles.end(), [&](const auto& entry) {
        return entry.first.zone == 1 && entry.first.x == x && entry.first.z == z;
    });
    return found == state.tiles.end() ? nullptr : &found->second;
}

acnet::TerrainState terrain_for_item(std::uint16_t item) {
    if ((item >= 0x0011 && item <= 0x0029) || (item >= 0x002A && item <= 0x005B))
        return acnet::TerrainState::Hole;
    if ((item >= 0x0001 && item <= 0x0004) || (item >= 0x0070 && item <= 0x0077) ||
        (item >= 0x007B && item <= 0x007E)) return acnet::TerrainState::Stump;
    if ((item >= 0x005E && item <= 0x0061) || item == 0x0069 ||
        (item >= 0x0078 && item <= 0x0082)) return acnet::TerrainState::Tree;
    if (item < 0x0800 || item > 0x0869) return acnet::TerrainState::Normal;
    const std::uint16_t offset = static_cast<std::uint16_t>(item - 0x0800);
    if ((offset <= 3) ||
        (offset >= 5 && offset <= 8) || (offset >= 13 && offset <= 16) ||
        (offset >= 21 && offset <= 24) || (offset >= 29 && offset <= 32) ||
        (offset >= 37 && offset <= 40) || (offset >= 45 && offset <= 48) ||
        (offset >= 50 && offset <= 53) || (offset >= 55 && offset <= 58) ||
        (offset >= 60 && offset <= 68) || offset == 78 ||
        (offset >= 79 && offset <= 82) || (offset >= 84 && offset <= 87) || offset == 92 ||
        (offset >= 93 && offset <= 96) || offset == 98 ||
        (offset >= 99 && offset <= 102) || offset == 105) return acnet::TerrainState::Planted;
    return acnet::TerrainState::Tree;
}

void patch_save(std::vector<std::uint8_t>& bytes,
                std::size_t save_offset,
                const GciTownState& state) {
    for (int block_z = 0; block_z < kBlocksZ; ++block_z) {
        for (int block_x = 0; block_x < kBlocksX; ++block_x) {
            for (int unit_z = 0; unit_z < kUnits; ++unit_z) {
                std::uint16_t deposit = 0;
                for (int unit_x = 0; unit_x < kUnits; ++unit_x) {
                    const std::int16_t x = static_cast<std::int16_t>((block_x + 1) * kUnits + unit_x);
                    const std::int16_t z = static_cast<std::int16_t>((block_z + 1) * kUnits + unit_z);
                    const acnet::TileState* tile = find_tile(state, x, z);
                    if (tile == nullptr) continue;
                    write_be16(bytes, tile_item_offset(save_offset, block_x, block_z, unit_x, unit_z), tile->item);
                    if (tile->buried) deposit = static_cast<std::uint16_t>(deposit | (1U << unit_x));
                }
                write_be16(bytes, deposit_row_offset(save_offset, block_x, block_z, unit_z), deposit);
            }
        }
    }
    for (std::size_t slot = 0; slot < state.residents.size(); ++slot) {
        const GciResidentState& resident = state.residents[slot];
        const std::size_t base = save_offset + kPrivateOffset + slot * kPrivateSize;
        if (!resident.exists) continue;
        std::copy(resident.appearance.name.begin(), resident.appearance.name.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(base));
        bytes[base + kPrivateGenderOffset] = resident.appearance.gender;
        bytes[base + kPrivateFaceOffset] = resident.appearance.face;
        write_be16(bytes, base + kPrivateEquipmentOffset, resident.appearance.equipped_item);
        write_be16(bytes, base + kPrivateClothingOffset, resident.appearance.clothing);
        std::uint32_t conditions = 0;
        for (std::size_t item = 0; item < resident.inventory.slots.size(); ++item) {
            write_be16(bytes, base + kPrivatePocketsOffset + item * 2,
                       resident.inventory.slots[item].item);
            conditions |= static_cast<std::uint32_t>(resident.inventory.slots[item].condition & 3U) << (item * 2);
        }
        write_be32(bytes, base + kPrivateConditionsOffset, conditions);
        write_be32(bytes, base + kPrivateWalletOffset, resident.inventory.bells);
        write_be32(bytes, base + kPrivateLoanOffset,
                   static_cast<std::uint32_t>(std::min<std::uint64_t>(resident.ledger.debt,
                                                                      std::numeric_limits<std::uint32_t>::max())));
        write_be32(bytes, base + kPrivateBankOffset,
                   static_cast<std::uint32_t>(std::min<std::uint64_t>(resident.ledger.bank_balance,
                                                                      std::numeric_limits<std::uint32_t>::max())));
        bytes[base + kPrivateExistsOffset] = 1;
    }
    bytes[save_offset + kWeatherOffset] = static_cast<std::uint8_t>((state.weather_intensity << 4) |
                                                                    (state.weather & 0x0FU));
    write_be16(bytes, save_offset + kChecksumOffset, 0);
    write_be16(bytes, save_offset + kChecksumOffset, checksum(bytes, save_offset));
}

} // namespace

bool decode_gci_town(const std::vector<std::uint8_t>& bytes,
                     GciTownState& state,
                     std::string& error) {
    error.clear();
    if (!valid_gci(bytes)) {
        error = "invalid or unsupported Animal Crossing town GCI";
        return false;
    }
    state = {};
    state.tiles.reserve(kBlocksX * kBlocksZ * kUnits * kUnits);
    for (int block_z = 0; block_z < kBlocksZ; ++block_z) {
        for (int block_x = 0; block_x < kBlocksX; ++block_x) {
            for (int unit_z = 0; unit_z < kUnits; ++unit_z) {
                const std::uint16_t deposit = read_be16(bytes,
                    deposit_row_offset(kSaveMainOffset, block_x, block_z, unit_z));
                for (int unit_x = 0; unit_x < kUnits; ++unit_x) {
                    acnet::TileAddress address;
                    address.zone = 1;
                    address.x = static_cast<std::int16_t>((block_x + 1) * kUnits + unit_x);
                    address.z = static_cast<std::int16_t>((block_z + 1) * kUnits + unit_z);
                    acnet::TileState tile;
                    tile.item = read_be16(bytes,
                        tile_item_offset(kSaveMainOffset, block_x, block_z, unit_x, unit_z));
                    tile.terrain = terrain_for_item(tile.item);
                    tile.buried = (deposit & (1U << unit_x)) != 0;
                    state.tiles.emplace_back(address, tile);
                }
            }
        }
    }
    for (std::size_t slot = 0; slot < state.residents.size(); ++slot) {
        GciResidentState& resident = state.residents[slot];
        const std::size_t base = kSaveMainOffset + kPrivateOffset + slot * kPrivateSize;
        resident.exists = bytes[base + kPrivateExistsOffset] != 0;
        std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(base),
                  bytes.begin() + static_cast<std::ptrdiff_t>(base + resident.appearance.name.size()),
                  resident.appearance.name.begin());
        resident.appearance.gender = bytes[base + kPrivateGenderOffset];
        resident.appearance.face = bytes[base + kPrivateFaceOffset];
        resident.appearance.equipped_item = read_be16(bytes, base + kPrivateEquipmentOffset);
        resident.appearance.clothing = read_be16(bytes, base + kPrivateClothingOffset);
        const std::uint32_t conditions = read_be32(bytes, base + kPrivateConditionsOffset);
        for (std::size_t item = 0; item < resident.inventory.slots.size(); ++item) {
            resident.inventory.slots[item].item = read_be16(bytes, base + kPrivatePocketsOffset + item * 2);
            resident.inventory.slots[item].condition = static_cast<std::uint8_t>((conditions >> (item * 2)) & 3U);
        }
        resident.inventory.bells = read_be32(bytes, base + kPrivateWalletOffset);
        resident.ledger.debt = read_be32(bytes, base + kPrivateLoanOffset);
        resident.ledger.bank_balance = read_be32(bytes, base + kPrivateBankOffset);
    }
    const std::uint8_t weather = bytes[kSaveMainOffset + kWeatherOffset];
    state.weather = weather & 0x0FU;
    state.weather_intensity = weather >> 4;
    return true;
}

bool encode_gci_town(std::vector<std::uint8_t>& bytes,
                     const GciTownState& state,
                     std::string& error) {
    error.clear();
    if (!valid_gci(bytes) || state.tiles.size() > kBlocksX * kBlocksZ * kUnits * kUnits) {
        error = "invalid GCI template or town state";
        return false;
    }
    patch_save(bytes, kSaveMainOffset, state);
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(kSaveMainOffset),
              bytes.begin() + static_cast<std::ptrdiff_t>(kSaveMainOffset + kSaveAlignedSize),
              bytes.begin() + static_cast<std::ptrdiff_t>(kSaveBackupOffset));
    return true;
}

} // namespace acserver
