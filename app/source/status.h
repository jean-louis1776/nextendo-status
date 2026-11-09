#pragma once

#include <string>
#include <vector>

struct PublicGame {
    std::string name;
    std::string titleId;
    int players = 0;
    bool online = false;
    bool nostatus = false;
    bool rooms = false;
    std::string jpeg;
};

class PublicStatus {
public:
    void start();
    void stop();
    void hold(bool on);
    bool ready() const;
    bool failed() const;
    int revision() const;
    std::vector<PublicGame> snapshot() const;

private:
    struct State;
    State* state_ = nullptr;
};
