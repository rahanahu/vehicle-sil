// SPDX-License-Identifier: MIT
//
// The CSV file of a wave's points (`points: { file: <path>, value: <column> }`): its text read
// into lines of cells. What a cell means (a time, a value of the variable) is decided by the
// scenario's reader (scenario.cpp); this only splits the text. Internal.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch::detail {

/// The most lines of values a points file may have (one point each).
inline constexpr std::size_t kMaxCsvRows = 100'000;
/// The largest points file, in bytes.
inline constexpr std::uintmax_t kMaxCsvBytes = std::uintmax_t{16} << 20;

struct CsvRow
{
    std::size_t line = 0; ///< in the file, from 1 (blank lines count)
    std::vector<std::string> cells;
};

struct CsvTable
{
    std::size_t header_line = 0;
    /// The names of the columns (the first line that is not blank), without the spaces around them.
    std::vector<std::string> columns;
    /// The lines below it that are not blank, each with as many cells as there are columns.
    std::vector<CsvRow> rows;
};

/// `text` (UTF-8, with or without a byte order mark; lines end in LF or CRLF; cells separated by
/// ',' and in double quotes if they hold one, "" for a quote in quotes) as a table, or empty and
/// `problem`: what is wrong and how to fix it, written to follow the file's name ("is empty: ...").
[[nodiscard]] std::optional<CsvTable> read_csv(std::string_view text, std::string& problem);

/// A cell as a message shows it: in quotes, and cut after 20 characters ("'12.5x'", "'abcdefghijklmnopqrst...'"),
/// so that a message says which cell without repeating the file.
[[nodiscard]] std::string shown_cell(const std::string& cell);

} // namespace vehicle_sil::launch::detail
