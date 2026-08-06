#pragma once

#include "acnet/world.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace acnet {

enum class EconomyOpType : std::uint8_t {
    Buy,
    Sell,
    Deposit,
    Withdraw,
    PayDebt,
    Donate,
    AttachMail,
};

struct ShopEntry {
    std::uint16_t item = 0;
    std::uint32_t price = 0;
    std::uint16_t quantity = 0;
};

struct ShopState {
    Revision revision = 1;
    std::vector<ShopEntry> stock;
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

struct MailRecord {
    std::uint64_t id = 0;
    AccountId sender = 0;
    AccountId recipient = 0;
    std::uint16_t attachment = 0;
    Revision revision = 1;
};

struct EconomyRequest {
    EconomyOpType type = EconomyOpType::Buy;
    AccountId account = 0;
    IdempotencyKey idempotency;
    Revision expected_inventory_revision = 0;
    Revision expected_aux_revision = 0;
    std::uint32_t shop_index = 0;
    std::uint8_t inventory_slot = 0;
    std::uint16_t expected_item = 0;
    std::uint64_t amount = 0;
    AccountId recipient = 0;
};

struct EconomyResult {
    ResultCode code = ResultCode::InternalError;
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

struct EconomyConfig {
    ZoneId shop_zone = 0;
    Vec3 shop_position{};
    float maximum_shop_distance = 0.0F;
    ZoneId museum_zone = 0;
    ZoneId post_office_zone = 0;
    float maximum_trade_distance = 120.0F;
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
    void set_commit_hook(EconomyCommitHook hook) { commit_hook_ = std::move(hook); }

    const ShopState& shop() const { return shop_; }
    const MuseumState& museum() const { return museum_; }
    const AccountLedger* ledger(AccountId account) const;
    const MailRecord* mail(std::uint64_t mail_id) const;
    const std::unordered_map<AccountId, AccountLedger>& ledgers() const { return ledgers_; }
    const std::unordered_map<std::uint64_t, MailRecord>& mail_records() const { return mail_; }
    bool restore_mail(const MailRecord& record);
    EconomyResult apply(const EconomyRequest& request);

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
    std::unordered_map<std::uint64_t, MailRecord> mail_;
    std::unordered_map<OperationKey, EconomyResult, OperationKeyHash> idempotency_;
    std::unordered_map<std::uint64_t, TradeSession> trades_;
    std::unordered_map<AccountId, std::uint64_t> account_trade_;
    std::uint64_t next_mail_id_ = 1;
};

} // namespace acnet
