#include "acnet/economy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace acnet {
namespace {

std::size_t hash_mix(std::size_t seed, std::uint64_t value) {
    return seed ^ static_cast<std::size_t>(value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
}

/* Every operation whose auxiliary revision is the account's mail revision. */
bool mail_operation(EconomyOpType type) {
    return type == EconomyOpType::AttachMail || type == EconomyOpType::ClaimMail ||
           type == EconomyOpType::TakeMail || type == EconomyOpType::DiscardMail;
}

} // namespace

std::size_t EconomyAuthority::OperationKeyHash::operator()(const OperationKey& value) const {
    return hash_mix(hash_mix(hash_mix(0, value.account), value.idempotency.high), value.idempotency.low);
}

EconomyAuthority::EconomyAuthority(WorldAuthority* world, PlayerDirectory* players, EconomyConfig config)
    : world_(world), players_(players), config_(config) {
    if (config_.maximum_trade_distance <= 0.0F) config_.maximum_trade_distance = 120.0F;
}

bool EconomyAuthority::register_account(AccountId account, const AccountLedger& ledger_value) {
    if (account == 0 || ledger_value.revision == 0) return false;
    if (!ledgers_.emplace(account, ledger_value).second) return false;
    mailbox_for(account);
    return true;
}

bool EconomyAuthority::set_account(AccountId account, const AccountLedger& ledger_value) {
    if (account == 0 || ledger_value.revision == 0) return false;
    ledgers_[account] = ledger_value;
    mailbox_for(account);
    return true;
}

void EconomyAuthority::set_sell_price(std::uint16_t item, std::uint32_t price) {
    if (item != 0) sell_prices_[item] = price;
}

const AccountLedger* EconomyAuthority::ledger(AccountId account) const {
    const auto found = ledgers_.find(account);
    return found == ledgers_.end() ? nullptr : &found->second;
}

const MailRecord* EconomyAuthority::mail(std::uint64_t mail_id) const {
    const auto found = mail_.find(mail_id);
    return found == mail_.end() ? nullptr : &found->second;
}

const MailboxState* EconomyAuthority::mailbox(AccountId account) const {
    const auto found = mailboxes_.find(account);
    return found == mailboxes_.end() ? nullptr : &found->second;
}

std::vector<MailRecord> EconomyAuthority::mail_for(AccountId account) const {
    std::vector<MailRecord> letters;
    const MailboxState* box = mailbox(account);
    if (box == nullptr) return letters;
    letters.reserve(box->mail.size() + box->carried.size());
    /* Mailbox first, then carried: both halves in their authoritative order. */
    for (const std::vector<std::uint64_t>* list : {&box->mail, &box->carried}) {
        for (std::uint64_t id : *list) {
            const auto found = mail_.find(id);
            if (found != mail_.end()) letters.push_back(found->second);
        }
    }
    return letters;
}

MailboxState& EconomyAuthority::mailbox_for(AccountId account) {
    return mailboxes_[account];
}

bool EconomyAuthority::restore_mail(const MailRecord& record) {
    /* A sender of kAdministratorAccount is the operator, not a corrupt record. */
    if (record.id == 0 || record.recipient == 0 || record.revision == 0) return false;
    if (mail_.find(record.id) != mail_.end()) return false;
    MailboxState& box = mailbox_for(record.recipient);
    const bool carried = record.location == MailLocation::Carried;
    std::vector<std::uint64_t>& list = carried ? box.carried : box.mail;
    if (list.size() >= (carried ? kCarriedMailCapacity : kMailboxCapacity)) return false;
    mail_[record.id] = record;
    /* Replay restores letters in identifier order, so appending preserves
     * delivery order without persisting the lists themselves. */
    list.push_back(record.id);
    std::sort(list.begin(), list.end());
    if (record.id >= next_mail_id_) next_mail_id_ = record.id + 1;
    return next_mail_id_ != 0;
}

void EconomyAuthority::clear_mail() {
    mail_.clear();
    for (auto& entry : mailboxes_) {
        entry.second.mail.clear();
        entry.second.carried.clear();
    }
    /* next_mail_id_ deliberately keeps climbing: an identifier is never reused
     * within a process lifetime, even across a re-decode. */
}

bool EconomyAuthority::restore_mailbox_revision(AccountId account, Revision revision) {
    if (account == 0 || revision == 0) return false;
    mailbox_for(account).revision = revision;
    return true;
}

std::uint64_t EconomyAuthority::deliver_mail(AccountId sender,
                                             AccountId recipient,
                                             std::uint16_t attachment,
                                             const MailContent& content) {
    MailboxState& box = mailbox_for(recipient);
    MailRecord record;
    record.id = next_mail_id_;
    record.sender = sender;
    record.recipient = recipient;
    record.attachment = attachment;
    record.revision = 1;
    record.location = MailLocation::Mailbox;
    record.content = content;
    mail_[record.id] = record;
    box.mail.push_back(record.id);
    box.revision = next_revision(box.revision);
    ++next_mail_id_;
    return record.id;
}

Revision EconomyAuthority::next_revision(Revision revision) {
    return revision == std::numeric_limits<Revision>::max() ? 1 : revision + 1;
}

std::optional<std::uint8_t> EconomyAuthority::empty_slot(const InventoryState& inventory) {
    for (std::size_t i = 0; i < inventory.slots.size(); ++i) {
        if (inventory.slots[i].item == 0) return static_cast<std::uint8_t>(i);
    }
    return std::nullopt;
}

EconomyResult EconomyAuthority::reject(const EconomyRequest& request, ResultCode code) const {
    EconomyResult result;
    result.code = code;
    result.type = request.type;
    result.idempotency = request.idempotency;
    result.inventory_slot = request.inventory_slot;
    if (world_ != nullptr) {
        const InventoryState* inventory = world_->inventory(request.account);
        if (inventory != nullptr) {
            result.inventory_revision = inventory->revision;
            result.bells = inventory->bells;
        }
    }
    const AccountLedger* account = ledger(request.account);
    if (account != nullptr) {
        result.auxiliary_revision = account->revision;
        result.balance = account->bank_balance;
        result.debt = account->debt;
    }
    /* A rejected mail operation still reports the mailbox the client observed
     * against, so a stale claim can be retried without a fresh baseline. */
    if (mail_operation(request.type)) {
        const MailboxState* box = mailbox(request.account);
        result.auxiliary_revision = box == nullptr ? 0 : box->revision;
        result.mail_id = request.mail_id;
    }
    return result;
}

EconomyResult EconomyAuthority::apply(const EconomyRequest& request) {
    const OperationKey key{request.account, request.idempotency};
    /* The decoder already refuses these on the wire; refusing them here as well
     * keeps a locally constructed request from reaching operator authority. */
    if (static_cast<std::uint8_t>(request.type) > kMaximumClientEconomyOp) {
        return reject(request, ResultCode::Unauthorized);
    }
    if (!request.idempotency.valid() || request.account == 0 || world_ == nullptr) {
        return reject(request, ResultCode::Malformed);
    }
    const auto previous = idempotency_.find(key);
    if (previous != idempotency_.end()) {
        EconomyResult replay = previous->second;
        replay.replayed = true;
        return replay;
    }
    if (!validate_context(request)) {
        const EconomyResult result = reject(request, ResultCode::OutOfRange);
        idempotency_[key] = result;
        return result;
    }
    const InventoryState* current_inventory = world_->inventory(request.account);
    const auto ledger_it = ledgers_.find(request.account);
    if (current_inventory == nullptr || ledger_it == ledgers_.end()) {
        const EconomyResult result = reject(request, ResultCode::NotFound);
        idempotency_[key] = result;
        return result;
    }
    if (request.expected_inventory_revision != current_inventory->revision) {
        const EconomyResult result = reject(request, ResultCode::StaleRevision);
        idempotency_[key] = result;
        return result;
    }

    InventoryState inventory = *current_inventory;
    AccountLedger account = ledger_it->second;
    ShopState shop = shop_;
    MuseumState museum = museum_;
    EconomyResult result;
    result.code = ResultCode::Ok;
    result.type = request.type;
    result.idempotency = request.idempotency;
    result.inventory_slot = request.inventory_slot;
    bool inventory_changed = false;
    bool ledger_changed = false;
    bool shop_changed = false;
    bool museum_changed = false;
    /* Mail is committed after the switch: a delivery names the recipient whose
     * mailbox grows, a claim names the letter that leaves this account's. */
    AccountId mail_recipient = 0;
    std::uint16_t mail_attachment = 0;
    MailContent mail_content;
    std::uint64_t taken_mail = 0;
    std::uint64_t claimed_mail = 0;
    std::uint64_t discarded_mail = 0;

    switch (request.type) {
        case EconomyOpType::Buy: {
            if (request.expected_aux_revision != shop.revision || request.shop_index >= shop.stock.size()) {
                result.code = request.shop_index >= shop.stock.size() ? ResultCode::NotFound : ResultCode::StaleRevision;
                break;
            }
            ShopEntry& entry = shop.stock[request.shop_index];
            const auto slot = empty_slot(inventory);
            if (entry.item == 0 || entry.quantity == 0 || inventory.bells < entry.price || !slot.has_value()) {
                result.code = ResultCode::InvalidState;
                break;
            }
            inventory.bells -= entry.price;
            inventory.slots[*slot].item = entry.item;
            --entry.quantity;
            result.item = entry.item;
            result.inventory_slot = *slot;
            result.transaction_value = entry.price;
            inventory_changed = true;
            shop_changed = true;
            break;
        }
        case EconomyOpType::Sell: {
            if (request.inventory_slot >= inventory.slots.size()) {
                result.code = ResultCode::Malformed;
                break;
            }
            /* A mask sells the whole selection at once; without one this is the
             * single slot named directly. Either way the sale is atomic: if any
             * chosen pocket is empty or unsellable, nothing moves. */
            std::uint16_t mask = request.slot_mask;
            if (mask == 0) mask = static_cast<std::uint16_t>(1U << request.inventory_slot);
            if ((mask >> inventory.slots.size()) != 0) {
                result.code = ResultCode::Malformed;
                break;
            }
            const auto price_of = [this](std::uint16_t item) -> std::uint32_t {
                const auto override_price = sell_prices_.find(item);
                if (override_price != sell_prices_.end()) return override_price->second;
                return sell_price_resolver_ ? sell_price_resolver_(item) : 0;
            };
            std::uint32_t total = 0;
            bool sellable = true;
            for (std::size_t i = 0; i < inventory.slots.size() && sellable; ++i) {
                if ((mask & (1U << i)) == 0) continue;
                const std::uint16_t item = inventory.slots[i].item;
                const std::uint32_t price = price_of(item);
                sellable = item != 0 && price != 0 &&
                           (request.expected_item == 0 || item == request.expected_item) &&
                           price <= std::numeric_limits<std::uint32_t>::max() - inventory.bells - total;
                total += price;
                /* The last item cleared is what the result reports, matching the
                 * single-slot case where there is only one. */
                if (sellable) result.item = item;
            }
            if (!sellable) {
                result.code = ResultCode::InvalidState;
                result.item = 0;
                break;
            }
            for (std::size_t i = 0; i < inventory.slots.size(); ++i) {
                if ((mask & (1U << i)) != 0) inventory.slots[i] = {};
            }
            result.transaction_value = total;
            inventory.bells += total;
            /* The wallet cannot hold the whole sale, so the overflow comes back
             * as money bags -- starting in the slot the sold item just vacated,
             * which is what guarantees there is somewhere to put the first one.
             * Bells that will not fit in any pocket stay in the wallet above the
             * cap rather than being destroyed. */
            if (config_.wallet_maximum != 0 && config_.wallet_overflow_chunk != 0) {
                while (inventory.bells >= config_.wallet_maximum) {
                    const auto bag = empty_slot(inventory);
                    if (!bag.has_value()) break;
                    inventory.bells -= config_.wallet_overflow_chunk;
                    inventory.slots[*bag].item = config_.wallet_overflow_item;
                }
            }
            inventory_changed = true;
            break;
        }
        case EconomyOpType::Deposit: {
            if (request.expected_aux_revision != account.revision || request.amount == 0 || request.amount > inventory.bells ||
                request.amount > std::numeric_limits<std::uint64_t>::max() - account.bank_balance) {
                result.code = request.expected_aux_revision != account.revision ? ResultCode::StaleRevision
                                                                                : ResultCode::InvalidState;
                break;
            }
            inventory.bells -= static_cast<std::uint32_t>(request.amount);
            account.bank_balance += request.amount;
            inventory_changed = true;
            ledger_changed = true;
            break;
        }
        case EconomyOpType::Withdraw: {
            if (request.expected_aux_revision != account.revision || request.amount == 0 ||
                request.amount > account.bank_balance ||
                request.amount > std::numeric_limits<std::uint32_t>::max() - inventory.bells) {
                result.code = request.expected_aux_revision != account.revision ? ResultCode::StaleRevision
                                                                                : ResultCode::InvalidState;
                break;
            }
            account.bank_balance -= request.amount;
            inventory.bells += static_cast<std::uint32_t>(request.amount);
            inventory_changed = true;
            ledger_changed = true;
            break;
        }
        case EconomyOpType::PayDebt: {
            if (request.expected_aux_revision != account.revision || request.amount == 0 ||
                request.amount > inventory.bells || request.amount > account.debt) {
                result.code = request.expected_aux_revision != account.revision ? ResultCode::StaleRevision
                                                                                : ResultCode::InvalidState;
                break;
            }
            inventory.bells -= static_cast<std::uint32_t>(request.amount);
            account.debt -= request.amount;
            inventory_changed = true;
            ledger_changed = true;
            break;
        }
        case EconomyOpType::Donate: {
            if (request.expected_aux_revision != museum.revision || request.inventory_slot >= inventory.slots.size()) {
                result.code = request.expected_aux_revision != museum.revision ? ResultCode::StaleRevision
                                                                               : ResultCode::Malformed;
                break;
            }
            ItemSlot& slot = inventory.slots[request.inventory_slot];
            if (slot.item == 0 || (request.expected_item != 0 && slot.item != request.expected_item) ||
                museum.donated_items.find(slot.item) != museum.donated_items.end()) {
                result.code = ResultCode::InvalidState;
                break;
            }
            result.item = slot.item;
            museum.donated_items.insert(slot.item);
            slot = {};
            inventory_changed = true;
            museum_changed = true;
            break;
        }
        case EconomyOpType::AttachMail: {
            if (request.recipient == 0 || ledgers_.find(request.recipient) == ledgers_.end() ||
                request.inventory_slot >= inventory.slots.size()) {
                result.code = ResultCode::NotFound;
                break;
            }
            ItemSlot& slot = inventory.slots[request.inventory_slot];
            if (slot.item == 0 || (request.expected_item != 0 && slot.item != request.expected_item) ||
                request.recipient == request.account) {
                result.code = ResultCode::InvalidState;
                break;
            }
            if (mailbox_for(request.recipient).mail.size() >= kMailboxCapacity) {
                result.code = ResultCode::Capacity;
                break;
            }
            mail_recipient = request.recipient;
            mail_attachment = slot.item;
            result.item = slot.item;
            result.mail_id = next_mail_id_;
            slot = {};
            inventory_changed = true;
            break;
        }
        case EconomyOpType::TakeMail: {
            const auto letter = mail_.find(request.mail_id);
            if (request.mail_id == 0 || letter == mail_.end() || letter->second.recipient != request.account ||
                letter->second.location != MailLocation::Mailbox) {
                result.code = ResultCode::NotFound;
                break;
            }
            const MailboxState& box = mailbox_for(request.account);
            if (request.expected_aux_revision != box.revision) {
                result.code = ResultCode::StaleRevision;
                break;
            }
            if (box.carried.size() >= kCarriedMailCapacity) {
                result.code = ResultCode::Capacity;
                break;
            }
            taken_mail = letter->second.id;
            result.item = letter->second.attachment;
            result.mail_id = letter->second.id;
            break;
        }
        case EconomyOpType::ClaimMail: {
            /* Only a carried letter gives up its present, exactly as in the
             * original: the letter itself survives with an empty attachment. */
            const auto letter = mail_.find(request.mail_id);
            if (request.mail_id == 0 || letter == mail_.end() || letter->second.recipient != request.account ||
                letter->second.location != MailLocation::Carried) {
                result.code = ResultCode::NotFound;
                break;
            }
            const MailboxState& box = mailbox_for(request.account);
            if (request.expected_aux_revision != box.revision) {
                result.code = ResultCode::StaleRevision;
                break;
            }
            if (letter->second.attachment == 0) {
                result.code = ResultCode::InvalidState;
                break;
            }
            const auto slot = empty_slot(inventory);
            if (!slot.has_value()) {
                result.code = ResultCode::Capacity;
                break;
            }
            inventory.slots[*slot].item = letter->second.attachment;
            inventory.slots[*slot].condition = 0;
            result.item = letter->second.attachment;
            result.inventory_slot = *slot;
            inventory_changed = true;
            claimed_mail = letter->second.id;
            result.mail_id = letter->second.id;
            break;
        }
        case EconomyOpType::DiscardMail: {
            /* Without this a full pocket of letters is a dead end: the player
             * could never take another one out of the mailbox. */
            const auto letter = mail_.find(request.mail_id);
            if (request.mail_id == 0 || letter == mail_.end() || letter->second.recipient != request.account) {
                result.code = ResultCode::NotFound;
                break;
            }
            const MailboxState& box = mailbox_for(request.account);
            if (request.expected_aux_revision != box.revision) {
                result.code = ResultCode::StaleRevision;
                break;
            }
            /* A letter still holding a present would take the item with it. */
            if (letter->second.attachment != 0) {
                result.code = ResultCode::InvalidState;
                break;
            }
            discarded_mail = letter->second.id;
            result.mail_id = letter->second.id;
            break;
        }
        case EconomyOpType::HoldItem: {
            /* A swap, not a take: equipping moves the pocket item into the hand
             * and whatever was held back into that pocket, putting away names
             * an empty slot, and swapping tools names the slot of the next one.
             * One operation covers all three, and because it is a swap it can
             * neither create nor destroy an item -- the failure that let a tool
             * exist in the hand and the pocket at once. */
            if (request.inventory_slot >= inventory.slots.size()) {
                result.code = ResultCode::Malformed;
                break;
            }
            ItemSlot& slot = inventory.slots[request.inventory_slot];
            if (request.expected_item != 0 && slot.item != request.expected_item) {
                result.code = ResultCode::InvalidState;
                break;
            }
            if (slot.item == 0 && inventory.equipped.item == 0) {
                result.code = ResultCode::InvalidState;
                break;
            }
            const ItemSlot held = inventory.equipped;
            inventory.equipped = slot;
            slot = held;
            result.item = inventory.equipped.item;
            inventory_changed = true;
            break;
        }
        case EconomyOpType::Grant: {
            /* Client-trusted, and the only one -- see the note on the enum. The
             * server checks the two things it actually can: that this is a real
             * item, and that there is somewhere to put it. */
            if (request.expected_item == 0) {
                result.code = ResultCode::Malformed;
                break;
            }
            const auto slot = empty_slot(inventory);
            if (!slot.has_value()) {
                result.code = ResultCode::Capacity;
                break;
            }
            inventory.slots[*slot].item = request.expected_item;
            inventory.slots[*slot].condition = static_cast<std::uint8_t>(request.amount & 0xFFU);
            result.item = request.expected_item;
            result.inventory_slot = *slot;
            inventory_changed = true;
            break;
        }
        case EconomyOpType::AdminGrantBells:
        case EconomyOpType::AdminSendMail:
            result.code = ResultCode::Unauthorized;
            break;
    }

    if (result.code != ResultCode::Ok) {
        EconomyResult rejected = reject(request, result.code);
        if (request.type == EconomyOpType::Buy) rejected.auxiliary_revision = shop_.revision;
        if (request.type == EconomyOpType::Donate) rejected.auxiliary_revision = museum_.revision;
        idempotency_[key] = rejected;
        return rejected;
    }
    if (inventory_changed) inventory.revision = next_revision(inventory.revision);
    if (ledger_changed) account.revision = next_revision(account.revision);
    if (shop_changed) shop.revision = next_revision(shop.revision);
    if (museum_changed) museum.revision = next_revision(museum.revision);
    result.inventory_revision = inventory.revision;
    result.auxiliary_revision = shop_changed ? shop.revision : museum_changed ? museum.revision : account.revision;
    result.balance = account.bank_balance;
    result.debt = account.debt;
    result.bells = inventory.bells;
    /* A mail operation reports the acting account's own mail revision. A
     * delivery leaves the sender's untouched; taking, claiming, and discarding
     * all move the actor's own letters. */
    if (mail_operation(request.type)) {
        MailboxState& box = mailbox_for(request.account);
        const bool own_mail_changed = taken_mail != 0 || claimed_mail != 0 || discarded_mail != 0;
        result.auxiliary_revision = own_mail_changed ? next_revision(box.revision) : box.revision;
    }

    if (commit_hook_ && !commit_hook_(request, result, inventory)) {
        return reject(request, ResultCode::InternalError);
    }
    world_->set_inventory(request.account, inventory);
    ledger_it->second = account;
    shop_ = std::move(shop);
    museum_ = std::move(museum);
    if (mail_recipient != 0) {
        const std::uint64_t delivered = deliver_mail(request.account, mail_recipient, mail_attachment, mail_content);
        result.mail_id = delivered;
    }
    if (taken_mail != 0) {
        MailboxState& box = mailbox_for(request.account);
        box.mail.erase(std::remove(box.mail.begin(), box.mail.end(), taken_mail), box.mail.end());
        box.carried.push_back(taken_mail);
        MailRecord& letter = mail_[taken_mail];
        letter.location = MailLocation::Carried;
        letter.revision = next_revision(letter.revision);
        box.revision = next_revision(box.revision);
    }
    if (claimed_mail != 0) {
        MailboxState& box = mailbox_for(request.account);
        MailRecord& letter = mail_[claimed_mail];
        letter.attachment = 0;
        letter.revision = next_revision(letter.revision);
        box.revision = next_revision(box.revision);
    }
    if (discarded_mail != 0) {
        MailboxState& box = mailbox_for(request.account);
        box.mail.erase(std::remove(box.mail.begin(), box.mail.end(), discarded_mail), box.mail.end());
        box.carried.erase(std::remove(box.carried.begin(), box.carried.end(), discarded_mail), box.carried.end());
        box.revision = next_revision(box.revision);
        mail_.erase(discarded_mail);
    }
    idempotency_[key] = result;
    return result;
}

EconomyResult EconomyAuthority::admin_grant_bank_bells(AccountId account, std::uint64_t amount) {
    EconomyRequest request;
    request.type = EconomyOpType::AdminGrantBells;
    request.account = account;
    request.amount = amount;
    const auto found = ledgers_.find(account);
    if (account == 0 || found == ledgers_.end()) return reject(request, ResultCode::NotFound);
    if (amount == 0 || amount > std::numeric_limits<std::uint64_t>::max() - found->second.bank_balance) {
        return reject(request, ResultCode::InvalidState);
    }
    found->second.bank_balance += amount;
    found->second.revision = next_revision(found->second.revision);
    EconomyResult result;
    result.code = ResultCode::Ok;
    result.type = EconomyOpType::AdminGrantBells;
    result.auxiliary_revision = found->second.revision;
    result.balance = found->second.bank_balance;
    result.debt = found->second.debt;
    if (world_ != nullptr) {
        const InventoryState* inventory = world_->inventory(account);
        if (inventory != nullptr) {
            result.inventory_revision = inventory->revision;
            result.bells = inventory->bells;
        }
    }
    return result;
}

EconomyResult EconomyAuthority::admin_send_mail(AccountId recipient,
                                                std::uint16_t attachment,
                                                const MailContent& content) {
    EconomyRequest request;
    request.type = EconomyOpType::AdminSendMail;
    request.account = recipient;
    request.recipient = recipient;
    if (recipient == 0 || ledgers_.find(recipient) == ledgers_.end()) {
        return reject(request, ResultCode::NotFound);
    }
    if (mailbox_for(recipient).mail.size() >= kMailboxCapacity) return reject(request, ResultCode::Capacity);
    EconomyResult result;
    result.code = ResultCode::Ok;
    result.type = EconomyOpType::AdminSendMail;
    result.item = attachment;
    result.mail_id = deliver_mail(kAdministratorAccount, recipient, attachment, content);
    result.auxiliary_revision = mailbox_for(recipient).revision;
    const AccountLedger* account = ledger(recipient);
    if (account != nullptr) {
        result.balance = account->bank_balance;
        result.debt = account->debt;
    }
    return result;
}

bool EconomyAuthority::validate_context(const EconomyRequest& request) const {
    if (players_ == nullptr) return true;
    const PlayerView* player = players_->by_account(request.account);
    if (player == nullptr || !player->interaction_eligible) return false;
    ZoneId required_zone = 0;
    switch (request.type) {
        case EconomyOpType::Buy:
        case EconomyOpType::Sell:
            required_zone = config_.shop_zone;
            break;
        case EconomyOpType::Donate:
            required_zone = config_.museum_zone;
            break;
        case EconomyOpType::Deposit:
        case EconomyOpType::Withdraw:
        case EconomyOpType::PayDebt:
        case EconomyOpType::AttachMail:
            required_zone = config_.post_office_zone;
            break;
        case EconomyOpType::TakeMail:
        case EconomyOpType::ClaimMail:
        case EconomyOpType::DiscardMail:
            required_zone = config_.mailbox_zone;
            break;
        case EconomyOpType::Grant:
            /* An NPC can hand something over anywhere they stand, so this is
             * not tied to a zone the way a counter transaction is. */
        case EconomyOpType::HoldItem:
            /* Reaching into your own pocket works anywhere the original lets
             * the submenu open, which is everywhere. */
            break;
        case EconomyOpType::AdminGrantBells:
        case EconomyOpType::AdminSendMail:
            return false;
    }
    if (required_zone != 0 && player->zone != required_zone) return false;
    if ((request.type == EconomyOpType::Buy || request.type == EconomyOpType::Sell) &&
        config_.maximum_shop_distance > 0.0F) {
        const float dx = player->transform.position.x - config_.shop_position.x;
        const float dz = player->transform.position.z - config_.shop_position.z;
        const float maximum = config_.maximum_shop_distance;
        if (dx * dx + dz * dz > maximum * maximum) return false;
    }
    return true;
}

bool EconomyAuthority::validate_trade_context(AccountId first, AccountId second) const {
    if (players_ == nullptr) return true;
    const PlayerView* first_player = players_->by_account(first);
    const PlayerView* second_player = players_->by_account(second);
    if (first_player == nullptr || second_player == nullptr || !first_player->interaction_eligible ||
        !second_player->interaction_eligible || first_player->zone != second_player->zone) return false;
    const float dx = first_player->transform.position.x - second_player->transform.position.x;
    const float dz = first_player->transform.position.z - second_player->transform.position.z;
    const float maximum = config_.maximum_trade_distance;
    return dx * dx + dz * dz <= maximum * maximum;
}

TradeOffer* EconomyAuthority::offer_for(TradeSession& trade_value, AccountId account) {
    if (account == trade_value.first) return &trade_value.first_offer;
    if (account == trade_value.second) return &trade_value.second_offer;
    return nullptr;
}

const TradeOffer* EconomyAuthority::offer_for(const TradeSession& trade_value, AccountId account) const {
    if (account == trade_value.first) return &trade_value.first_offer;
    if (account == trade_value.second) return &trade_value.second_offer;
    return nullptr;
}

AccountId EconomyAuthority::other_account(const TradeSession& trade_value, AccountId account) const {
    if (account == trade_value.first) return trade_value.second;
    if (account == trade_value.second) return trade_value.first;
    return 0;
}

bool EconomyAuthority::validate_offer(AccountId account, const std::vector<std::uint8_t>& slots) const {
    if (world_ == nullptr || slots.size() > kInventorySlots) return false;
    const InventoryState* inventory = world_->inventory(account);
    if (inventory == nullptr) return false;
    std::set<std::uint8_t> unique;
    for (std::uint8_t slot : slots) {
        if (slot >= inventory->slots.size() || inventory->slots[slot].item == 0 || !unique.insert(slot).second) return false;
    }
    return true;
}

TradeResult EconomyAuthority::create_trade(std::uint64_t trade_id, AccountId first, AccountId second) {
    TradeResult result;
    result.trade_id = trade_id;
    if (trade_id == 0 || first == 0 || second == 0 || first == second || world_ == nullptr ||
        world_->inventory(first) == nullptr || world_->inventory(second) == nullptr) {
        result.code = ResultCode::Malformed;
        return result;
    }
    if (!validate_trade_context(first, second)) {
        result.code = ResultCode::OutOfRange;
        return result;
    }
    if (trades_.find(trade_id) != trades_.end() || account_trade_.find(first) != account_trade_.end() ||
        account_trade_.find(second) != account_trade_.end()) {
        result.code = ResultCode::Conflict;
        return result;
    }
    TradeSession trade_value;
    trade_value.id = trade_id;
    trade_value.first = first;
    trade_value.second = second;
    trades_[trade_id] = trade_value;
    account_trade_[first] = trade_id;
    account_trade_[second] = trade_id;
    result.code = ResultCode::Ok;
    result.trade_revision = 1;
    return result;
}

TradeResult EconomyAuthority::update_trade_offer(std::uint64_t trade_id,
                                                 AccountId account,
                                                 Revision expected_trade_revision,
                                                 const std::vector<std::uint8_t>& slots) {
    TradeResult result;
    result.trade_id = trade_id;
    const auto found = trades_.find(trade_id);
    if (found == trades_.end()) {
        result.code = ResultCode::NotFound;
        return result;
    }
    TradeSession& trade_value = found->second;
    result.trade_revision = trade_value.revision;
    TradeOffer* offer = offer_for(trade_value, account);
    if (offer == nullptr || trade_value.complete || trade_value.cancelled || !validate_offer(account, slots)) {
        result.code = ResultCode::InvalidState;
        return result;
    }
    if (expected_trade_revision != trade_value.revision) {
        result.code = ResultCode::StaleRevision;
        return result;
    }
    offer->slots = slots;
    offer->inventory_revision = world_->inventory(account)->revision;
    offer->confirmed = false;
    offer->revision = next_revision(offer->revision);
    trade_value.first_offer.confirmed = false;
    trade_value.second_offer.confirmed = false;
    trade_value.revision = next_revision(trade_value.revision);
    result.code = ResultCode::Ok;
    result.trade_revision = trade_value.revision;
    result.inventory_revision = offer->inventory_revision;
    return result;
}

TradeResult EconomyAuthority::finalize_trade(TradeSession& trade_value) {
    TradeResult result;
    result.trade_id = trade_value.id;
    result.trade_revision = trade_value.revision;
    const InventoryState* current_first = world_->inventory(trade_value.first);
    const InventoryState* current_second = world_->inventory(trade_value.second);
    if (current_first == nullptr || current_second == nullptr ||
        current_first->revision != trade_value.first_offer.inventory_revision ||
        current_second->revision != trade_value.second_offer.inventory_revision) {
        result.code = ResultCode::StaleRevision;
        trade_value.first_offer.confirmed = false;
        trade_value.second_offer.confirmed = false;
        return result;
    }
    InventoryState first = *current_first;
    InventoryState second = *current_second;
    std::vector<ItemSlot> first_items;
    std::vector<ItemSlot> second_items;
    for (std::uint8_t slot : trade_value.first_offer.slots) {
        first_items.push_back(first.slots[slot]);
        first.slots[slot] = {};
    }
    for (std::uint8_t slot : trade_value.second_offer.slots) {
        second_items.push_back(second.slots[slot]);
        second.slots[slot] = {};
    }
    const auto insert_all = [](InventoryState& inventory, const std::vector<ItemSlot>& items) {
        for (const ItemSlot& item : items) {
            const auto slot = EconomyAuthority::empty_slot(inventory);
            if (!slot.has_value()) return false;
            inventory.slots[*slot] = item;
        }
        return true;
    };
    if (!insert_all(first, second_items) || !insert_all(second, first_items)) {
        result.code = ResultCode::Capacity;
        trade_value.first_offer.confirmed = false;
        trade_value.second_offer.confirmed = false;
        return result;
    }
    first.revision = next_revision(first.revision);
    second.revision = next_revision(second.revision);
    if (!world_->set_inventory(trade_value.first, first) || !world_->set_inventory(trade_value.second, second)) {
        result.code = ResultCode::InternalError;
        return result;
    }
    trade_value.complete = true;
    trade_value.revision = next_revision(trade_value.revision);
    account_trade_.erase(trade_value.first);
    account_trade_.erase(trade_value.second);
    result.code = ResultCode::Ok;
    result.trade_revision = trade_value.revision;
    result.inventory_revision = first.revision;
    result.finalized = true;
    return result;
}

TradeResult EconomyAuthority::confirm_trade(std::uint64_t trade_id,
                                            AccountId account,
                                            Revision expected_trade_revision) {
    TradeResult result;
    result.trade_id = trade_id;
    const auto found = trades_.find(trade_id);
    if (found == trades_.end()) {
        result.code = ResultCode::NotFound;
        return result;
    }
    TradeSession& trade_value = found->second;
    result.trade_revision = trade_value.revision;
    if (trade_value.complete) {
        result.code = ResultCode::Ok;
        result.finalized = true;
        return result;
    }
    TradeOffer* offer = offer_for(trade_value, account);
    if (offer == nullptr || offer->slots.empty() || trade_value.cancelled) {
        result.code = ResultCode::InvalidState;
        return result;
    }
    if (expected_trade_revision != trade_value.revision) {
        result.code = ResultCode::StaleRevision;
        return result;
    }
    if (world_->inventory(account)->revision != offer->inventory_revision) {
        result.code = ResultCode::StaleRevision;
        offer->confirmed = false;
        return result;
    }
    offer->confirmed = true;
    const TradeOffer* other = offer_for(trade_value, other_account(trade_value, account));
    if (other != nullptr && other->confirmed) return finalize_trade(trade_value);
    result.code = ResultCode::Ok;
    result.inventory_revision = offer->inventory_revision;
    return result;
}

bool EconomyAuthority::cancel_trade(std::uint64_t trade_id) {
    const auto found = trades_.find(trade_id);
    if (found == trades_.end() || found->second.complete) return false;
    found->second.cancelled = true;
    account_trade_.erase(found->second.first);
    account_trade_.erase(found->second.second);
    return true;
}

const TradeSession* EconomyAuthority::trade(std::uint64_t trade_id) const {
    const auto found = trades_.find(trade_id);
    return found == trades_.end() ? nullptr : &found->second;
}

std::uint64_t EconomyAuthority::total_bells() const {
    std::uint64_t result = 0;
    for (const auto& item : ledgers_) result += item.second.bank_balance;
    if (world_ != nullptr) {
        for (const auto& item : ledgers_) {
            const InventoryState* inventory = world_->inventory(item.first);
            if (inventory != nullptr) result += inventory->bells;
        }
    }
    return result;
}

std::uint64_t EconomyAuthority::total_item_units() const {
    std::uint64_t result = world_ == nullptr ? 0 : world_->total_item_units();
    for (const ShopEntry& entry : shop_.stock) result += entry.quantity;
    result += museum_.donated_items.size();
    for (const auto& item : mail_) {
        if (item.second.attachment != 0) ++result;
    }
    return result;
}

} // namespace acnet
