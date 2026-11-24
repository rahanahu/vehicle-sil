// SPDX-License-Identifier: MIT
#include "test_harness.hpp"

#include "usecases/sha256.hpp"

#include <string>

using namespace vehicle_sil::launch::detail;

TEST_CASE(sha256_hex_matches_fips_180_4_and_boundary_vectors)
{
    // Empty input
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // NIST "abc"
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // NIST 56-byte two-block padding boundary
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // 55-byte single-block boundary and 64-byte exact block boundary
    CHECK(sha256_hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(sha256_hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    // Non-ASCII bytes >= 0x80 (verifies unsigned byte cast without sign extension)
    CHECK(sha256_hex("\x80\xff\xc3\xa9") == "f3614fd97f11371afb0a57b0766597bba5fb304f74bd88054c401cd8ab26c8ca");
    // Multi-block > 1 KiB input
    std::string large;
    large.reserve(1500);
    for (int i = 0; i < 150; ++i)
    {
        large += "VehicleSIL";
    }
    CHECK(sha256_hex(large) == "b7c730226e6d8a55e8131fd4d1b0b30ae91b65d9b9d6cceb7495576817924d14");
}
