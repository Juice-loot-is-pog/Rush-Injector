#pragma once
#include <thread>

class DiscordPresence {
    std::jthread worker;
public:
    void Start();
    void Stop();
    ~DiscordPresence() { Stop(); }
};
