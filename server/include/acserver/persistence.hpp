#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace acserver {

struct JournalRecord {
    std::uint64_t sequence = 0;
    std::uint16_t type = 0;
    std::vector<std::uint8_t> payload;
};

struct Checkpoint {
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;
    std::filesystem::path path;
};

struct ReplayReport {
    std::size_t applied = 0;
    std::size_t ignored_before_checkpoint = 0;
    bool truncated_tail = false;
    std::uint64_t last_sequence = 0;
    std::uint64_t valid_bytes = 0;
};

struct PersistenceConfig {
    std::size_t retained_checkpoints = 7;
    std::size_t maximum_checkpoint_bytes = 64U * 1024U * 1024U;
    std::size_t maximum_journal_payload_bytes = 1024U * 1024U;
};

using ReplayCallback = std::function<bool(const JournalRecord&)>;

class PersistenceStore {
public:
    explicit PersistenceStore(std::filesystem::path root, PersistenceConfig config = {});

    bool initialize(std::string& error);
    bool append(const JournalRecord& record, std::string& error);
    bool write_checkpoint(std::uint64_t sequence,
                          const std::vector<std::uint8_t>& payload,
                          std::string& error);
    std::optional<Checkpoint> load_latest_checkpoint(std::string& error) const;
    bool replay(std::uint64_t after_sequence,
                const ReplayCallback& callback,
                ReplayReport& report,
                std::string& error) const;

    bool import_gci(const std::filesystem::path& source, std::string& error);
    bool export_gci(const std::filesystem::path& destination, std::string& error) const;
    bool load_gci_bytes(std::vector<std::uint8_t>& bytes, std::string& error) const;
    bool export_gci_bytes(const std::vector<std::uint8_t>& bytes,
                          const std::filesystem::path& destination,
                          std::string& error) const;
    bool mark_clean_shutdown(std::string& error);

    bool previous_shutdown_was_clean() const { return previous_clean_; }
    std::uint64_t last_sequence() const { return last_sequence_; }
    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path journal_path() const;

private:
    bool rotate_checkpoints(std::string& error);
    bool durable_write(const std::filesystem::path& destination,
                       const std::vector<std::uint8_t>& bytes,
                       std::string& error) const;
    bool scan_last_sequence(std::string& error);

    std::filesystem::path root_;
    PersistenceConfig config_;
    std::uint64_t last_sequence_ = 0;
    bool previous_clean_ = false;
    bool initialized_ = false;
};

} // namespace acserver
