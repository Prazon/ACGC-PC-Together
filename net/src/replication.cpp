#include "acnet/replication.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace acnet {
namespace {

constexpr std::size_t kMaximumBaselineBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumBaselineTiles = 65535;
constexpr std::size_t kMaximumBaselineNpcs = 256;
constexpr std::size_t kMaximumShopEntries = 256;

/* A letter is bounded and self-describing: identifier, both accounts, the
 * attached item, where it currently sits, and fixed-size text carried as opaque
 * bytes in the game's own encoding. Sender 0 is the town operator. */
bool encode_mail(ByteWriter& writer, const MailRecord& letter) {
    const MailContent& content = letter.content;
    return letter.id != 0 && letter.recipient != 0 && letter.revision != 0 && writer.u64(letter.id) &&
           writer.u64(letter.sender) && writer.u64(letter.recipient) && writer.u16(letter.attachment) &&
           writer.u32(letter.revision) && writer.u8(static_cast<std::uint8_t>(letter.location)) &&
           writer.u8(content.font) && writer.u8(content.mail_type) && writer.u8(content.paper_type) &&
           writer.u8(content.header_back_start) &&
           writer.bytes(content.sender_name.data(), content.sender_name.size()) &&
           writer.bytes(content.header.data(), content.header.size()) &&
           writer.bytes(content.body.data(), content.body.size()) &&
           writer.bytes(content.footer.data(), content.footer.size());
}

bool decode_mail(ByteReader& reader, MailRecord& letter) {
    MailContent& content = letter.content;
    std::uint8_t location;
    if (!reader.u64(letter.id) || !reader.u64(letter.sender) || !reader.u64(letter.recipient) ||
        !reader.u16(letter.attachment) || !reader.u32(letter.revision) || !reader.u8(location) ||
        location > static_cast<std::uint8_t>(MailLocation::Carried) || !reader.u8(content.font) ||
        !reader.u8(content.mail_type) || !reader.u8(content.paper_type) ||
        !reader.u8(content.header_back_start) ||
        !reader.bytes(content.sender_name.data(), content.sender_name.size()) ||
        !reader.bytes(content.header.data(), content.header.size()) ||
        !reader.bytes(content.body.data(), content.body.size()) ||
        !reader.bytes(content.footer.data(), content.footer.size()) || letter.id == 0 ||
        letter.recipient == 0 || letter.revision == 0 || letter.sender == letter.recipient) return false;
    letter.location = static_cast<MailLocation>(location);
    return true;
}

bool valid_transition(const PlayerSnapshot& player) {
    const std::uint8_t phase = static_cast<std::uint8_t>(player.transition_phase);
    return phase <= static_cast<std::uint8_t>(DoorTransitionPhase::Arriving) &&
           ((phase == 0 && player.transition_door == 0 && player.transition_expires_tick == 0) ||
            (phase != 0 && player.transition_door != 0 && player.transition_expires_tick != 0));
}

/* A shared house is identified on the wire by the ownerless (owner, slot)
 * pair rather than a separate flag, so the two can never disagree. */
bool valid_house_ownership(const HouseState& house) {
    if (house.shared) return house.owner == 0 && house.original_slot == kSharedHouseSlot;
    return house.owner != 0 && house.original_slot < kOriginalResidentSlots;
}

bool encode_house(ByteWriter& writer, const HouseState& house) {
    if (house.house_id == 0 || !valid_house_ownership(house) ||
        house.zone == 0 || house.upgrade_level > kMaximumHouseUpgradeLevel || house.revision == 0 ||
        house.furniture.size() > kMaximumHouseFurniture) return false;
    if (!writer.u64(house.house_id) || !writer.u64(house.owner) || !writer.u8(house.original_slot) ||
        !writer.u32(house.zone) || !writer.u8(house.upgrade_level) || !writer.u32(house.revision) ||
        !writer.u8(house.initialized ? 1 : 0) || !writer.u8(house.main_light_on ? 1 : 0) ||
        !writer.u8(house.basement_light_on ? 1 : 0)) return false;
    for (std::int16_t music : house.music_tracks) {
        if (!writer.i16(music)) return false;
    }
    for (std::uint64_t switches : house.furniture_switches) {
        if (!writer.u64(switches)) return false;
    }
    if (!encode_house_surfaces(writer, house.surfaces)) return false;
    for (std::uint32_t songs : house.music_box) {
        if (!writer.u32(songs)) return false;
    }
    std::vector<std::pair<FurnitureAddress, ItemSlot>> furniture(house.furniture.begin(), house.furniture.end());
    std::sort(furniture.begin(), furniture.end(), [](const auto& left, const auto& right) {
        if (left.first.floor != right.first.floor) return left.first.floor < right.first.floor;
        if (left.first.layer != right.first.layer) return left.first.layer < right.first.layer;
        if (left.first.z != right.first.z) return left.first.z < right.first.z;
        return left.first.x < right.first.x;
    });
    const std::size_t floor_limit = house.shared ? kSharedHouseFloorCount : kHouseFloorCount;
    if (!writer.u16(static_cast<std::uint16_t>(furniture.size()))) return false;
    for (const auto& entry : furniture) {
        if (entry.first.x >= 16 || entry.first.z >= 16 || entry.first.floor >= floor_limit ||
            entry.first.layer >= kHouseLayerCount || entry.second.item == 0 ||
            !writer.u8(entry.first.x) || !writer.u8(entry.first.z) || !writer.u8(entry.first.floor) ||
            !writer.u8(entry.first.layer) || !writer.u16(entry.second.item) ||
            !writer.u8(entry.second.condition)) return false;
    }
    return true;
}

bool decode_house(ByteReader& reader, HouseState& house) {
    std::uint8_t initialized;
    std::uint8_t main_light;
    std::uint8_t basement_light;
    std::uint16_t count;
    if (!reader.u64(house.house_id) || !reader.u64(house.owner) || !reader.u8(house.original_slot) ||
        !reader.u32(house.zone) || !reader.u8(house.upgrade_level) || !reader.u32(house.revision) ||
        !reader.u8(initialized) || !reader.u8(main_light) || !reader.u8(basement_light) ||
        initialized > 1 || main_light > 1 || basement_light > 1) return false;
    for (std::int16_t& music : house.music_tracks) {
        if (!reader.i16(music)) return false;
    }
    for (std::uint64_t& switches : house.furniture_switches) {
        if (!reader.u64(switches)) return false;
    }
    if (!decode_house_surfaces(reader, house.surfaces)) return false;
    for (std::uint32_t& songs : house.music_box) {
        if (!reader.u32(songs)) return false;
    }
    house.shared = house.original_slot == kSharedHouseSlot;
    if (!reader.u16(count) || count > kMaximumHouseFurniture || house.house_id == 0 ||
        !valid_house_ownership(house) || house.zone == 0 ||
        house.upgrade_level > kMaximumHouseUpgradeLevel ||
        house.revision == 0) return false;
    house.initialized = initialized != 0;
    house.main_light_on = main_light != 0;
    house.basement_light_on = basement_light != 0;
    house.furniture.clear();
    for (std::uint16_t i = 0; i < count; ++i) {
        FurnitureAddress address;
        ItemSlot item;
        if (!reader.u8(address.x) || !reader.u8(address.z) || !reader.u8(address.floor) ||
            !reader.u8(address.layer) || !reader.u16(item.item) || !reader.u8(item.condition) ||
            address.x >= 16 || address.z >= 16 ||
            address.floor >= (house.shared ? kSharedHouseFloorCount : kHouseFloorCount) ||
            address.layer >= kHouseLayerCount || item.item == 0 ||
            !house.furniture.emplace(address, item).second) return false;
    }
    return true;
}

bool encode_transform(ByteWriter& writer, const Transform& value) {
    return writer.f32(value.position.x) && writer.f32(value.position.y) && writer.f32(value.position.z) &&
           writer.f32(value.velocity.x) && writer.f32(value.velocity.y) && writer.f32(value.velocity.z) &&
           writer.i16(value.yaw) && writer.u16(value.action);
}

bool decode_transform(ByteReader& reader, Transform& value) {
    return reader.f32(value.position.x) && reader.f32(value.position.y) && reader.f32(value.position.z) &&
           reader.f32(value.velocity.x) && reader.f32(value.velocity.y) && reader.f32(value.velocity.z) &&
           reader.i16(value.yaw) && reader.u16(value.action) && finite(value.position) && finite(value.velocity);
}

bool encode_appearance(ByteWriter& writer,
                       const PlayerAppearance& appearance,
                       const CustomPattern& pattern) {
    if (appearance.gender > 2 || appearance.face >= 8 || pattern.palette >= 16 ||
        (pattern.present ? appearance.clothing_index < 0x100 || appearance.clothing_index >= 0x108
                         : appearance.clothing_index >= 0x100)) return false;
    if (!writer.bytes(appearance.name.data(), appearance.name.size()) || !writer.u8(appearance.gender) ||
        !writer.u8(appearance.face) || !writer.u16(appearance.clothing) ||
        !writer.u16(appearance.clothing_index) ||
        !writer.u32(appearance.revision) || !writer.u8(pattern.present ? 1 : 0) ||
        !writer.u8(pattern.palette)) return false;
    return !pattern.present || writer.bytes(pattern.texture.data(), pattern.texture.size());
}

bool decode_appearance(ByteReader& reader,
                       PlayerAppearance& appearance,
                       CustomPattern& pattern) {
    std::uint8_t present;
    pattern = {};
    if (!reader.bytes(appearance.name.data(), appearance.name.size()) || !reader.u8(appearance.gender) ||
        !reader.u8(appearance.face) || !reader.u16(appearance.clothing) ||
        !reader.u16(appearance.clothing_index) ||
        !reader.u32(appearance.revision) || !reader.u8(present) || !reader.u8(pattern.palette) ||
        present > 1 || appearance.gender > 2 || appearance.face >= 8 || pattern.palette >= 16) return false;
    pattern.present = present != 0;
    if ((pattern.present ? appearance.clothing_index < 0x100 || appearance.clothing_index >= 0x108
                         : appearance.clothing_index >= 0x100)) return false;
    return !pattern.present || reader.bytes(pattern.texture.data(), pattern.texture.size());
}

bool encode_presentation(ByteWriter& writer, const PlayerPresentation& presentation) {
    const PlayerAnimation& animation = presentation.animation;
    const std::uint8_t flags = static_cast<std::uint8_t>((animation.looping ? 1U : 0U) |
                                                         (animation.reversed ? 2U : 0U));
    const PlayerAppearanceBits& bits = presentation.appearance_bits;
    /* The three single-bit resource selectors share one byte with the two
     * animation flags rather than costing a byte each. */
    const std::uint8_t appearance = static_cast<std::uint8_t>((bits.bee_swell ? 1U : 0U) |
                                                              (bits.decoy ? 2U : 0U) |
                                                              (bits.change_color ? 4U : 0U));
    return valid(animation) && valid(bits) && writer.u8(animation.body) && writer.u8(animation.overlay) &&
           writer.u8(animation.part_table) && writer.u8(animation.item_state) && writer.u8(flags) &&
           writer.u16(presentation.equipped_item) && writer.u8(appearance) && writer.u8(bits.sunburn) &&
           writer.u8(bits.umbrella_state) && writer.u16(bits.carried_item);
}

bool decode_presentation(ByteReader& reader, PlayerPresentation& presentation) {
    PlayerAnimation& animation = presentation.animation;
    std::uint8_t flags = 0;
    PlayerAppearanceBits& bits = presentation.appearance_bits;
    std::uint8_t appearance = 0;
    if (!reader.u8(animation.body) || !reader.u8(animation.overlay) || !reader.u8(animation.part_table) ||
        !reader.u8(animation.item_state) || !reader.u8(flags) || (flags & 0xFCU) != 0 ||
        !reader.u16(presentation.equipped_item) || !reader.u8(appearance) || (appearance & 0xF8U) != 0 ||
        !reader.u8(bits.sunburn) || !reader.u8(bits.umbrella_state) || !reader.u16(bits.carried_item))
        return false;
    animation.looping = (flags & 1U) != 0;
    animation.reversed = (flags & 2U) != 0;
    bits.bee_swell = (appearance & 1U) != 0;
    bits.decoy = (appearance & 2U) != 0;
    bits.change_color = (appearance & 4U) != 0;
    /* Bounds-checked here so a viewer may index its face-palette and umbrella
     * tables with these directly, exactly as it already does with the
     * animation indices. */
    return valid(animation) && valid(bits);
}

/* A vacant slot carries no identity at all, so a peer cannot smuggle a name or
 * an account through one and have a reader pick it up by ignoring the flag. */
bool valid_resident(const ResidentIdentity& resident) {
    if (!resident.occupied) {
        return resident.account == 0 && resident.gender == 0 &&
               std::all_of(resident.name.begin(), resident.name.end(),
                           [](std::uint8_t byte) { return byte == 0; });
    }
    return resident.account != 0 && resident.gender <= 2;
}

bool encode_roster(ByteWriter& writer, const ResidentRoster& roster) {
    for (const ResidentIdentity& resident : roster.slots) {
        if (!valid_resident(resident) || !writer.u8(resident.occupied ? 1 : 0) ||
            !writer.u8(resident.gender) || !writer.u64(resident.account) ||
            !writer.bytes(resident.name.data(), resident.name.size())) return false;
    }
    return true;
}

bool decode_roster(ByteReader& reader, ResidentRoster& roster) {
    for (ResidentIdentity& resident : roster.slots) {
        std::uint8_t occupied;
        if (!reader.u8(occupied) || !reader.u8(resident.gender) || !reader.u64(resident.account) ||
            !reader.bytes(resident.name.data(), resident.name.size()) || occupied > 1) return false;
        resident.occupied = occupied != 0;
        if (!valid_resident(resident)) return false;
    }
    return true;
}

Revision next_revision(Revision revision) {
    return revision == std::numeric_limits<Revision>::max() ? 1 : revision + 1;
}

bool valid_gyroid(const GyroidState& state) {
    if (state.revision == 0) return false;
    for (const GyroidItem& entry : state.items) {
        if (entry.item == 0) {
            if (entry.exchange != 0 || entry.price != 0) return false;
        } else if (entry.exchange > kMaximumGyroidExchange ||
                   (entry.exchange == kGyroidExchangeSale ? entry.price == 0 : entry.price != 0)) {
            return false;
        }
    }
    return true;
}

bool encode_gyroid(ByteWriter& writer, const GyroidState& state) {
    if (!valid_gyroid(state) || !writer.u32(state.revision)) return false;
    for (const GyroidItem& entry : state.items) {
        if (!writer.u16(entry.item) || !writer.u8(entry.exchange) || !writer.u32(entry.price)) return false;
    }
    return writer.bytes(state.message.data(), state.message.size()) && writer.u32(state.bells);
}

bool decode_gyroid(ByteReader& reader, GyroidState& state) {
    if (!reader.u32(state.revision)) return false;
    for (GyroidItem& entry : state.items) {
        if (!reader.u16(entry.item) || !reader.u8(entry.exchange) || !reader.u32(entry.price)) return false;
    }
    return reader.bytes(state.message.data(), state.message.size()) && reader.u32(state.bells) &&
           valid_gyroid(state);
}

} // namespace

bool encode_player_delta(const PlayerPresentationDelta& delta, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (delta.account == 0 || delta.entity == 0 || !writer.u64(delta.account) || !writer.u64(delta.entity) ||
        !encode_presentation(writer, delta.presentation)) return false;
    output = writer.data();
    return true;
}

bool decode_player_delta(const std::vector<std::uint8_t>& input, PlayerPresentationDelta& delta) {
    ByteReader reader(input);
    return reader.u64(delta.account) && reader.u64(delta.entity) &&
           decode_presentation(reader, delta.presentation) && delta.account != 0 && delta.entity != 0 &&
           reader.finished();
}

static bool encode_turnips(ByteWriter& writer, const TurnipMarket& market) {
    if (market.trend >= kTurnipTrendCount || market.revision == 0) return false;
    for (std::uint16_t price : market.daily_price) {
        if (price > kTurnipPriceMaximum || !writer.u16(price)) return false;
    }
    return writer.u8(market.trend) && writer.u32(market.revision);
}

static bool decode_turnips(ByteReader& reader, TurnipMarket& market) {
    for (std::uint16_t& price : market.daily_price) {
        if (!reader.u16(price) || price > kTurnipPriceMaximum) return false;
    }
    return reader.u8(market.trend) && reader.u32(market.revision) && market.trend < kTurnipTrendCount &&
           market.revision != 0;
}

bool encode_gyroid_delta(const GyroidDelta& delta, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (delta.house_id == 0 || delta.original_slot >= kOriginalResidentSlots ||
        !writer.u64(delta.house_id) || !writer.u8(delta.original_slot) ||
        !encode_gyroid(writer, delta.state)) return false;
    output = writer.data();
    return true;
}

bool decode_gyroid_delta(const std::vector<std::uint8_t>& input, GyroidDelta& delta) {
    ByteReader reader(input);
    return reader.u64(delta.house_id) && reader.u8(delta.original_slot) && decode_gyroid(reader, delta.state) &&
           delta.house_id != 0 && delta.original_slot < kOriginalResidentSlots && reader.finished();
}

static bool encode_notices(ByteWriter& writer, const NoticeBoard& board) {
    if (board.revision == 0 || board.posts.size() > kNoticeBoardPosts) return false;
    if (!writer.u8(static_cast<std::uint8_t>(board.posts.size())) || !writer.u32(board.revision)) return false;
    for (const NoticePost& post : board.posts) {
        if (!writer.bytes(post.message.data(), post.message.size()) ||
            !writer.bytes(post.posted_time.data(), post.posted_time.size())) return false;
    }
    return true;
}

static bool decode_notices(ByteReader& reader, NoticeBoard& board) {
    std::uint8_t count = 0;
    if (!reader.u8(count) || !reader.u32(board.revision) || count > kNoticeBoardPosts ||
        board.revision == 0) return false;
    board.posts.clear();
    board.posts.resize(count);
    for (NoticePost& post : board.posts) {
        if (!reader.bytes(post.message.data(), post.message.size()) ||
            !reader.bytes(post.posted_time.data(), post.posted_time.size())) return false;
    }
    return true;
}

static bool encode_villagers(ByteWriter& writer, const VillagerRoster& roster) {
    if (roster.revision == 0) return false;
    if (roster.move_in.pending && roster.move_in.slot >= kVillagerSlots) return false;
    if (!writer.u32(roster.revision) || !writer.u8(roster.initialized ? 1 : 0) ||
        !writer.u8(roster.move_in.pending ? 1 : 0) || !writer.u8(roster.move_in.slot) ||
        !writer.u32(roster.move_in.seed) ||
        !writer.u64(static_cast<std::uint64_t>(roster.last_move_in_unix))) return false;
    for (const VillagerSlot& slot : roster.slots) {
        if (!valid_villager_slot(slot) || !writer.u8(slot.occupied ? 1 : 0)) return false;
        if (!slot.occupied) continue;
        const VillagerIdentity& v = slot.villager;
        if (!writer.u16(v.npc_id) || !writer.u16(v.land_id) ||
            !writer.bytes(v.land_name.data(), v.land_name.size()) || !writer.u8(v.name_id) ||
            !writer.u8(v.looks) || !writer.u8(v.home_block_x) || !writer.u8(v.home_block_z) ||
            !writer.u8(v.home_ut_x) || !writer.u8(v.home_ut_z) ||
            !writer.bytes(v.catchphrase.data(), v.catchphrase.size()) || !writer.u16(v.cloth) ||
            !writer.u16(v.present_cloth) || !writer.u8(v.cloth_original_id) || !writer.u8(v.umbrella_id) ||
            !writer.u8(v.mood) || !writer.u8(v.mood_time) || !writer.u8(v.is_home) || !writer.u8(v.moved_in) ||
            !writer.u8(v.removing) || !writer.u16(v.previous_land_id) ||
            !writer.bytes(v.previous_land_name.data(), v.previous_land_name.size()) ||
            !writer.bytes(v.parent_name.data(), v.parent_name.size()) ||
            !writer.bytes(v.relations.data(), v.relations.size())) return false;
    }
    return true;
}

static bool decode_villagers(ByteReader& reader, VillagerRoster& roster) {
    std::uint8_t initialized = 0;
    std::uint8_t pending = 0;
    std::uint64_t last_move_in = 0;
    if (!reader.u32(roster.revision) || !reader.u8(initialized) || initialized > 1 ||
        !reader.u8(pending) || pending > 1 || !reader.u8(roster.move_in.slot) ||
        !reader.u32(roster.move_in.seed) || !reader.u64(last_move_in) ||
        roster.revision == 0) return false;
    roster.initialized = initialized != 0;
    roster.move_in.pending = pending != 0;
    roster.last_move_in_unix = static_cast<std::int64_t>(last_move_in);
    /* A pending opening names a real slot; a viewer indexes the roster with it. */
    if (roster.move_in.pending && roster.move_in.slot >= kVillagerSlots) return false;
    for (VillagerSlot& slot : roster.slots) {
        std::uint8_t occupied = 0;
        slot = {};
        if (!reader.u8(occupied) || occupied > 1) return false;
        slot.occupied = occupied != 0;
        if (!slot.occupied) continue;
        VillagerIdentity& v = slot.villager;
        if (!reader.u16(v.npc_id) || !reader.u16(v.land_id) ||
            !reader.bytes(v.land_name.data(), v.land_name.size()) || !reader.u8(v.name_id) ||
            !reader.u8(v.looks) || !reader.u8(v.home_block_x) || !reader.u8(v.home_block_z) ||
            !reader.u8(v.home_ut_x) || !reader.u8(v.home_ut_z) ||
            !reader.bytes(v.catchphrase.data(), v.catchphrase.size()) || !reader.u16(v.cloth) ||
            !reader.u16(v.present_cloth) || !reader.u8(v.cloth_original_id) || !reader.u8(v.umbrella_id) ||
            !reader.u8(v.mood) || !reader.u8(v.mood_time) || !reader.u8(v.is_home) || !reader.u8(v.moved_in) ||
            !reader.u8(v.removing) || !reader.u16(v.previous_land_id) ||
            !reader.bytes(v.previous_land_name.data(), v.previous_land_name.size()) ||
            !reader.bytes(v.parent_name.data(), v.parent_name.size()) ||
            !reader.bytes(v.relations.data(), v.relations.size())) return false;
        /* Bounds-checked here so a viewer may index the game's personality and
         * character tables with these directly. */
        if (!valid_villager_slot(slot)) return false;
    }
    return true;
}

static bool encode_villager_memories(ByteWriter& writer, const VillagerMemories& memories) {
    if (memories.revision == 0 || !writer.u32(memories.revision)) return false;
    for (const VillagerMemory& memory : memories.slots) {
        if (!writer.u8(memory.present ? 1 : 0)) return false;
        if (memory.present && !writer.bytes(memory.data.data(), memory.data.size())) return false;
    }
    return true;
}

static bool decode_villager_memories(ByteReader& reader, VillagerMemories& memories) {
    if (!reader.u32(memories.revision) || memories.revision == 0) return false;
    for (VillagerMemory& memory : memories.slots) {
        std::uint8_t present = 0;
        memory = {};
        if (!reader.u8(present) || present > 1) return false;
        memory.present = present != 0;
        if (memory.present && !reader.bytes(memory.data.data(), memory.data.size())) return false;
    }
    return true;
}

static bool encode_special_event(ByteWriter& writer, const SpecialEvent& event) {
    if (event.revision == 0) return false;
    /* The game indexes special_events[] with this, so a kind it does not define
     * cannot be allowed to reach a viewer. */
    if (event.kind != kNoSpecialEvent && event.kind > kMaximumSpecialEventKind) return false;
    return writer.u32(event.kind) && writer.u32(event.revision) &&
           writer.bytes(event.scheduled.data(), event.scheduled.size()) &&
           writer.bytes(event.payload.data(), event.payload.size());
}

static bool decode_special_event(ByteReader& reader, SpecialEvent& event) {
    if (!reader.u32(event.kind) || !reader.u32(event.revision) ||
        !reader.bytes(event.scheduled.data(), event.scheduled.size()) ||
        !reader.bytes(event.payload.data(), event.payload.size()) || event.revision == 0) return false;
    return event.kind == kNoSpecialEvent || event.kind <= kMaximumSpecialEventKind;
}

bool encode_special_event_delta(const SpecialEvent& event, std::vector<std::uint8_t>& output) {
    /* kind, revision, the schedule time and the payload -- comfortably clear of
     * the payload size so widening it again cannot silently fail the encode,
     * which is how this last broke: the checkpoint writes through this same
     * codec, so a short writer took the whole server tick down with it. */
    ByteWriter writer(kSpecialEventPayloadBytes + 64);
    if (!encode_special_event(writer, event)) return false;
    output = writer.data();
    return true;
}

bool decode_special_event_delta(const std::vector<std::uint8_t>& input, SpecialEvent& event) {
    ByteReader reader(input);
    return decode_special_event(reader, event) && reader.finished();
}

bool encode_villager_memories_payload(const VillagerMemories& memories, std::vector<std::uint8_t>& output) {
    ByteWriter writer(kMaximumBaselineBytes);
    if (!encode_villager_memories(writer, memories)) return false;
    output = writer.data();
    return true;
}

bool decode_villager_memories_payload(const std::vector<std::uint8_t>& input, VillagerMemories& memories) {
    ByteReader reader(input);
    return decode_villager_memories(reader, memories) && reader.finished();
}

bool encode_villager_delta(const VillagerRoster& roster, std::vector<std::uint8_t>& output) {
    ByteWriter writer(kMaximumBaselineBytes);
    if (!encode_villagers(writer, roster)) return false;
    output = writer.data();
    return true;
}

bool decode_villager_delta(const std::vector<std::uint8_t>& input, VillagerRoster& roster) {
    ByteReader reader(input);
    return decode_villagers(reader, roster) && reader.finished();
}

bool encode_notice_delta(const NoticeBoard& board, std::vector<std::uint8_t>& output) {
    ByteWriter writer(kMaximumBaselineBytes);
    if (!encode_notices(writer, board)) return false;
    output = writer.data();
    return true;
}

bool decode_notice_delta(const std::vector<std::uint8_t>& input, NoticeBoard& board) {
    ByteReader reader(input);
    return decode_notices(reader, board) && reader.finished();
}

bool encode_town_tune_delta(const TownTune& tune, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (tune.revision == 0 || !writer.u64(tune.notes) || !writer.u32(tune.revision)) return false;
    output = writer.data();
    return true;
}

bool decode_town_tune_delta(const std::vector<std::uint8_t>& input, TownTune& tune) {
    ByteReader reader(input);
    return reader.u64(tune.notes) && reader.u32(tune.revision) && tune.revision != 0 && reader.finished();
}

bool encode_turnip_delta(const TurnipMarket& market, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!encode_turnips(writer, market)) return false;
    output = writer.data();
    return true;
}

bool decode_turnip_delta(const std::vector<std::uint8_t>& input, TurnipMarket& market) {
    ByteReader reader(input);
    return decode_turnips(reader, market) && reader.finished();
}

bool ResidentRoster::operator==(const ResidentRoster& other) const {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const ResidentIdentity& left = slots[i];
        const ResidentIdentity& right = other.slots[i];
        if (left.occupied != right.occupied || left.account != right.account ||
            left.gender != right.gender || left.name != right.name) return false;
    }
    return true;
}

bool encode_baseline(const ZoneBaseline& baseline, std::vector<std::uint8_t>& output) {
    if (baseline.zone == 0 || baseline.revision == 0 || baseline.tiles.size() > kMaximumBaselineTiles ||
        baseline.players.size() > kMaxPlayersPerZone || baseline.npcs.size() > kMaximumBaselineNpcs ||
        baseline.inventory.revision == 0 || baseline.ledger.revision == 0 || baseline.shop.revision == 0 ||
        baseline.mailbox.revision == 0 || baseline.mail.size() > kMailboxCapacity + kCarriedMailCapacity ||
        baseline.shop.stock.size() > kMaximumShopEntries || (baseline.house_light_mask & 0xF0U) != 0 ||
        baseline.town_capacity == 0 || baseline.town_population > baseline.town_capacity ||
        (baseline.has_house && baseline.house.zone != baseline.zone)) return false;
    ByteWriter writer(kMaximumBaselineBytes);
    if (!writer.u32(baseline.server_tick) || !writer.u32(baseline.revision) || !writer.u32(baseline.zone) ||
        !writer.u64(static_cast<std::uint64_t>(baseline.town_unix_seconds)) || !writer.u8(baseline.weather) ||
        !writer.u8(baseline.weather_intensity) || !writer.u8(baseline.town_population) ||
        !writer.u8(baseline.town_capacity) || !writer.u8(baseline.house_light_mask) ||
        !writer.u8(baseline.has_house ? 1 : 0) ||
        !writer.u32(static_cast<std::uint32_t>(baseline.tiles.size())) ||
        !writer.u16(static_cast<std::uint16_t>(baseline.players.size())) ||
        !writer.u16(static_cast<std::uint16_t>(baseline.npcs.size())) ||
        !writer.u32(baseline.inventory.revision) || !writer.u32(baseline.inventory.bells) ||
        !encode_roster(writer, baseline.residents)) return false;
    for (const ItemSlot& slot : baseline.inventory.slots) {
        if (!writer.u16(slot.item) || !writer.u8(slot.condition)) return false;
    }
    if (!writer.u16(baseline.inventory.equipped.item) || !writer.u8(baseline.inventory.equipped.condition))
        return false;
    if (!writer.u32(baseline.ledger.revision) || !writer.u64(baseline.ledger.bank_balance) ||
        !writer.u64(baseline.ledger.debt) || !writer.u32(baseline.mailbox.revision) ||
        !writer.u8(static_cast<std::uint8_t>(baseline.mail.size()))) return false;
    for (const MailRecord& letter : baseline.mail) {
        if (!encode_mail(writer, letter)) return false;
    }
    if (baseline.shop.tier > static_cast<std::uint8_t>(ShopTier::DepartmentStore)) return false;
    if (!writer.u32(baseline.shop.revision) || !writer.u16(baseline.shop.rare_item) ||
        !writer.u8(baseline.shop.tier) || !writer.u32(baseline.shop.sales_sum) ||
        !writer.u16(static_cast<std::uint16_t>(baseline.shop.stock.size()))) return false;
    for (const ShopEntry& entry : baseline.shop.stock) {
        if (!writer.u16(entry.item) || !writer.u32(entry.price) || !writer.u16(entry.quantity)) return false;
    }
    {
        std::vector<std::uint8_t> museum;
        if (!encode_museum_delta(baseline.museum, museum) ||
            !writer.u32(static_cast<std::uint32_t>(museum.size())) ||
            !writer.bytes(museum.data(), museum.size())) return false;
    }
    for (const auto& entry : baseline.gyroids) {
        if (entry.occupied && entry.house_id == 0) return false;
        if (!writer.u8(entry.occupied ? 1 : 0)) return false;
        if (entry.occupied && (!writer.u64(entry.house_id) || !encode_gyroid(writer, entry.state))) return false;
    }
    if (!encode_turnips(writer, baseline.turnips)) return false;
    if (baseline.town_tune.revision == 0) return false;
    if (!writer.u64(baseline.town_tune.notes) || !writer.u32(baseline.town_tune.revision)) return false;
    if (!encode_notices(writer, baseline.notices)) return false;
    if (!encode_villagers(writer, baseline.villagers)) return false;
    if (!writer.u64(baseline.npc_simulation_host)) return false;
    if (!encode_villager_memories(writer, baseline.villager_memories)) return false;
    if (!encode_special_event(writer, baseline.special_event)) return false;
    if (baseline.has_house && !encode_house(writer, baseline.house)) return false;
    for (const auto& entry : baseline.tiles) {
        if (!writer.i16(entry.first.x) || !writer.i16(entry.first.z) || !writer.u32(entry.second.revision) ||
            !writer.u16(entry.second.item) || !writer.u8(entry.second.condition) ||
            !writer.u8(static_cast<std::uint8_t>(entry.second.terrain)) ||
            !writer.u8(entry.second.buried ? 1 : 0) || !writer.u8(entry.second.placed_furniture ? 1 : 0)) return false;
    }
    for (const PlayerSnapshot& player : baseline.players) {
        if (!valid_transition(player)) return false;
        if (!writer.u64(player.entity) || !writer.u64(player.account) || !writer.u32(player.zone) ||
            !writer.u32(player.acknowledged_input) || !encode_transform(writer, player.transform) ||
            !encode_appearance(writer, player.appearance, player.pattern) ||
            !encode_presentation(writer, player.presentation) ||
            !writer.u8(static_cast<std::uint8_t>(player.transition_phase)) ||
            !writer.u32(player.transition_door) || !writer.u32(player.transition_expires_tick)) return false;
    }
    for (const NpcState& npc : baseline.npcs) {
        if (!writer.u64(npc.entity) || !writer.u32(npc.zone) || !writer.u32(npc.revision) ||
            !writer.u16(npc.schedule_state) || !writer.u16(npc.animation) || !writer.u16(npc.emotion) ||
            !writer.u64(npc.destination) || !encode_transform(writer, npc.transform) ||
        !writer.u64(npc.conversation_owner)) return false;
    }
    output = writer.data();
    return true;
}

bool decode_baseline(const std::vector<std::uint8_t>& input, ZoneBaseline& baseline) {
    if (input.size() > kMaximumBaselineBytes) return false;
    ByteReader reader(input);
    std::uint64_t town_time;
    std::uint32_t tile_count;
    std::uint16_t player_count;
    std::uint16_t npc_count;
    std::uint16_t shop_count;
    std::uint8_t has_house;
    if (!reader.u32(baseline.server_tick) || !reader.u32(baseline.revision) || !reader.u32(baseline.zone) ||
        !reader.u64(town_time) || !reader.u8(baseline.weather) || !reader.u8(baseline.weather_intensity) ||
        !reader.u8(baseline.town_population) || !reader.u8(baseline.town_capacity) ||
        baseline.town_capacity == 0 || baseline.town_population > baseline.town_capacity ||
        !reader.u8(baseline.house_light_mask) || !reader.u8(has_house) || has_house > 1 ||
        (baseline.house_light_mask & 0xF0U) != 0 ||
        !reader.u32(tile_count) || !reader.u16(player_count) || !reader.u16(npc_count) || baseline.zone == 0 ||
        baseline.revision == 0 || tile_count > kMaximumBaselineTiles || player_count > kMaxPlayersPerZone ||
        npc_count > kMaximumBaselineNpcs || town_time > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return false;
    if (!reader.u32(baseline.inventory.revision) || !reader.u32(baseline.inventory.bells) ||
        baseline.inventory.revision == 0 || !decode_roster(reader, baseline.residents)) return false;
    for (ItemSlot& slot : baseline.inventory.slots) {
        if (!reader.u16(slot.item) || !reader.u8(slot.condition)) return false;
    }
    if (!reader.u16(baseline.inventory.equipped.item) || !reader.u8(baseline.inventory.equipped.condition))
        return false;
    std::uint8_t mail_count;
    if (!reader.u32(baseline.ledger.revision) || !reader.u64(baseline.ledger.bank_balance) ||
        !reader.u64(baseline.ledger.debt) || !reader.u32(baseline.mailbox.revision) ||
        !reader.u8(mail_count) || baseline.ledger.revision == 0 || baseline.mailbox.revision == 0 ||
        mail_count > kMailboxCapacity + kCarriedMailCapacity) return false;
    baseline.mail.clear();
    baseline.mailbox.mail.clear();
    baseline.mailbox.carried.clear();
    baseline.mail.reserve(mail_count);
    for (std::uint8_t i = 0; i < mail_count; ++i) {
        MailRecord letter;
        if (!decode_mail(reader, letter)) return false;
        if (letter.location == MailLocation::Carried) baseline.mailbox.carried.push_back(letter.id);
        else baseline.mailbox.mail.push_back(letter.id);
        baseline.mail.push_back(letter);
    }
    if (baseline.mailbox.mail.size() > kMailboxCapacity ||
        baseline.mailbox.carried.size() > kCarriedMailCapacity) return false;
    if (!reader.u32(baseline.shop.revision) || !reader.u16(baseline.shop.rare_item) ||
        !reader.u8(baseline.shop.tier) || !reader.u32(baseline.shop.sales_sum) || !reader.u16(shop_count) ||
        baseline.shop.revision == 0 ||
        baseline.shop.tier > static_cast<std::uint8_t>(ShopTier::DepartmentStore) ||
        shop_count > kMaximumShopEntries) return false;
    baseline.shop.stock.clear();
    baseline.shop.stock.resize(shop_count);
    for (ShopEntry& entry : baseline.shop.stock) {
        if (!reader.u16(entry.item) || !reader.u32(entry.price) || !reader.u16(entry.quantity)) return false;
    }
    {
        std::uint32_t museum_bytes;
        if (!reader.u32(museum_bytes) || museum_bytes > kMaximumBaselineBytes) return false;
        std::vector<std::uint8_t> museum(museum_bytes);
        if (!reader.bytes(museum.data(), museum.size()) || !decode_museum_delta(museum, baseline.museum))
            return false;
    }
    for (auto& entry : baseline.gyroids) {
        std::uint8_t occupied;
        entry = {};
        if (!reader.u8(occupied) || occupied > 1) return false;
        entry.occupied = occupied != 0;
        if (entry.occupied && (!reader.u64(entry.house_id) || entry.house_id == 0 ||
                               !decode_gyroid(reader, entry.state))) return false;
    }
    if (!decode_turnips(reader, baseline.turnips)) return false;
    if (!reader.u64(baseline.town_tune.notes) || !reader.u32(baseline.town_tune.revision) ||
        baseline.town_tune.revision == 0) return false;
    if (!decode_notices(reader, baseline.notices)) return false;
    if (!decode_villagers(reader, baseline.villagers)) return false;
    if (!reader.u64(baseline.npc_simulation_host)) return false;
    if (!decode_villager_memories(reader, baseline.villager_memories)) return false;
    if (!decode_special_event(reader, baseline.special_event)) return false;
    baseline.has_house = has_house != 0;
    baseline.house = {};
    if (baseline.has_house && (!decode_house(reader, baseline.house) || baseline.house.zone != baseline.zone)) return false;
    baseline.town_unix_seconds = static_cast<std::int64_t>(town_time);
    baseline.tiles.clear();
    baseline.players.clear();
    baseline.npcs.clear();
    baseline.tiles.reserve(tile_count);
    for (std::uint32_t i = 0; i < tile_count; ++i) {
        TileAddress address;
        address.zone = baseline.zone;
        TileState tile;
        std::uint8_t terrain;
        std::uint8_t buried;
        std::uint8_t placed;
        if (!reader.i16(address.x) || !reader.i16(address.z) || !reader.u32(tile.revision) ||
            !reader.u16(tile.item) || !reader.u8(tile.condition) || !reader.u8(terrain) ||
            !reader.u8(buried) || !reader.u8(placed) || terrain > static_cast<std::uint8_t>(TerrainState::Planted) ||
            buried > 1 || placed > 1 || tile.revision == 0) return false;
        tile.terrain = static_cast<TerrainState>(terrain);
        tile.buried = buried != 0;
        tile.placed_furniture = placed != 0;
        baseline.tiles.emplace_back(address, tile);
    }
    baseline.players.reserve(player_count);
    for (std::uint16_t i = 0; i < player_count; ++i) {
        PlayerSnapshot player;
        std::uint8_t transition_phase;
        if (!reader.u64(player.entity) || !reader.u64(player.account) || !reader.u32(player.zone) ||
            !reader.u32(player.acknowledged_input) || !decode_transform(reader, player.transform) ||
            !decode_appearance(reader, player.appearance, player.pattern) ||
            !decode_presentation(reader, player.presentation) || !reader.u8(transition_phase) ||
            !reader.u32(player.transition_door) || !reader.u32(player.transition_expires_tick) ||
            transition_phase > static_cast<std::uint8_t>(DoorTransitionPhase::Arriving) ||
            player.entity == 0 || player.account == 0 || player.zone != baseline.zone) return false;
        player.transition_phase = static_cast<DoorTransitionPhase>(transition_phase);
        if (!valid_transition(player)) return false;
        baseline.players.push_back(player);
    }
    baseline.npcs.reserve(npc_count);
    for (std::uint16_t i = 0; i < npc_count; ++i) {
        NpcState npc;
        if (!reader.u64(npc.entity) || !reader.u32(npc.zone) || !reader.u32(npc.revision) ||
            !reader.u16(npc.schedule_state) || !reader.u16(npc.animation) || !reader.u16(npc.emotion) ||
            !reader.u64(npc.destination) || !decode_transform(reader, npc.transform) ||
        !reader.u64(npc.conversation_owner) || npc.entity == 0 ||
            npc.zone != baseline.zone || npc.revision == 0) return false;
        baseline.npcs.push_back(npc);
    }
    return reader.finished();
}

TileChangeCause tile_change_cause(WorldOpType type) {
    switch (type) {
        case WorldOpType::DropItem: return TileChangeCause::Drop;
        case WorldOpType::PickupItem: return TileChangeCause::Pickup;
        case WorldOpType::Dig: return TileChangeCause::Dig;
        case WorldOpType::Bury: return TileChangeCause::Bury;
        case WorldOpType::Plant: return TileChangeCause::Plant;
        case WorldOpType::ChopTree: return TileChangeCause::ChopTree;
        case WorldOpType::PlaceFurniture: return TileChangeCause::PlaceFurniture;
        case WorldOpType::RemoveFurniture: return TileChangeCause::RemoveFurniture;
        case WorldOpType::FillHole: return TileChangeCause::FillHole;
    }
    return TileChangeCause::Server;
}

bool encode_shop_delta(const ShopState& shop, std::vector<std::uint8_t>& output) {
    if (shop.revision == 0 || shop.stock.size() > kMaximumShopEntries) return false;
    ByteWriter writer(8 + kMaximumShopEntries * 8);
    if (shop.tier > static_cast<std::uint8_t>(ShopTier::DepartmentStore)) return false;
    if (!writer.u32(shop.revision) || !writer.u16(shop.rare_item) || !writer.u8(shop.tier) ||
        !writer.u32(shop.sales_sum) ||
        !writer.u16(static_cast<std::uint16_t>(shop.stock.size()))) return false;
    for (const ShopEntry& entry : shop.stock) {
        if (!writer.u16(entry.item) || !writer.u32(entry.price) || !writer.u16(entry.quantity)) return false;
    }
    output = writer.data();
    return true;
}

bool decode_shop_delta(const std::vector<std::uint8_t>& input, ShopState& shop) {
    ByteReader reader(input);
    std::uint16_t count;
    if (!reader.u32(shop.revision) || !reader.u16(shop.rare_item) || !reader.u8(shop.tier) ||
        !reader.u32(shop.sales_sum) || !reader.u16(count) || shop.revision == 0 ||
        shop.tier > static_cast<std::uint8_t>(ShopTier::DepartmentStore) ||
        count > kMaximumShopEntries) return false;
    shop.stock.clear();
    shop.stock.resize(count);
    for (ShopEntry& entry : shop.stock) {
        if (!reader.u16(entry.item) || !reader.u32(entry.price) || !reader.u16(entry.quantity)) return false;
    }
    return reader.finished();
}

/* A whole museum is bounded by the number of donatable species, which is far
 * below the item space; this is a sanity bound, not a design limit. */
constexpr std::size_t kMaximumMuseumItems = 4096;

bool encode_museum_delta(const MuseumState& museum, std::vector<std::uint8_t>& output) {
    if (museum.revision == 0 || museum.donated_items.size() > kMaximumMuseumItems) return false;
    std::vector<std::uint16_t> sorted(museum.donated_items.begin(), museum.donated_items.end());
    std::sort(sorted.begin(), sorted.end());
    ByteWriter writer(8 + sorted.size() * 2);
    if (!writer.u32(museum.revision) || !writer.u16(static_cast<std::uint16_t>(sorted.size()))) return false;
    for (std::uint16_t item : sorted) {
        if (item == 0 || !writer.u16(item)) return false;
    }
    output = writer.data();
    return true;
}

bool decode_museum_delta(const std::vector<std::uint8_t>& input, MuseumState& museum) {
    ByteReader reader(input);
    std::uint16_t count;
    if (!reader.u32(museum.revision) || !reader.u16(count) || museum.revision == 0 ||
        count > kMaximumMuseumItems) return false;
    museum.donated_items.clear();
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint16_t item;
        if (!reader.u16(item) || item == 0 || !museum.donated_items.insert(item).second) return false;
    }
    return reader.finished();
}

bool encode_npc_delta(const NpcState& npc, std::vector<std::uint8_t>& output) {
    if (npc.entity == 0 || npc.zone == 0 || npc.revision == 0 || !finite(npc.transform.position) ||
        !finite(npc.transform.velocity)) return false;
    /* 66 bytes as encoded below; the slack absorbs the next field without a
     * silent truncation of the one after it. */
    ByteWriter writer(96);
    if (!writer.u64(npc.entity) || !writer.u32(npc.zone) || !writer.u32(npc.revision) ||
        !writer.u16(npc.schedule_state) || !writer.u16(npc.animation) || !writer.u16(npc.emotion) ||
        !writer.u64(npc.destination) || !encode_transform(writer, npc.transform) ||
        !writer.u64(npc.conversation_owner)) return false;
    output = writer.data();
    return true;
}

bool decode_npc_delta(const std::vector<std::uint8_t>& input, NpcState& npc) {
    ByteReader reader(input);
    if (!reader.u64(npc.entity) || !reader.u32(npc.zone) || !reader.u32(npc.revision) ||
        !reader.u16(npc.schedule_state) || !reader.u16(npc.animation) || !reader.u16(npc.emotion) ||
        !reader.u64(npc.destination) || !decode_transform(reader, npc.transform) ||
        !reader.u64(npc.conversation_owner) || npc.entity == 0 ||
        npc.zone == 0 || npc.revision == 0) return false;
    return reader.finished();
}

bool encode_tile_delta(const TileStateDelta& delta, std::vector<std::uint8_t>& output) {
    if (delta.address.zone == 0 || delta.state.revision == 0 ||
        static_cast<std::uint8_t>(delta.state.terrain) > static_cast<std::uint8_t>(TerrainState::Planted) ||
        static_cast<std::uint8_t>(delta.cause) > static_cast<std::uint8_t>(TileChangeCause::FillHole)) return false;
    ByteWriter writer(48);
    if (!writer.u32(delta.address.zone) || !writer.i16(delta.address.x) || !writer.i16(delta.address.z) ||
        !writer.u32(delta.state.revision) || !writer.u16(delta.state.item) || !writer.u8(delta.state.condition) ||
        !writer.u8(static_cast<std::uint8_t>(delta.state.terrain)) ||
        !writer.u8(delta.state.buried ? 1 : 0) || !writer.u8(delta.state.placed_furniture ? 1 : 0) ||
        !writer.u64(delta.actor) || !writer.u8(static_cast<std::uint8_t>(delta.cause))) return false;
    output = writer.data();
    return true;
}

bool decode_tile_delta(const std::vector<std::uint8_t>& input, TileStateDelta& delta) {
    ByteReader reader(input);
    std::uint8_t terrain;
    std::uint8_t buried;
    std::uint8_t placed;
    std::uint8_t cause;
    if (!reader.u32(delta.address.zone) || !reader.i16(delta.address.x) || !reader.i16(delta.address.z) ||
        !reader.u32(delta.state.revision) || !reader.u16(delta.state.item) ||
        !reader.u8(delta.state.condition) || !reader.u8(terrain) || !reader.u8(buried) ||
        !reader.u8(placed) || !reader.u64(delta.actor) || !reader.u8(cause) ||
        !reader.finished() || delta.address.zone == 0 || delta.state.revision == 0 ||
        terrain > static_cast<std::uint8_t>(TerrainState::Planted) || buried > 1 || placed > 1 ||
        cause > static_cast<std::uint8_t>(TileChangeCause::FillHole)) return false;
    delta.state.terrain = static_cast<TerrainState>(terrain);
    delta.state.buried = buried != 0;
    delta.state.placed_furniture = placed != 0;
    delta.cause = static_cast<TileChangeCause>(cause);
    return true;
}

bool encode_town_delta(const TownOccupancy& occupancy, std::vector<std::uint8_t>& output) {
    if (occupancy.capacity == 0 || occupancy.population > occupancy.capacity) return false;
    ByteWriter writer(2);
    if (!writer.u8(occupancy.population) || !writer.u8(occupancy.capacity)) return false;
    output = writer.data();
    return true;
}

bool decode_town_delta(const std::vector<std::uint8_t>& input, TownOccupancy& occupancy) {
    ByteReader reader(input);
    if (!reader.u8(occupancy.population) || !reader.u8(occupancy.capacity)) return false;
    return occupancy.capacity != 0 && occupancy.population <= occupancy.capacity;
}

bool encode_mail_delta(const MailDelta& delta, std::vector<std::uint8_t>& output) {
    if (delta.account == 0 || delta.mailbox_revision == 0 || delta.record.recipient != delta.account) return false;
    ByteWriter writer(128 + kMailNameBytes + kMailHeaderBytes + kMailBodyBytes + kMailFooterBytes);
    if (!writer.u64(delta.account) || !writer.u32(delta.mailbox_revision) ||
        !writer.u8(delta.removed ? 1 : 0) || !encode_mail(writer, delta.record)) return false;
    output = writer.data();
    return true;
}

bool decode_mail_delta(const std::vector<std::uint8_t>& input, MailDelta& delta) {
    ByteReader reader(input);
    std::uint8_t removed;
    if (!reader.u64(delta.account) || !reader.u32(delta.mailbox_revision) || !reader.u8(removed) ||
        removed > 1 || !decode_mail(reader, delta.record) || !reader.finished() || delta.account == 0 ||
        delta.mailbox_revision == 0 || delta.record.recipient != delta.account) return false;
    delta.removed = removed != 0;
    return true;
}

bool encode_resident_delta(const ResidentRoster& roster, std::vector<std::uint8_t>& output) {
    ByteWriter writer(kOriginalResidentSlots * 18U);
    if (!encode_roster(writer, roster)) return false;
    output = writer.data();
    return true;
}

bool decode_resident_delta(const std::vector<std::uint8_t>& input, ResidentRoster& roster) {
    ByteReader reader(input);
    return decode_roster(reader, roster) && reader.finished();
}

DeltaLog::DeltaLog(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {}

Revision DeltaLog::append(ReplicationDelta delta) {
    revision_ = next_revision(revision_);
    delta.revision = revision_;
    deltas_.push_back(std::move(delta));
    while (deltas_.size() > capacity_) deltas_.pop_front();
    return revision_;
}

bool DeltaLog::relevant(const ReplicationDelta& delta, const InterestContext& interest) {
    if (delta.target_account != 0 && delta.target_account != interest.account) return false;
    if (delta.kind == ResourceKind::Clock || delta.kind == ResourceKind::Weather ||
        delta.kind == ResourceKind::Town ||
        delta.kind == ResourceKind::Resident || delta.kind == ResourceKind::Shop ||
        delta.kind == ResourceKind::Museum ||
        delta.kind == ResourceKind::Gyroid ||
        delta.kind == ResourceKind::Turnip ||
        delta.kind == ResourceKind::TownTune ||
        delta.kind == ResourceKind::Notice ||
        delta.kind == ResourceKind::Villager ||
        delta.kind == ResourceKind::Event) return true; /* town-wide: not zone or distance scoped */
    if (delta.zone != 0 && delta.zone != interest.zone) return false;
    if (!interest.exterior || !delta.has_position || delta.reliable) return true;
    const float dx = delta.position.x - interest.position.x;
    const float dy = delta.position.y - interest.position.y;
    const float dz = delta.position.z - interest.position.z;
    return dx * dx + dy * dy + dz * dz <= interest.radius * interest.radius;
}

DeltaQueryResult DeltaLog::since(Revision after, const InterestContext& interest, std::size_t maximum) const {
    DeltaQueryResult result;
    result.newest_revision = revision_;
    if (maximum == 0) return result;
    if (!deltas_.empty() && after != 0 && after < deltas_.front().revision - 1) {
        result.requires_baseline = true;
        return result;
    }
    for (const ReplicationDelta& delta : deltas_) {
        if (delta.revision > after && relevant(delta, interest)) {
            result.deltas.push_back(delta);
            if (result.deltas.size() >= maximum) break;
        }
    }
    return result;
}

ZoneBaseline build_baseline(ZoneId zone,
                            Tick tick,
                            Revision revision,
                            std::int64_t town_unix_seconds,
                            std::uint8_t weather,
                            std::uint8_t weather_intensity,
                            const WorldAuthority& world,
                            const PlayerDirectory& players,
                            const NpcAuthority& npcs) {
    ZoneBaseline result;
    result.server_tick = tick;
    result.revision = revision;
    result.zone = zone;
    result.town_unix_seconds = town_unix_seconds;
    result.weather = weather;
    result.weather_intensity = weather_intensity;
    result.tiles = world.tiles_in_zone(zone);
    for (const PlayerView* player : players.query_zone(zone, kMaxPlayersPerZone)) {
        PlayerSnapshot snapshot;
        snapshot.entity = player->entity;
        snapshot.account = player->account;
        snapshot.zone = player->zone;
        snapshot.transform = player->transform;
        snapshot.appearance = player->appearance;
        snapshot.presentation = player->presentation;
        snapshot.pattern = player->pattern;
        result.players.push_back(snapshot);
    }
    result.npcs = npcs.zone_snapshot(zone);
    return result;
}

} // namespace acnet
