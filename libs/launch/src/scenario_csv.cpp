// SPDX-License-Identifier: MIT
//
// The points file of a wave: read_csv() splits its text, files_beside() reads it next to the
// scenario file (the only file access of a scenario's check besides the scenario itself).
#include "scenario_csv.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace vehicle_sil::launch {

namespace detail {

namespace {

bool blank(std::string_view line)
{
    return line.find_first_not_of(" \t") == std::string_view::npos;
}

std::string trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return std::string(text.substr(first, last - first + 1));
}

/// The cells of one line, or empty and `problem`.
std::optional<std::vector<std::string>> cells_of(std::string_view line, std::size_t number, std::string& problem)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (true)
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        {
            ++i;
        }
        if (i < line.size() && line[i] == '"')
        {
            std::string cell;
            ++i;
            bool closed = false;
            while (i < line.size())
            {
                if (line[i] == '"')
                {
                    if (i + 1 < line.size() && line[i + 1] == '"')
                    {
                        cell += '"';
                        i += 2;
                        continue;
                    }
                    closed = true;
                    ++i;
                    break;
                }
                cell += line[i++];
            }
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            {
                ++i;
            }
            if (!closed || (i < line.size() && line[i] != ','))
            {
                problem = "has a cell in quotes on line " + std::to_string(number) +
                          (closed ? " with more after its closing quote" : " whose closing quote is not on the line") +
                          ": end a cell in quotes with a quote before the next ',' (write \"\" for a quote in it)";
                return std::nullopt;
            }
            out.push_back(std::move(cell));
        }
        else
        {
            const std::size_t end = std::min(line.find(',', i), line.size());
            out.push_back(trimmed(line.substr(i, end - i)));
            i = end;
        }
        if (i >= line.size())
        {
            return out;
        }
        ++i; // the ','
    }
}

} // namespace

std::optional<CsvTable> read_csv(std::string_view text, std::string& problem)
{
    if (text.substr(0, 2) == "\xFF\xFE" || text.substr(0, 2) == "\xFE\xFF" || text.find('\0') != std::string_view::npos ||
        !vehicle_sil::detail::is_utf8(text))
    {
        problem = "is not UTF-8 text (it looks like UTF-16, or has bytes that are not UTF-8): save it as CSV in UTF-8";
        return std::nullopt;
    }
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
    {
        text.remove_prefix(3);
    }
    CsvTable out;
    std::size_t number = 0;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++number;
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if (blank(line))
        {
            continue;
        }
        auto cells = cells_of(line, number, problem);
        if (!cells)
        {
            return std::nullopt;
        }
        if (out.header_line == 0)
        {
            out.header_line = number;
            out.columns = std::move(*cells);
            continue;
        }
        if (cells->size() != out.columns.size())
        {
            problem = "has " + std::to_string(cells->size()) + (cells->size() == 1 ? " cell" : " cells") + " on line " +
                      std::to_string(number) + ", and its first line names " + std::to_string(out.columns.size()) +
                      (out.columns.size() == 1 ? " column" : " columns") + ": give every line one cell for each column";
            return std::nullopt;
        }
        if (out.rows.size() == kMaxCsvRows)
        {
            problem = "has more than " + std::to_string(kMaxCsvRows) +
                      " lines of values (one point each): keep fewer points, or split it into steps of their own";
            return std::nullopt;
        }
        out.rows.push_back(CsvRow{number, std::move(*cells)});
    }
    if (out.header_line == 0)
    {
        problem = "is empty: its first line names the columns (time,throttle), and each line below it gives a time and the values";
        return std::nullopt;
    }
    if (out.rows.empty())
    {
        problem = "has no lines of values below the names of its columns (line " + std::to_string(out.header_line) +
                  "): give one line for each point (0,50)";
        return std::nullopt;
    }
    return out;
}

std::string shown_cell(const std::string& cell)
{
    constexpr std::size_t kLongest = 20;
    if (cell.size() <= kLongest)
    {
        return "'" + cell + "'";
    }
    std::size_t cut = kLongest;
    // Not in the middle of a character of more than one byte.
    while (cut > 0 && (static_cast<unsigned char>(cell[cut]) & 0xC0) == 0x80)
    {
        --cut;
    }
    return "'" + cell.substr(0, cut) + "...'";
}

} // namespace detail

std::function<std::optional<std::string>(const std::string&, std::string&)> files_beside(const std::filesystem::path& scenario)
{
    return [directory = scenario.parent_path()](const std::string& name, std::string& problem) -> std::optional<std::string> {
        const std::filesystem::path path = directory / process::path_from_utf8(name);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            problem = "does not exist (looked for " + process::utf8_from_path(path) +
                      "): the path is from the scenario file's directory; check it";
            return std::nullopt;
        }
        if (!std::filesystem::is_regular_file(path, ec))
        {
            problem = "is not a regular file: name the CSV file";
            return std::nullopt;
        }
        if (std::filesystem::file_size(path, ec) > detail::kMaxCsvBytes)
        {
            problem = "is larger than " + std::to_string(detail::kMaxCsvBytes >> 20) +
                      " MiB, too large for points: keep fewer points, or split it into steps of their own";
            return std::nullopt;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            problem = "cannot be opened: check its permissions";
            return std::nullopt;
        }
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
}

} // namespace vehicle_sil::launch
