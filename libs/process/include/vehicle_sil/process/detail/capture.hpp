// SPDX-License-Identifier: MIT
//
// The pieces of run_captured() that the tests reach: one captured stream and, on POSIX, one
// bounded read of a descriptor. Not part of the API.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

namespace vehicle_sil::process::detail {

/// The most reads (of 4096 bytes) one read_available() call makes before it returns, so that a
/// process that writes without a pause cannot keep run_captured() from its time limit.
inline constexpr std::size_t kReadsPerCall = 16;

/// Appends to one captured stream, up to the limit: its beginning (standard output, where a
/// description starts) or its end (standard error, where the reason of a failure is).
struct Sink
{
    std::string& text;
    std::size_t max_bytes;
    bool& truncated;
    bool keep_end = false;

    void append(const char* data, std::size_t size)
    {
        if (keep_end)
        {
            text.append(data, size);
            // Cut only now and then: the text stays below twice the limit until finish().
            if (text.size() > 2 * max_bytes)
            {
                finish();
            }
            return;
        }
        const std::size_t room = text.size() < max_bytes ? max_bytes - text.size() : 0;
        if (size > room)
        {
            truncated = true;
        }
        text.append(data, std::min(size, room));
    }

    void finish()
    {
        if (keep_end && text.size() > max_bytes)
        {
            text.erase(0, text.size() - max_bytes);
            truncated = true;
        }
    }
};

#ifndef _WIN32
/// Reads what the non-blocking descriptor `fd` holds now into `sink`, at most kReadsPerCall
/// times. Returns false once it is closed (end of file or an error: then `fd` is closed and set
/// to -1), true while it is open.
bool read_available(int& fd, Sink& sink);
#endif

} // namespace vehicle_sil::process::detail
