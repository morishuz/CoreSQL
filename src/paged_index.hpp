#pragma once
#include "state.hpp"
namespace coresql::detail {
// Optional native INTEGER primary-key images. Other providers keep their normal
// rebuild path; cache files are disposable and never authoritative storage.
void rebuild_paged_indexes(State&, const Registry&, const std::filesystem::path&, std::uint64_t fingerprint);
void page_primary_index(Table&, const Registry&);
} // namespace coresql::detail
