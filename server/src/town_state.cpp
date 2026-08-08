#include "acserver/town_runtime.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <ctime>
#include <limits>

namespace acserver {
namespace {

constexpr std::uint32_t kTownStateMagic = 0x41545354U; // ATST
constexpr std::uint16_t kTownStateVersion = 10;
constexpr std::size_t kMaximumStateBytes = 64U * 1024U * 1024U;

bool write_transform(acnet::ByteWriter& writer, const acnet::Transform& value) {
    return writer.f32(value.position.x) && writer.f32(value.position.y) && writer.f32(value.position.z) &&
           writer.f32(value.velocity.x) && writer.f32(value.velocity.y) && writer.f32(value.velocity.z) &&
           writer.i16(value.yaw) && writer.u16(value.action);
}

bool read_transform(acnet::ByteReader& reader, acnet::Transform& value) {
    return reader.f32(value.position.x) && reader.f32(value.position.y) && reader.f32(value.position.z) &&
           reader.f32(value.velocity.x) && reader.f32(value.velocity.y) && reader.f32(value.velocity.z) &&
           reader.i16(value.yaw) && reader.u16(value.action) && acnet::finite(value.position) &&
           acnet::finite(value.velocity);
}

bool write_appearance(acnet::ByteWriter& writer,
                      const acnet::PlayerAppearance& value,
                      const acnet::CustomPattern& pattern) {
    if (value.gender > 2 || value.face >= 8 || value.clothing_index >= 0x108 || value.revision == 0 ||
        pattern.palette >= 16 ||
        (pattern.present ? value.clothing_index < 0x100 : value.clothing_index >= 0x100)) return false;
    if (!writer.bytes(value.name.data(), value.name.size()) || !writer.u8(value.gender) || !writer.u8(value.face) ||
        !writer.u16(value.clothing) ||
        !writer.u16(value.clothing_index) || !writer.u32(value.revision) ||
        !writer.u8(pattern.present ? 1 : 0) || !writer.u8(pattern.palette)) return false;
    return !pattern.present || writer.bytes(pattern.texture.data(), pattern.texture.size());
}

/* `legacy_equipment` receives the held item for checkpoints written before
 * version 8, which stored it inside the appearance. From 8 on it belongs to the
 * inventory and is read with the rest of the pockets, so this stays 0. */
bool read_appearance(acnet::ByteReader& reader,
                     std::uint16_t version,
                     acnet::PlayerAppearance& value,
                     acnet::CustomPattern& pattern,
                     std::uint16_t& legacy_equipment) {
    legacy_equipment = 0;
    if (!reader.bytes(value.name.data(), value.name.size()) || !reader.u8(value.gender) || !reader.u8(value.face) ||
        !reader.u16(value.clothing) || (version < 8 && !reader.u16(legacy_equipment)) ||
        value.gender > 2 || value.face >= 8)
        return false;
    pattern = {};
    if (version < 7) {
        value.clothing_index = value.clothing >= 0x2400 && value.clothing < 0x24FF
                                   ? static_cast<std::uint16_t>(value.clothing - 0x2400)
                                   : 0;
        value.revision = 1;
        return true;
    }
    std::uint8_t present;
    if (!reader.u16(value.clothing_index) || !reader.u32(value.revision) || !reader.u8(present) ||
        !reader.u8(pattern.palette) || present > 1 || value.clothing_index >= 0x108 ||
        value.revision == 0 || pattern.palette >= 16) return false;
    pattern.present = present != 0;
    if (pattern.present ? value.clothing_index < 0x100 : value.clothing_index >= 0x100) return false;
    return !pattern.present || reader.bytes(pattern.texture.data(), pattern.texture.size());
}

acnet::PlayerAppearance legacy_appearance(acnet::AccountId account) {
    acnet::PlayerAppearance value;
    value.name[0] = 'P';
    value.gender = static_cast<std::uint8_t>(account & 1U);
    value.face = static_cast<std::uint8_t>(account % 8U);
    value.clothing = static_cast<std::uint16_t>(0x2400U + account % 0xFFU);
    value.clothing_index = static_cast<std::uint16_t>(value.clothing - 0x2400U);
    value.revision = 1;
    return value;
}

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& values) {
    std::vector<typename Map::key_type> keys;
    keys.reserve(values.size());
    for (const auto& item : values) keys.push_back(item.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

} // namespace

std::uint16_t TownRuntime::town_year() const {
    const std::time_t town_time = static_cast<std::time_t>(clock_.state().town_unix_seconds);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &town_time) != 0) return 0;
#else
    if (gmtime_r(&town_time, &utc) == nullptr) return 0;
#endif
    const int year = utc.tm_year + 1900;
    return year < 0 || year > 65535 ? 0 : static_cast<std::uint16_t>(year);
}

std::vector<std::uint8_t> TownRuntime::encode_state() const {
    acnet::ByteWriter writer(kMaximumStateBytes);
    const std::vector<std::uint8_t> clock = clock_.encode_state();
    if (!writer.u32(kTownStateMagic) || !writer.u16(kTownStateVersion) ||
        !writer.u16(town_bootstrapped_ ? 1 : 0) ||
        !writer.u32(static_cast<std::uint32_t>(clock.size())) || !writer.bytes(clock.data(), clock.size()) ||
        accounts_.size() > std::numeric_limits<std::uint32_t>::max() ||
        !writer.u32(static_cast<std::uint32_t>(accounts_.size()))) return {};
    for (acnet::AccountId account : sorted_keys(accounts_)) {
        AccountState state = accounts_.at(account);
        if (const acnet::PlayerView* active = players_.by_account(account)) {
            state.zone = active->zone;
            state.transform = active->transform;
        }
        const acnet::InventoryState* inventory = world_.inventory(account);
        const acnet::AccountLedger* ledger = economy_.ledger(account);
        if (inventory == nullptr || ledger == nullptr || !writer.u64(account) || !writer.u64(state.entity) ||
            !writer.u8(static_cast<std::uint8_t>(state.kind)) || !writer.u32(state.zone) ||
            !write_transform(writer, state.transform) || !writer.u8(state.resident_slot) ||
            !write_appearance(writer, state.appearance, state.pattern) ||
            !writer.u32(inventory->revision) || !writer.u32(inventory->bells)) return {};
        for (const acnet::ItemSlot& slot : inventory->slots) {
            if (!writer.u16(slot.item) || !writer.u8(slot.condition)) return {};
        }
        if (!writer.u16(inventory->equipped.item) || !writer.u8(inventory->equipped.condition)) return {};
        if (!writer.u32(ledger->revision) || !writer.u64(ledger->bank_balance) || !writer.u64(ledger->debt)) return {};
        /* Only the mailbox revision is stored: the letter list is rebuilt from
         * the mail records below, which carry their own recipient and are
         * replayed in identifier order. */
        const acnet::MailboxState* mailbox = economy_.mailbox(account);
        if (!writer.u32(mailbox == nullptr ? 1 : mailbox->revision)) return {};
    }

    const auto& tiles = world_.tiles();
    if (tiles.size() > std::numeric_limits<std::uint32_t>::max() ||
        !writer.u32(static_cast<std::uint32_t>(tiles.size()))) return {};
    std::vector<std::pair<acnet::TileAddress, acnet::TileState>> ordered_tiles;
    ordered_tiles.reserve(tiles.size());
    for (const auto& item : tiles) ordered_tiles.push_back(item);
    std::sort(ordered_tiles.begin(), ordered_tiles.end(), [](const auto& a, const auto& b) {
        if (a.first.zone != b.first.zone) return a.first.zone < b.first.zone;
        if (a.first.z != b.first.z) return a.first.z < b.first.z;
        return a.first.x < b.first.x;
    });
    for (const auto& item : ordered_tiles) {
        if (!writer.u32(item.first.zone) || !writer.i16(item.first.x) || !writer.i16(item.first.z) ||
            !writer.u32(item.second.revision) || !writer.u16(item.second.item) ||
            !writer.u8(item.second.condition) || !writer.u8(static_cast<std::uint8_t>(item.second.terrain)) ||
            !writer.u8(item.second.buried ? 1 : 0) || !writer.u8(item.second.placed_furniture ? 1 : 0)) return {};
    }

    /* Version 9. The town's own fruit, which the bootstrapping client reports
     * once and pricing needs on every sale thereafter, and the shelf state the
     * daily roll consumes. The rarity permutation was previously re-derived
     * from the town seed on every start, which worked only because nothing
     * mutated it; lifetime sales and the paint rotation both accumulate, so the
     * whole of ShopStockState is now written. */
    if (!writer.u16(native_fruit_) || !writer.u8(static_cast<std::uint8_t>(shop_stock_.tier)) ||
        !writer.i16(shop_stock_.goods_power) || !writer.u16(shop_stock_.rare_item) ||
        !writer.u32(shop_stock_.sales_sum) || !writer.u16(shop_stock_.paint_color)) return {};
    for (const acnet::ShopCategoryPriority& priority : shop_stock_.priorities) {
        if (!writer.u8(priority.a) || !writer.u8(priority.b) || !writer.u8(priority.c)) return {};
    }

    const acnet::ShopState& shop = economy_.shop();
    if (shop.stock.size() > std::numeric_limits<std::uint32_t>::max() || !writer.u32(shop.revision) ||
        !writer.u16(shop.rare_item) ||
        !writer.u32(static_cast<std::uint32_t>(shop.stock.size()))) return {};
    for (const acnet::ShopEntry& entry : shop.stock) {
        if (!writer.u16(entry.item) || !writer.u32(entry.price) || !writer.u16(entry.quantity)) return {};
    }
    const acnet::MuseumState& museum = economy_.museum();
    if (museum.donated_items.size() > std::numeric_limits<std::uint32_t>::max() || !writer.u32(museum.revision) ||
        !writer.u32(static_cast<std::uint32_t>(museum.donated_items.size()))) return {};
    std::vector<std::uint16_t> donated(museum.donated_items.begin(), museum.donated_items.end());
    std::sort(donated.begin(), donated.end());
    for (std::uint16_t item : donated) if (!writer.u16(item)) return {};

    const auto& mail = economy_.mail_records();
    if (mail.size() > std::numeric_limits<std::uint32_t>::max() ||
        !writer.u32(static_cast<std::uint32_t>(mail.size()))) return {};
    for (std::uint64_t id : sorted_keys(mail)) {
        const acnet::MailRecord& record = mail.at(id);
        const acnet::MailContent& content = record.content;
        if (!writer.u64(record.id) || !writer.u64(record.sender) || !writer.u64(record.recipient) ||
            !writer.u16(record.attachment) || !writer.u32(record.revision) ||
            !writer.u8(static_cast<std::uint8_t>(record.location)) || !writer.u8(content.font) ||
            !writer.u8(content.mail_type) || !writer.u8(content.paper_type) ||
            !writer.u8(content.header_back_start) ||
            !writer.bytes(content.sender_name.data(), content.sender_name.size()) ||
            !writer.bytes(content.header.data(), content.header.size()) ||
            !writer.bytes(content.body.data(), content.body.size()) ||
            !writer.bytes(content.footer.data(), content.footer.size())) return {};
    }

    const auto& npcs = npcs_.all_npcs();
    if (npcs.size() > std::numeric_limits<std::uint32_t>::max() ||
        !writer.u32(static_cast<std::uint32_t>(npcs.size()))) return {};
    for (acnet::EntityId id : sorted_keys(npcs)) {
        const acnet::NpcState& npc = npcs.at(id);
        if (!writer.u64(npc.entity) || !writer.u32(npc.zone) || !write_transform(writer, npc.transform) ||
            !writer.u16(npc.schedule_state) || !writer.u16(npc.animation) || !writer.u16(npc.emotion) ||
            !writer.u64(npc.destination) || !writer.u32(npc.revision)) return {};
    }

    const auto& houses = housing_.houses();
    if (houses.size() > std::numeric_limits<std::uint32_t>::max() ||
        !writer.u32(static_cast<std::uint32_t>(houses.size()))) return {};
    for (std::uint64_t id : sorted_keys(houses)) {
        const acnet::HouseState& house = houses.at(id);
        if (house.furniture.size() > std::numeric_limits<std::uint32_t>::max() ||
            !writer.u64(house.house_id) || !writer.u64(house.owner) || !writer.u8(house.original_slot) ||
            !writer.u32(house.zone) || !writer.u8(house.upgrade_level) || !writer.u32(house.revision) ||
            !writer.u8(house.initialized ? 1 : 0) || !writer.u8(house.main_light_on ? 1 : 0) ||
            !writer.u8(house.basement_light_on ? 1 : 0)) return {};
        for (std::int16_t music : house.music_tracks) if (!writer.i16(music)) return {};
        for (std::uint64_t switches : house.furniture_switches) if (!writer.u64(switches)) return {};
        if (!writer.u32(static_cast<std::uint32_t>(house.furniture.size()))) return {};
        std::vector<std::pair<acnet::FurnitureAddress, acnet::ItemSlot>> furniture;
        for (const auto& item : house.furniture) furniture.push_back(item);
        std::sort(furniture.begin(), furniture.end(), [](const auto& a, const auto& b) {
            if (a.first.floor != b.first.floor) return a.first.floor < b.first.floor;
            if (a.first.layer != b.first.layer) return a.first.layer < b.first.layer;
            if (a.first.z != b.first.z) return a.first.z < b.first.z;
            return a.first.x < b.first.x;
        });
        for (const auto& item : furniture) {
            if (!writer.u8(item.first.x) || !writer.u8(item.first.z) || !writer.u8(item.first.floor) ||
                !writer.u8(item.first.layer) ||
                !writer.u16(item.second.item) || !writer.u8(item.second.condition)) return {};
        }
        /* Version 10: the exterior gyroid. */
        if (!writer.u32(house.gyroid.revision)) return {};
        for (const acnet::GyroidItem& entry : house.gyroid.items) {
            if (!writer.u16(entry.item) || !writer.u8(entry.exchange) || !writer.u32(entry.price)) return {};
        }
        if (!writer.bytes(house.gyroid.message.data(), house.gyroid.message.size()) ||
            !writer.u32(house.gyroid.bells)) return {};
    }
    return writer.ok() ? writer.data() : std::vector<std::uint8_t>{};
}

bool TownRuntime::decode_state(const std::vector<std::uint8_t>& payload, std::string& error) {
    error.clear();
    if (payload.empty() || payload.size() > kMaximumStateBytes) { error = "invalid town state size"; return false; }
    acnet::ByteReader reader(payload);
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t reserved;
    std::uint32_t clock_size;
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(reserved) || !reader.u32(clock_size) ||
        magic != kTownStateMagic || (version < 1 || version > kTownStateVersion) ||
        (version < 3 ? reserved != 0 : reserved > 1) ||
        clock_size > reader.remaining()) {
        error = "unsupported or corrupt town state";
        return false;
    }
    // State files written before v3 predate the explicit bootstrap flag, but
    // they already represent an authoritative town.  Treating them as empty
    // would allow the next resident to replace an upgraded server's world.
    town_bootstrapped_ = version < 3 || reserved != 0;
    std::vector<std::uint8_t> clock(clock_size);
    if (!reader.bytes(clock.data(), clock.size()) || !clock_.decode_state(clock)) {
        error = "invalid clock in town state";
        return false;
    }
    std::uint32_t account_count;
    if (!reader.u32(account_count) || account_count > 100000) { error = "invalid account count"; return false; }
    for (std::uint32_t i = 0; i < account_count; ++i) {
        acnet::AccountId account;
        AccountState state;
        std::uint8_t kind;
        acnet::InventoryState inventory;
        acnet::AccountLedger ledger;
        std::uint16_t legacy_equipment = 0;
        if (!reader.u64(account) || !reader.u64(state.entity) || !reader.u8(kind) ||
            kind > static_cast<std::uint8_t>(acnet::PlayerKind::Visitor) || !reader.u32(state.zone) ||
            !read_transform(reader, state.transform) || !reader.u8(state.resident_slot) ||
            (version >= 2 &&
             !read_appearance(reader, version, state.appearance, state.pattern, legacy_equipment)) ||
            !reader.u32(inventory.revision) || !reader.u32(inventory.bells)) {
            error = "invalid account state"; return false;
        }
        if (version == 1) state.appearance = legacy_appearance(account);
        state.kind = static_cast<acnet::PlayerKind>(kind);
        for (acnet::ItemSlot& slot : inventory.slots) {
            if (!reader.u16(slot.item) || !reader.u8(slot.condition)) { error = "invalid inventory"; return false; }
        }
        if (version >= 8) {
            if (!reader.u16(inventory.equipped.item) || !reader.u8(inventory.equipped.condition)) {
                error = "invalid held item"; return false;
            }
        } else {
            inventory.equipped.item = legacy_equipment;
        }
        acnet::Revision mailbox_revision = 1;
        if (!reader.u32(ledger.revision) || !reader.u64(ledger.bank_balance) || !reader.u64(ledger.debt) ||
            (version >= 5 && !reader.u32(mailbox_revision)) ||
            account == 0 || state.entity == 0 || state.zone == 0 || inventory.revision == 0 ||
            ledger.revision == 0 || mailbox_revision == 0) {
            error = "invalid account ledger"; return false;
        }
        accounts_[account] = state;
        if (entities_.by_id(state.entity) == nullptr) {
            const acnet::EntityRecord entity{state.entity, static_cast<std::uintptr_t>(account), 0, state.zone,
                                             entities_.process_generation(), 1};
            if (!entities_.restore(entity)) { error = "invalid persisted entity"; return false; }
        }
        if (!world_.set_inventory(account, inventory) || !economy_.set_account(account, ledger) ||
            !economy_.restore_mailbox_revision(account, mailbox_revision)) {
            error = "failed to restore account authorities"; return false;
        }
    }

    std::uint32_t tile_count;
    if (!reader.u32(tile_count) || tile_count > 1000000) { error = "invalid tile count"; return false; }
    for (std::uint32_t i = 0; i < tile_count; ++i) {
        acnet::TileAddress address;
        acnet::TileState tile;
        std::uint8_t terrain;
        std::uint8_t buried;
        std::uint8_t furniture;
        if (!reader.u32(address.zone) || !reader.i16(address.x) || !reader.i16(address.z) ||
            !reader.u32(tile.revision) || !reader.u16(tile.item) || !reader.u8(tile.condition) ||
            !reader.u8(terrain) || !reader.u8(buried) || !reader.u8(furniture) || address.zone == 0 ||
            tile.revision == 0 || terrain > static_cast<std::uint8_t>(acnet::TerrainState::Planted) ||
            buried > 1 || furniture > 1) { error = "invalid tile state"; return false; }
        tile.terrain = static_cast<acnet::TerrainState>(terrain);
        tile.buried = buried != 0;
        tile.placed_furniture = furniture != 0;
        if (!world_.set_tile(address, tile)) { error = "failed to restore tile"; return false; }
    }

    /* Towns written before version 9 recorded neither the fruit nor the shelf
     * state. Zero fruit prices everything as foreign until the next bootstrap
     * reports one, and the shelf keeps the seed-derived permutation
     * initialize() already installed -- both being the state a brand-new town
     * starts in. */
    native_fruit_ = 0;
    if (version >= 9) {
        std::uint8_t tier;
        if (!reader.u16(native_fruit_) || !reader.u8(tier) || tier > 3 ||
            !reader.i16(shop_stock_.goods_power) || !reader.u16(shop_stock_.rare_item) ||
            !reader.u32(shop_stock_.sales_sum) || !reader.u16(shop_stock_.paint_color)) {
            error = "invalid shop stock state";
            return false;
        }
        shop_stock_.tier = static_cast<acnet::ShopTier>(tier);
        for (acnet::ShopCategoryPriority& priority : shop_stock_.priorities) {
            if (!reader.u8(priority.a) || !reader.u8(priority.b) || !reader.u8(priority.c) ||
                priority.a > 2 || priority.b > 2 || priority.c > 2) {
                error = "invalid shop rarity permutation";
                return false;
            }
        }
    }

    acnet::ShopState shop;
    std::uint32_t shop_count;
    if (!reader.u32(shop.revision) || (version >= 9 && !reader.u16(shop.rare_item)) || !reader.u32(shop_count) ||
        shop.revision == 0 || shop_count > 65535) {
        error = "invalid shop state"; return false;
    }
    shop.stock.resize(shop_count);
    for (acnet::ShopEntry& entry : shop.stock) {
        if (!reader.u16(entry.item) || !reader.u32(entry.price) || !reader.u16(entry.quantity)) {
            error = "invalid shop stock"; return false;
        }
    }
    economy_.set_shop(shop);
    acnet::MuseumState museum;
    std::uint32_t museum_count;
    if (!reader.u32(museum.revision) || !reader.u32(museum_count) || museum.revision == 0 || museum_count > 65535) {
        error = "invalid museum state"; return false;
    }
    for (std::uint32_t i = 0; i < museum_count; ++i) {
        std::uint16_t item;
        if (!reader.u16(item) || item == 0) { error = "invalid museum item"; return false; }
        museum.donated_items.insert(item);
    }
    economy_.set_museum(museum);

    std::uint32_t mail_count;
    if (!reader.u32(mail_count) || mail_count > 100000) { error = "invalid mail count"; return false; }
    /* The decoded payload is the whole truth about pending mail, and this
     * function runs twice at startup -- once for the checkpoint, once for the
     * newest journalled state. */
    economy_.clear_mail();
    for (std::uint32_t i = 0; i < mail_count; ++i) {
        acnet::MailRecord mail;
        acnet::MailContent& content = mail.content;
        std::uint8_t location = 0;
        /* v5 letters predate carried mail and the full letter body: they were
         * all waiting in a mailbox and had only a 96-byte note, which maps onto
         * the head of the body field. */
        std::array<std::uint8_t, 96> legacy_text{};
        if (!reader.u64(mail.id) || !reader.u64(mail.sender) || !reader.u64(mail.recipient) ||
            !reader.u16(mail.attachment) || !reader.u32(mail.revision) ||
            (version == 5 && !reader.bytes(legacy_text.data(), legacy_text.size())) ||
            (version >= 6 &&
             (!reader.u8(location) || location > static_cast<std::uint8_t>(acnet::MailLocation::Carried) ||
              !reader.u8(content.font) || !reader.u8(content.mail_type) || !reader.u8(content.paper_type) ||
              !reader.u8(content.header_back_start) ||
              !reader.bytes(content.sender_name.data(), content.sender_name.size()) ||
              !reader.bytes(content.header.data(), content.header.size()) ||
              !reader.bytes(content.body.data(), content.body.size()) ||
              !reader.bytes(content.footer.data(), content.footer.size())))) {
            error = "invalid mail state"; return false;
        }
        if (version == 5) std::copy(legacy_text.begin(), legacy_text.end(), content.body.begin());
        mail.location = static_cast<acnet::MailLocation>(location);
        if (!economy_.restore_mail(mail)) { error = "invalid mail state"; return false; }
    }

    std::uint32_t npc_count;
    if (!reader.u32(npc_count) || npc_count > 4096) { error = "invalid NPC count"; return false; }
    for (std::uint32_t i = 0; i < npc_count; ++i) {
        acnet::NpcState npc;
        if (!reader.u64(npc.entity) || !reader.u32(npc.zone) || !read_transform(reader, npc.transform) ||
            !reader.u16(npc.schedule_state) || !reader.u16(npc.animation) || !reader.u16(npc.emotion) ||
            !reader.u64(npc.destination) || !reader.u32(npc.revision) || npc.entity == 0 || npc.zone == 0 ||
            npc.revision == 0) { error = "invalid NPC state"; return false; }
        if (acnet::NpcState* existing = npcs_.npc(npc.entity)) *existing = npc;
        else if (!npcs_.add_npc(npc)) { error = "failed to restore NPC"; return false; }
    }

    std::uint32_t house_count;
    /* The four resident houses plus the town's shared island cabin. */
    if (!reader.u32(house_count) || house_count > acnet::kOriginalResidentSlots + 1) {
        error = "invalid house count"; return false;
    }
    for (std::uint32_t i = 0; i < house_count; ++i) {
        acnet::HouseState house;
        std::uint32_t furniture_count;
        std::uint8_t initialized = 0;
        std::uint8_t main_light = 0;
        std::uint8_t basement_light = 0;
        if (!reader.u64(house.house_id) || !reader.u64(house.owner) || !reader.u8(house.original_slot) ||
            !reader.u32(house.zone) || !reader.u8(house.upgrade_level) || !reader.u32(house.revision) ||
            (version >= 4 && (!reader.u8(initialized) || !reader.u8(main_light) || !reader.u8(basement_light))) ||
            (version >= 4 && (initialized > 1 || main_light > 1 || basement_light > 1))) {
            error = "invalid house state"; return false;
        }
        if (version >= 4) {
            for (std::int16_t& music : house.music_tracks) {
                if (!reader.i16(music)) { error = "invalid house music state"; return false; }
            }
            for (std::uint64_t& switches : house.furniture_switches) {
                if (!reader.u64(switches)) { error = "invalid furniture switch state"; return false; }
            }
        }
        if (!reader.u32(furniture_count) || furniture_count > acnet::kMaximumHouseFurniture) {
            error = "invalid house state"; return false;
        }
        house.initialized = initialized != 0;
        house.main_light_on = main_light != 0;
        house.basement_light_on = basement_light != 0;
        /* Ownership is the record: a shared house is the ownerless one. Kept
         * derived rather than stored so the two can never disagree, exactly as
         * on the wire. */
        house.shared = house.original_slot == acnet::kSharedHouseSlot;
        for (std::uint32_t j = 0; j < furniture_count; ++j) {
            acnet::FurnitureAddress address;
            acnet::ItemSlot item;
            if (!reader.u8(address.x) || !reader.u8(address.z) ||
                (version >= 4 && !reader.u8(address.floor)) || !reader.u8(address.layer) ||
                !reader.u16(item.item) || !reader.u8(item.condition) || item.item == 0) {
                error = "invalid furniture state"; return false;
            }
            house.furniture[address] = item;
        }
        if (version >= 10) {
            bool gyroid_ok = reader.u32(house.gyroid.revision) && house.gyroid.revision != 0;
            for (acnet::GyroidItem& entry : house.gyroid.items) {
                gyroid_ok = gyroid_ok && reader.u16(entry.item) && reader.u8(entry.exchange) &&
                            reader.u32(entry.price);
            }
            gyroid_ok = gyroid_ok &&
                        reader.bytes(house.gyroid.message.data(), house.gyroid.message.size()) &&
                        reader.u32(house.gyroid.bells);
            if (!gyroid_ok) { error = "invalid gyroid state"; return false; }
        }
        if (version < 4) house.initialized = furniture_count != 0;
        if (!housing_.restore_house(house)) { error = "failed to restore house"; return false; }
    }
    if (!reader.finished()) { error = "trailing town state data"; return false; }
    /* A town flagged as bootstrapped with no foreground behind it is worse than
     * one that was never bootstrapped: town_bootstrapped_ closes the only path
     * that installs a world, and every interest window the town then answers is
     * an all-empty chunk which the client writes straight over its own field.
     * Trees, rocks and the bulletin board disappear one acre at a time as the
     * player walks up to them, and never come back. Pre-v3 state has no flag to
     * read and is assumed bootstrapped, which is how such a town arises.
     * Nothing here can be lost by reopening the door -- there is nothing to
     * lose -- so let the next resident install the foreground. */
    if (town_bootstrapped_) {
        const auto town_tiles = world_.tiles_in_zone(1);
        const std::size_t occupied =
            static_cast<std::size_t>(std::count_if(town_tiles.begin(), town_tiles.end(),
                                                   [](const auto& entry) { return entry.second.item != 0; }));
        /* One acre's worth. A generated town's foreground is thousands of tiles
         * -- trees, rocks, weeds, signs -- across every acre, so any real one
         * clears this by an order of magnitude. A town that was flagged as
         * created without ever being given a foreground still accumulates a
         * handful of occupied tiles from dropped items, which is why the test
         * is a floor rather than "any". */
        if (occupied < acnet::kBlockUnits * acnet::kBlockUnits) town_bootstrapped_ = false;
    }
    return true;
}

bool TownRuntime::commit_state(std::uint16_t record_type, std::string& error) {
    const std::vector<std::uint8_t> state = encode_state();
    if (state.empty()) { error = "failed to serialize town state"; return false; }
    return persistence_.append({persistence_.last_sequence() + 1, record_type, state}, error);
}

bool TownRuntime::commit_transaction(acnet::AccountId account,
                                     std::uint16_t operation_type,
                                     acnet::ResultCode result,
                                     std::string& error) {
    if (!commit_state(100, error)) return false;
    return database_.record_transaction(persistence_.last_sequence(), account, operation_type,
                                        result, wall_unix_seconds(), error);
}

} // namespace acserver
