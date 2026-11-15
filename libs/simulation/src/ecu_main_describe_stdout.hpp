// SPDX-License-Identifier: MIT
//
// The standard output of ecu_main() (vehicle_sil/simulation/ecu_main.hpp): --describe's JSON
// and the usage text. Internal to the library; not installed, not part of the API.
#pragma once

#include <cstddef>
#include <string>

namespace vehicle_sil::ecu_main_detail {

/// --describe's stdout. From its construction to the end of the process, fd 1 points at stderr,
/// so what the application writes to stdout (in its constructor, its destructor, a thread of
/// its own, atexit, a library's banner) goes to stderr; it is never pointed back. The JSON goes
/// to a copy of the original stdout descriptor (write()). If stderr is closed, it is first
/// opened on the null device, so that no copy of stdout lands on descriptor 2; if stdout is
/// closed, fd 1 still points at stderr (no file the application opens becomes fd 1) and write()
/// fails. The C and C++ streams are flushed before the switch.
class DescribeStdout
{
public:
    DescribeStdout();
    ~DescribeStdout();
    DescribeStdout(const DescribeStdout&) = delete;
    DescribeStdout& operator=(const DescribeStdout&) = delete;

    /// Writes `text` to the original stdout. false if it is closed or the write failed.
    bool write(const std::string& text);

private:
    static constexpr int kStdoutFd = 1;
    static constexpr int kStderrFd = 2;
    int saved_ = -1;
};

/// Writes `text` to stdout as it is: on Windows without turning "\n" into "\r\n", so that the
/// JSON has the same bytes everywhere. false if the write failed.
bool write_stdout(const std::string& text);

/// write() of POSIX, _write() of Windows (at most 1 GiB at a time): the bytes written, or -1.
long write_fd(int fd, const char* data, std::size_t size);

} // namespace vehicle_sil::ecu_main_detail
