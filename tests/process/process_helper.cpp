// SPDX-License-Identifier: MIT
//
// A child for the tests of vehicle_sil::process and vehicle_sil_launch. It does what its
// arguments say, in this order, and ignores the arguments it does not know (so that a launcher
// may add its own):
//   --out <text>         writes <text> to stdout
//   --err <text>         writes <text> to stderr
//   --out-file <path>    writes the content of <path> to stdout
//   --fill-out <n>       writes <n> bytes 'o' to stdout
//   --fill-err <n>       writes <n> bytes 'e' to stderr
//   --echo-args          writes each of its arguments on a line of its own to stdout
//   --read-stdin         reads stdin to its end and writes "stdin <bytes>" to stdout
//   --sleep-ms <ms>      sleeps
//   --flood              writes to stdout without end
//   --abort              ends with std::abort() (POSIX: the signal SIGABRT)
//   --exit <code>        the exit code (default 0)
// On Windows it reads its arguments as UTF-8 from the wide command line and writes "\n" as it is.
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

std::vector<std::string> arguments(int argc, char** argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> out;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 0; wide != nullptr && i < count; ++i)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
        if (size > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        }
        out.push_back(std::move(text));
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> args = arguments(argc, argv);
    const std::size_t count = args.size();
    int code = 0;
    for (std::size_t i = 1; i < count; ++i)
    {
        const std::string& arg = args[i];
        const bool hasValue = i + 1 < count;
        if (arg == "--out" && hasValue)
        {
            std::fputs(args[++i].c_str(), stdout);
        }
        else if (arg == "--err" && hasValue)
        {
            std::fputs(args[++i].c_str(), stderr);
        }
        else if (arg == "--out-file" && hasValue)
        {
            std::ifstream in(args[++i], std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::fwrite(text.data(), 1, text.size(), stdout);
        }
        else if ((arg == "--fill-out" || arg == "--fill-err") && hasValue)
        {
            const std::string text(std::stoul(args[++i]), arg == "--fill-out" ? 'o' : 'e');
            std::fwrite(text.data(), 1, text.size(), arg == "--fill-out" ? stdout : stderr);
        }
        else if (arg == "--echo-args")
        {
            for (std::size_t j = 1; j < count; ++j)
            {
                std::printf("%s\n", args[j].c_str());
            }
        }
        else if (arg == "--read-stdin")
        {
            std::size_t bytes = 0;
            while (std::fgetc(stdin) != EOF)
            {
                ++bytes;
            }
            std::printf("stdin %zu\n", bytes);
        }
        else if (arg == "--sleep-ms" && hasValue)
        {
            std::fflush(stdout);
            std::fflush(stderr);
            std::this_thread::sleep_for(std::chrono::milliseconds(std::stol(args[++i])));
        }
        else if (arg == "--flood")
        {
            const std::string block(65536, 'f');
            while (std::fwrite(block.data(), 1, block.size(), stdout) == block.size())
            {
            }
        }
        else if (arg == "--abort")
        {
            std::fflush(stdout);
            std::abort();
        }
        else if (arg == "--exit" && hasValue)
        {
            code = std::stoi(args[++i]);
        }
    }
    std::fflush(stdout);
    return code;
}
