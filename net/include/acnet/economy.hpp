#pragma once

#include "acnet/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace acnet {

/* Appended, never reordered: the wire encodes this enum as a u8 and validates
 * it against the last client-reachable value.  Everything at or below
 * kMaximumClientEconomyOp may arrive in a client request; the administrative
 * operations above it exist only inside the server process and are rejected by
 * both the decoder and EconomyAuthority::apply. */
enum class EconomyOpType : std::uint8_t {
    Buy,
    Sell,
    Deposit,
    Withdraw,
    PayDebt,
    Donate,
    AttachMail,
    ClaimMail,
    TakeMail,
    DiscardMail,
    /* Swap a pocket slot with the hand: equipping, putting away, and the
     * pocket-menu drag onto the player are all the same move, exactly as
     * mHD_drop_item performs it locally. */
    HoldItem,
    AdminGrantBells,
    AdminSendMail,
};

constexpr std::uint8_t kMaximumClientEconomyOp = static_cast<std::uint8_t>(EconomyOpType::HoldItem);

/* The original game moves a letter in two steps: the house mailbox holds ten,
 * the player then carries up to ten in their pockets, and only a carried letter
 * gives up its present. Both halves are server-owned, so the same two steps are
 * two transactions -- TakeMail then ClaimMail -- and the capacities match the
 * original arrays (HOME_MAILBOX_SIZE and mPr_INVENTORY_MAIL_COUNT). */
enum class MailLocation : std::uint8_t {
    Mailbox,
    Carried,
};

constexpr std::size_t kMailboxCapacity = 10;
constexpr std::size_t kCarriedMailCapacity = 10;

/* A letter is carried whole so the original UI can render it unchanged: the
 * text is opaque bytes in the game's own font encoding, sized to the fields of
 * Mail_c (MAIL_HEADER_LEN, MAIL_BODY_LEN, MAIL_FOOTER_LEN, Mail_nm_c). */
constexpr std::size_t kMailNameBytes = 22;
constexpr std::size_t kMailHeaderBytes = 24;
constexpr std::size_t kMailBodyBytes = 192;
constexpr std::size_t kMailFooterBytes = 32;

/* Sender sentinel for letters the town operator posts. No account is ever 0. */
constexpr AccountId kAdministratorAccount = 0;

struct ShopEntry {
    std::uint16_t item = 0;
    std::uint32_t price = 0;
    std::uint16_t quantity = 0;
};

struct ShopState {
    Revision revision = 1;
    std::vector<ShopEntry> stock;
    /* The spotlight rare furniture, which the game keeps in its own Shop_c
     * field as well as on the shelf. It cannot be recovered from `stock`
     * alone -- nothing there marks which row was the rare draw -- so it is
     * replicated beside it. Zero at the tiers that stock no rare item. */
    std::uint16_t rare_item = 0;
};

struct AccountLedger {
    Revision revision = 1;
    std::uint64_t bank_balance = 0;
    std::uint64_t debt = 0;
};

struct MuseumState {
    Revision revision = 1;
    std::unordered_set<std::uint16_t> donated_items;
};

struct MailContent {
    std::uint8_t font = 0;        /* mMl_FONT_* */
    std::uint8_t mail_type = 0;
    std::uint8_t paper_type = 0;
    std::uint8_t header_back_start = 0;
    std::array<std::uint8_t, kMailNameBytes> sender_name{};
    std::array<std::uint8_t, kMailHeaderBytes> header{};
    std::array<std::uint8_t, kMailBodyBytes> body{};
    std::array<std::uint8_t, kMailFooterBytes> footer{};
};

struct MailRecord {
    std::uint64_t id = 0;
    AccountId sender = 0; // kAdministratorAccount when the town operator posted it
    AccountId recipient = 0;
    std::uint16_t attachment = 0;
    Revision revision = 1;
    MailLocation location = MailLocation::Mailbox;
    MailContent content;
};

/* One account's letters, oldest first in each list. The identifiers are the
 * authoritative ordering, and a single revision covers both lists: every
 * delivery, take, claim, and discard bumps it, so a client always quotes one
 * observed value and cannot act on a stale view of either half. */
struct MailboxState {
    Revision revision = 1;
    std::vector<std::uint64_t> mail;
    std::vector<std::uint64_t> carried;
};

struct EconomyRequest {
    EconomyOpType type = EconomyOpType::Buy;
    AccountId account = 0;
    IdempotencyKey idempotency;
    Revision expected_inventory_revision = 0;
    Revision expected_aux_revision = 0;
    std::uint32_t shop_index = 0;
    std::uint8_t inventory_slot = 0;
    /* Sell only: sell every pocket whose bit is set, as one transaction. The
     * shop counter sells a whole selection at once and quotes a single total,
     * and a per-slot request cannot express that -- each would have to quote
     * the revision the previous one produced, so a client issuing them together
     * would have all but the first rejected as stale. Zero means "just
     * inventory_slot"; bit i is pocket i, so kInventorySlots fits exactly. */
    std::uint16_t slot_mask = 0;
    std::uint16_t expected_item = 0;
    std::uint64_t amount = 0;
    AccountId recipient = 0;
    std::uint64_t mail_id = 0;
};

struct EconomyResult {
    ResultCode code = ResultCode::InternalError;
    /* Echoed so a client can tell which authority auxiliary_revision belongs
     * to: the shop for Buy/Sell, the museum for Donate, the bank ledger for
     * Deposit/Withdraw/PayDebt, and a mailbox for the mail operations. */
    EconomyOpType type = EconomyOpType::Buy;
    IdempotencyKey idempotency;
    Revision inventory_revision = 0;
    Revision auxiliary_revision = 0;
    std::uint64_t balance = 0;
    std::uint64_t debt = 0;
    std::uint32_t bells = 0;
    std::uint16_t item = 0;
    std::uint8_t inventory_slot = 0;
    std::uint64_t mail_id = 0;
    bool replayed = false;
};

struct TradeOffer {
    Revision revision = 0;
    std::vector<std::uint8_t> slots;
    Revision inventory_revision = 0;
    bool confirmed = false;
};

struct TradeSession {
    std::uint64_t id = 0;
    AccountId first = 0;
    AccountId second = 0;
    Revision revision = 1;
    TradeOffer first_offer;
    TradeOffer second_offer;
    bool complete = false;
    bool cancelled = false;
};

struct TradeResult {
    ResultCode code = ResultCode::InternalError;
    std::uint64_t trade_id = 0;
    Revision trade_revision = 0;
    Revision inventory_revision = 0;
    bool finalized = false;
};

using EconomyCommitHook = std::function<bool(const EconomyRequest&,
                                             const EconomyResult&,
                                             const InventoryState&)>;

/* What Nook pays for one item. Returning 0 means the item is not sellable. */
using SellPriceResolver = std::function<std::uint32_t(std::uint16_t item)>;

struct EconomyConfig {
    ZoneId shop_zone = 0;
    Vec3 shop_position{};
    float maximum_shop_distance = 0.0F;
    ZoneId museum_zone = 0;
    ZoneId post_office_zone = 0;
    /* Letters are posted at the post office but read at the mailbox outside the
     * recipient's house, so claiming is not restricted to one zone by default.
     * Ownership, the mailbox revision, and inventory capacity are what make the
     * claim safe; the zone is only a fidelity rule. */
    ZoneId mailbox_zone = 0;
    float maximum_trade_distance = 120.0F;
    /* The wallet cap, and what happens above it: the original peels `chunk`
     * bells at a time into a `bag_item` in the pockets, which is why a big sale
     * hands back a money bag. Zero disables the rule, which is the default so
     * the authority carries no game constants of its own -- the town runtime
     * configures it from the generated tables. */
    std::uint32_t wallet_maximum = 0;
    std::uint32_t wallet_overflow_chunk = 0;
    std::uint16_t wallet_overflow_item = 0;
};

class EconomyAuthority {
public:
    explicit EconomyAuthority(WorldAuthority* world,
                              PlayerDirectory* players = nullptr,
                              EconomyConfig config = {});

    bool register_account(AccountId account, const AccountLedger& ledger = {});
    bool set_account(AccountId account, const AccountLedger& ledger);
    void set_shop(const ShopState& shop) { shop_ = shop; }
    void set_museum(const MuseumState& museum) { museum_ = museum; }
    void set_sell_price(std::uint16_t item, std::uint32_t price);
    /* Fallback for items with no explicit override, consulted on every sale.
     * The authority stays free of the game's price tables this way: the town
     * runtime installs a resolver over the generated ones, and a test can
     * price a handful of items by hand instead. An item no resolver prices
     * cannot be sold. */
    void set_sell_price_resolver(SellPriceResolver resolver) { sell_price_resolver_ = std::move(resolver); }
    void set_commit_hook(EconomyCommitHook hook) { commit_hook_ = std::move(hook); }

    const ShopState& shop() const { return shop_; }
    const MuseumState& museum() const { return museum_; }
    const AccountLedger* ledger(AccountId account) const;
    const MailRecord* mail(std::uint64_t mail_id) const;
    const MailboxState* mailbox(AccountId account) const;
    const std::unordered_map<AccountId, AccountLedger>& ledgers() const { return ledgers_; }
    const std::unordered_map<std::uint64_t, MailRecord>& mail_records() const { return mail_; }
    const std::unordered_map<AccountId, MailboxState>& mailboxes() const { return mailboxes_; }
    std::vector<MailRecord> mail_for(AccountId account) const;
    bool restore_mail(const MailRecord& record);
    bool restore_mailbox_revision(AccountId account, Revision revision);
    /* Startup decodes a checkpoint and then the newest journalled state on the
     * same authority. Mail is the one collection restored by appending rather
     * than by key, so the caller drops the previous set first: otherwise a
     * letter present in both would be rejected as a duplicate, and one claimed
     * between the two would come back to life and be claimable again. */
    void clear_mail();
    EconomyResult apply(const EconomyRequest& request);

    /* Operator actions.  They carry no idempotency key and skip the player
     * context checks a client request must pass -- the operator is not standing
     * in the post office -- but they commit through the same ledger, mailbox,
     * and revision rules, so the caller still journals the result. */
    EconomyResult admin_grant_bank_bells(AccountId account, std::uint64_t amount);
    EconomyResult admin_send_mail(AccountId recipient, std::uint16_t attachment, const MailContent& content);

    TradeResult create_trade(std::uint64_t trade_id, AccountId first, AccountId second);
    TradeResult update_trade_offer(std::uint64_t trade_id,
                                   AccountId account,
                                   Revision expected_trade_revision,
                                   const std::vector<std::uint8_t>& slots);
    TradeResult confirm_trade(std::uint64_t trade_id, AccountId account, Revision expected_trade_revision);
    bool cancel_trade(std::uint64_t trade_id);
    const TradeSession* trade(std::uint64_t trade_id) const;

    std::uint64_t total_bells() const;
    std::uint64_t total_item_units() const;

private:
    struct OperationKey {
        AccountId account;
        IdempotencyKey idempotency;
        bool operator==(const OperationKey& other) const {
            return account == other.account && idempotency == other.idempotency;
        }
    };
    struct OperationKeyHash {
        std::size_t operator()(const OperationKey& value) const;
    };

    static Revision next_revision(Revision revision);
    static std::optional<std::uint8_t> empty_slot(const InventoryState& inventory);
    MailboxState& mailbox_for(AccountId account);
    std::uint64_t deliver_mail(AccountId sender,
                               AccountId recipient,
                               std::uint16_t attachment,
                               const MailContent& content);
    EconomyResult reject(const EconomyRequest& request, ResultCode code) const;
    TradeOffer* offer_for(TradeSession& trade, AccountId account);
    const TradeOffer* offer_for(const TradeSession& trade, AccountId account) const;
    AccountId other_account(const TradeSession& trade, AccountId account) const;
    bool validate_offer(AccountId account, const std::vector<std::uint8_t>& slots) const;
    bool validate_context(const EconomyRequest& request) const;
    bool validate_trade_context(AccountId first, AccountId second) const;
    TradeResult finalize_trade(TradeSession& trade);

    WorldAuthority* world_;
    PlayerDirectory* players_;
    EconomyConfig config_;
    ShopState shop_;
    MuseumState museum_;
    EconomyCommitHook commit_hook_;
    std::unordered_map<AccountId, AccountLedger> ledgers_;
    std::unordered_map<std::uint16_t, std::uint32_t> sell_prices_;
    SellPriceResolver sell_price_resolver_;
    std::unordered_map<std::uint64_t, MailRecord> mail_;
    std::unordered_map<AccountId, MailboxState> mailboxes_;
    std::unordered_map<OperationKey, EconomyResult, OperationKeyHash> idempotency_;
    std::unordered_map<std::uint64_t, TradeSession> trades_;
    std::unordered_map<AccountId, std::uint64_t> account_trade_;
    std::uint64_t next_mail_id_ = 1;
};

} // namespace acnet
