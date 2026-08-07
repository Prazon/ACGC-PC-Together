#pragma once

#include "acserver/town_runtime.hpp"

#include <filesystem>
#include <string>

namespace acserver {

// Loads the dedicated-server INI file. Section headings are accepted for
// operator readability; setting names remain unique across the file so older
// flat config.toml files can also be imported during migration.
bool load_town_config(const std::filesystem::path& path,
                      TownRuntimeConfig& config,
                      bool& invite_required,
                      bool allow_missing,
                      std::string& error);

// Writes a complete, commented host configuration, including config.invite_key
// when one is set so a generated file can start the server unattended.
bool write_default_town_config(const std::filesystem::path& path,
                               const TownRuntimeConfig& config,
                               bool invite_required,
                               std::string& error);

} // namespace acserver
