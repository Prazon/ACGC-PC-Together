#include "acserver/persistence.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace acserver {
namespace {

constexpr std::uint32_t kCheckpointMagic = 0x41434B50U; // ACKP
constexpr std::uint32_t kJournalMagic = 0x41434A52U;    // ACJR
constexpr std::uint16_t kStorageVersion = 1;
constexpr std::size_t kCheckpointHeaderBytes = 24;
constexpr std::size_t kJournalHeaderBytes = 20;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value >> 32));
    append_u32(output, static_cast<std::uint32_t>(value));
}

bool read_u16(const std::vector<std::uint8_t>& input, std::size_t& offset, std::uint16_t& value) {
    if (offset > input.size() || input.size() - offset < 2) return false;
    value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[offset]) << 8) | input[offset + 1]);
    offset += 2;
    return true;
}

bool read_u32(const std::vector<std::uint8_t>& input, std::size_t& offset, std::uint32_t& value) {
    if (offset > input.size() || input.size() - offset < 4) return false;
    value = (static_cast<std::uint32_t>(input[offset]) << 24) |
            (static_cast<std::uint32_t>(input[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(input[offset + 2]) << 8) |
            input[offset + 3];
    offset += 4;
    return true;
}

bool read_u64(const std::vector<std::uint8_t>& input, std::size_t& offset, std::uint64_t& value) {
    std::uint32_t high;
    std::uint32_t low;
    if (!read_u32(input, offset, high) || !read_u32(input, offset, low)) return false;
    value = (static_cast<std::uint64_t>(high) << 32) | low;
    return true;
}

bool read_file(const std::filesystem::path& path,
               std::size_t maximum_size,
               std::vector<std::uint8_t>& output,
               std::string& error) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maximum_size || size > std::numeric_limits<std::size_t>::max()) {
        error = "invalid or oversized file: " + path.string();
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "failed to open file: " + path.string();
        return false;
    }
    output.resize(static_cast<std::size_t>(size));
    if (!output.empty()) stream.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!stream && !output.empty()) {
        error = "failed to read file: " + path.string();
        return false;
    }
    return true;
}

bool flush_file(std::FILE* file) {
    if (std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

bool valid_gci(const std::vector<std::uint8_t>& bytes) {
    constexpr std::size_t header_size = 0x40;
    constexpr std::size_t block_size = 0x2000;
    if (bytes.size() < header_size ||
        !(bytes[0] == 'G' && bytes[1] == 'A' && bytes[2] == 'F' && (bytes[3] == 'E' || bytes[3] == 'U')) ||
        bytes[4] != '0' || bytes[5] != '1') return false;
    const std::uint16_t blocks = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0x38]) << 8) |
                                                            bytes[0x39]);
    return blocks != 0 && bytes.size() == header_size + static_cast<std::size_t>(blocks) * block_size;
}

std::optional<std::uint64_t> checkpoint_sequence(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    constexpr const char* prefix = "checkpoint-";
    constexpr const char* suffix = ".bin";
    if (name.rfind(prefix, 0) != 0 || name.size() <= std::strlen(prefix) + std::strlen(suffix) ||
        name.substr(name.size() - std::strlen(suffix)) != suffix) return std::nullopt;
    const std::string number = name.substr(std::strlen(prefix),
                                           name.size() - std::strlen(prefix) - std::strlen(suffix));
    try {
        std::size_t consumed = 0;
        const std::uint64_t value = std::stoull(number, &consumed);
        if (consumed != number.size()) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

PersistenceStore::PersistenceStore(std::filesystem::path root, PersistenceConfig config)
    : root_(std::move(root)), config_(config) {
    if (config_.retained_checkpoints == 0) config_.retained_checkpoints = 1;
    if (config_.maximum_checkpoint_bytes == 0) config_.maximum_checkpoint_bytes = 1;
    if (config_.maximum_journal_payload_bytes == 0) config_.maximum_journal_payload_bytes = 1;
}

std::filesystem::path PersistenceStore::journal_path() const {
    return root_ / "journal" / "operations.log";
}

bool PersistenceStore::initialize(std::string& error) {
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(root_ / "journal", ec);
    if (!ec) std::filesystem::create_directories(root_ / "snapshots", ec);
    if (ec) {
        error = "failed to create town storage: " + ec.message();
        return false;
    }
    const std::filesystem::path clean = root_ / "clean.shutdown";
    previous_clean_ = std::filesystem::exists(clean, ec) && !ec;
    std::filesystem::remove(clean, ec);
    ec.clear();
    if (!std::filesystem::exists(journal_path(), ec)) {
        std::FILE* file = std::fopen(journal_path().string().c_str(), "wb");
        if (file == nullptr) {
            error = "failed to create journal";
            return false;
        }
        if (!flush_file(file)) {
            std::fclose(file);
            error = "failed to flush journal";
            return false;
        }
        std::fclose(file);
    }
    initialized_ = true;
    return scan_last_sequence(error);
}

bool PersistenceStore::durable_write(const std::filesystem::path& destination,
                                     const std::vector<std::uint8_t>& bytes,
                                     std::string& error) const {
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (file == nullptr) {
        error = "failed to open temporary file: " + temporary.string();
        return false;
    }
    const bool wrote = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool flushed = wrote && flush_file(file);
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !flushed || !closed) {
        error = "failed durable write: " + temporary.string();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    std::filesystem::remove(destination, ec);
    ec.clear();
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        error = "failed atomic rename: " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool PersistenceStore::append(const JournalRecord& record, std::string& error) {
    error.clear();
    if (!initialized_ || record.sequence != last_sequence_ + 1 || record.type == 0 ||
        record.payload.size() > config_.maximum_journal_payload_bytes ||
        record.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "invalid journal sequence, type, or payload";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kJournalHeaderBytes + record.payload.size() + 4);
    append_u32(bytes, kJournalMagic);
    append_u16(bytes, kStorageVersion);
    append_u16(bytes, record.type);
    append_u64(bytes, record.sequence);
    append_u32(bytes, static_cast<std::uint32_t>(record.payload.size()));
    bytes.insert(bytes.end(), record.payload.begin(), record.payload.end());
    append_u32(bytes, acnet::crc32(bytes.data(), bytes.size()));
    std::FILE* file = std::fopen(journal_path().string().c_str(), "ab");
    if (file == nullptr) {
        error = "failed to append journal";
        return false;
    }
    const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool flushed = wrote && flush_file(file);
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !flushed || !closed) {
        error = "failed to commit journal record";
        return false;
    }
    last_sequence_ = record.sequence;
    return true;
}

bool PersistenceStore::write_checkpoint(std::uint64_t sequence,
                                        const std::vector<std::uint8_t>& payload,
                                        std::string& error) {
    error.clear();
    if (!initialized_ || sequence > last_sequence_ || payload.size() > config_.maximum_checkpoint_bytes) {
        error = "invalid checkpoint sequence or payload";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kCheckpointHeaderBytes + payload.size() + 4);
    append_u32(bytes, kCheckpointMagic);
    append_u16(bytes, kStorageVersion);
    append_u16(bytes, 0);
    append_u64(bytes, sequence);
    append_u64(bytes, payload.size());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    append_u32(bytes, acnet::crc32(bytes.data(), bytes.size()));
    const std::filesystem::path destination = root_ / "snapshots" / ("checkpoint-" + std::to_string(sequence) + ".bin");
    if (!durable_write(destination, bytes, error)) return false;
    return rotate_checkpoints(error);
}

std::optional<Checkpoint> PersistenceStore::load_latest_checkpoint(std::string& error) const {
    error.clear();
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> candidates;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(root_ / "snapshots", ec)) {
        const auto sequence = checkpoint_sequence(item.path());
        if (sequence.has_value()) candidates.emplace_back(*sequence, item.path());
    }
    if (ec) {
        error = "failed to enumerate checkpoints: " + ec.message();
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& candidate : candidates) {
        std::vector<std::uint8_t> bytes;
        std::string candidate_error;
        if (!read_file(candidate.second, config_.maximum_checkpoint_bytes + kCheckpointHeaderBytes + 4, bytes,
                       candidate_error) || bytes.size() < kCheckpointHeaderBytes + 4) continue;
        std::size_t offset = 0;
        std::uint32_t magic;
        std::uint16_t version;
        std::uint16_t reserved;
        std::uint64_t sequence;
        std::uint64_t payload_size;
        if (!read_u32(bytes, offset, magic) || !read_u16(bytes, offset, version) ||
            !read_u16(bytes, offset, reserved) || !read_u64(bytes, offset, sequence) ||
            !read_u64(bytes, offset, payload_size) || magic != kCheckpointMagic || version != kStorageVersion ||
            reserved != 0 || payload_size > config_.maximum_checkpoint_bytes ||
            payload_size != bytes.size() - kCheckpointHeaderBytes - 4) continue;
        const std::uint32_t expected = acnet::crc32(bytes.data(), bytes.size() - 4);
        std::size_t crc_offset = bytes.size() - 4;
        std::uint32_t actual;
        if (!read_u32(bytes, crc_offset, actual) || expected != actual || sequence != candidate.first) continue;
        Checkpoint checkpoint;
        checkpoint.sequence = sequence;
        checkpoint.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kCheckpointHeaderBytes), bytes.end() - 4);
        checkpoint.path = candidate.second;
        return checkpoint;
    }
    error.clear();
    return std::nullopt;
}

bool PersistenceStore::replay(std::uint64_t after_sequence,
                              const ReplayCallback& callback,
                              ReplayReport& report,
                              std::string& error) const {
    error.clear();
    report = {};
    std::vector<std::uint8_t> bytes;
    if (!read_file(journal_path(),
                   std::numeric_limits<std::size_t>::max(),
                   bytes,
                   error)) return false;
    std::size_t offset = 0;
    std::uint64_t previous_sequence = 0;
    while (offset < bytes.size()) {
        const std::size_t record_start = offset;
        if (bytes.size() - offset < kJournalHeaderBytes) {
            report.truncated_tail = true;
            break;
        }
        std::uint32_t magic;
        std::uint16_t version;
        JournalRecord record;
        std::uint32_t payload_size;
        if (!read_u32(bytes, offset, magic) || !read_u16(bytes, offset, version) ||
            !read_u16(bytes, offset, record.type) || !read_u64(bytes, offset, record.sequence) ||
            !read_u32(bytes, offset, payload_size)) {
            report.truncated_tail = true;
            break;
        }
        if (magic != kJournalMagic || version != kStorageVersion || record.type == 0 ||
            payload_size > config_.maximum_journal_payload_bytes ||
            record.sequence == 0 || (previous_sequence != 0 && record.sequence != previous_sequence + 1)) {
            error = "corrupt journal record at offset " + std::to_string(record_start);
            return false;
        }
        if (bytes.size() - offset < static_cast<std::size_t>(payload_size) + 4) {
            report.truncated_tail = true;
            break;
        }
        record.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
        offset += payload_size;
        std::uint32_t actual_crc;
        if (!read_u32(bytes, offset, actual_crc)) {
            report.truncated_tail = true;
            break;
        }
        const std::uint32_t expected_crc = acnet::crc32(bytes.data() + record_start, offset - record_start - 4);
        if (actual_crc != expected_crc) {
            if (offset == bytes.size()) {
                report.truncated_tail = true;
                break;
            }
            error = "journal checksum failure at offset " + std::to_string(record_start);
            return false;
        }
        previous_sequence = record.sequence;
        report.last_sequence = record.sequence;
        report.valid_bytes = offset;
        if (record.sequence <= after_sequence) {
            ++report.ignored_before_checkpoint;
        } else if (callback && !callback(record)) {
            error = "journal replay callback rejected sequence " + std::to_string(record.sequence);
            return false;
        } else {
            ++report.applied;
        }
    }
    return true;
}

bool PersistenceStore::scan_last_sequence(std::string& error) {
    std::uint64_t checkpoint_sequence_value = 0;
    const auto checkpoint = load_latest_checkpoint(error);
    if (!error.empty()) return false;
    if (checkpoint.has_value()) checkpoint_sequence_value = checkpoint->sequence;
    ReplayReport report;
    if (!replay(0, {}, report, error)) return false;
    if (report.truncated_tail) {
        std::error_code ec;
        std::filesystem::resize_file(journal_path(), report.valid_bytes, ec);
        if (ec) {
            error = "failed to remove torn journal tail: " + ec.message();
            return false;
        }
        std::FILE* file = std::fopen(journal_path().string().c_str(), "ab");
        if (file == nullptr || !flush_file(file)) {
            if (file != nullptr) std::fclose(file);
            error = "failed to flush recovered journal";
            return false;
        }
        if (std::fclose(file) != 0) {
            error = "failed to close recovered journal";
            return false;
        }
    }
    last_sequence_ = std::max(checkpoint_sequence_value, report.last_sequence);
    return true;
}

bool PersistenceStore::rotate_checkpoints(std::string& error) {
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> checkpoints;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(root_ / "snapshots", ec)) {
        const auto sequence = checkpoint_sequence(item.path());
        if (sequence.has_value()) checkpoints.emplace_back(*sequence, item.path());
    }
    if (ec) {
        error = "failed to enumerate checkpoints: " + ec.message();
        return false;
    }
    std::sort(checkpoints.begin(), checkpoints.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = config_.retained_checkpoints; i < checkpoints.size(); ++i) {
        std::filesystem::remove(checkpoints[i].second, ec);
        if (ec) {
            error = "failed to rotate checkpoint: " + ec.message();
            return false;
        }
    }
    return true;
}

bool PersistenceStore::import_gci(const std::filesystem::path& source, std::string& error) {
    error.clear();
    std::vector<std::uint8_t> bytes;
    if (!read_file(source, config_.maximum_checkpoint_bytes, bytes, error)) return false;
    if (!valid_gci(bytes)) { error = "invalid or unsupported Animal Crossing GCI"; return false; }
    const std::filesystem::path destination = root_ / "town.gci";
    if (std::filesystem::exists(destination)) {
        const std::filesystem::path backup = root_ / "snapshots" / "town.gci.before-import";
        std::vector<std::uint8_t> existing;
        if (!read_file(destination, config_.maximum_checkpoint_bytes, existing, error) ||
            !durable_write(backup, existing, error)) return false;
    }
    return durable_write(destination, bytes, error);
}

bool PersistenceStore::export_gci(const std::filesystem::path& destination, std::string& error) const {
    error.clear();
    std::vector<std::uint8_t> bytes;
    if (!read_file(root_ / "town.gci", config_.maximum_checkpoint_bytes, bytes, error)) return false;
    if (!valid_gci(bytes)) { error = "stored town.gci is invalid"; return false; }
    return durable_write(destination, bytes, error);
}

bool PersistenceStore::load_gci_bytes(std::vector<std::uint8_t>& bytes, std::string& error) const {
    error.clear();
    if (!read_file(root_ / "town.gci", config_.maximum_checkpoint_bytes, bytes, error)) return false;
    if (!valid_gci(bytes)) {
        error = "stored town.gci is invalid";
        return false;
    }
    return true;
}

bool PersistenceStore::export_gci_bytes(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& destination,
                                        std::string& error) const {
    error.clear();
    if (!valid_gci(bytes)) {
        error = "generated town.gci is invalid";
        return false;
    }
    return durable_write(destination, bytes, error);
}

bool PersistenceStore::mark_clean_shutdown(std::string& error) {
    error.clear();
    const std::vector<std::uint8_t> marker{'c', 'l', 'e', 'a', 'n', '\n'};
    return durable_write(root_ / "clean.shutdown", marker, error);
}

} // namespace acserver
