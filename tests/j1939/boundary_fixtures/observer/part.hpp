// SPDX-License-Identifier: MIT
// Fixture of the parts' boundary check (not compiled): a part does not watch the network as its
// Observer (the one observer slot is for tests and recording).
#pragma once

class Part
{
public:
    explicit Part(Network& network) { network.set_observer(&watcher_); }
};
