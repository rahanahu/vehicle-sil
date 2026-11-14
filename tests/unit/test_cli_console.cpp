// SPDX-License-Identifier: MIT
//
// Tests for the output helper shared by the executables (tools/common): what goes to a stream
// that is not a console (a file, a pipe) is the UTF-8 text, byte for byte. The way to a Windows
// console (UTF-16, whatever the code page) needs a console and has no test here.
#include "test_harness.hpp"

#include "vehicle_sil/cli/console.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

namespace {

namespace cli = vehicle_sil::cli;

/// A file of its own, opened for writing and reading as bytes; removed at the end.
struct Scratch
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("vehicle-sil-cli-console-" + std::to_string(std::random_device{}()) + ".txt");
    std::FILE* file = nullptr;

    Scratch()
    {
#ifdef _WIN32
        file = _wfopen(path.c_str(), L"w+b");
#else
        file = std::fopen(path.c_str(), "w+b");
#endif
    }
    ~Scratch()
    {
        if (file != nullptr)
        {
            std::fclose(file);
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    /// What the file holds.
    std::string text() const
    {
        std::string out;
        std::rewind(file);
        char buffer[256];
        for (std::size_t got; (got = std::fread(buffer, 1, sizeof buffer, file)) > 0;)
        {
            out.append(buffer, got);
        }
        std::fseek(file, 0, SEEK_END);
        return out;
    }
};

// "kairo 2" (line 2) in Japanese, as UTF-8: the name of a network connection on a Japanese Windows.
const std::string kName = "\xE5\x9B\x9E\xE7\xB7\x9A 2";

} // namespace

TEST_CASE(cli_write_text_writes_the_utf8_bytes_to_a_file_after_what_the_stream_held)
{
    Scratch scratch;
    CHECK(scratch.file != nullptr);
    if (scratch.file == nullptr)
    {
        return;
    }
    std::fputs("held, ", scratch.file); // not flushed
    CHECK(cli::write_text(scratch.file, "--interface " + kName + "\n"));
    CHECK(cli::write_text(scratch.file, ""));
    CHECK(scratch.text() == "held, --interface " + kName + "\n");
}

TEST_CASE(cli_print_formats_as_printf_does)
{
    Scratch scratch;
    CHECK(scratch.file != nullptr);
    if (scratch.file == nullptr)
    {
        return;
    }
    cli::print(scratch.file, "error: no interface '%s' (%d of %s)\n", kName.c_str(), 2, "3");
    cli::print(scratch.file, "%s", "");
    const std::string longer(5000, 'x'); // longer than any buffer on the stack
    cli::print(scratch.file, "%s|%5.1f\n", longer.c_str(), 2.5);
    CHECK(scratch.text() == "error: no interface '" + kName + "' (2 of 3)\n" + longer + "|  2.5\n");
}
