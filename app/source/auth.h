#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct LiveChip {
    std::string name;
    std::string mark;
    std::string extra;
};

struct LiveLobby {
    std::string mode;
    std::string state;
    std::string places;
    std::vector<LiveChip> members;
};

struct LivePage {
    std::string headline;
    std::string note;
    std::vector<LiveLobby> lobbies;
    std::vector<LiveChip> players;
};

LivePage describeLive(const std::string& body, const std::string& titleId, int onlineCount);

// Sign-in shows a code and a QR. The phone opens the site and finishes there.
class NextendoAuth {
public:
    void beginPair();
    void cancelPair();
    bool takeTokens(std::string& access, std::string& refresh);
    std::string statusText() const;

    const std::string& pairCode() const { return pairCode_; }
    const std::string& pairUrl() const { return pairUrl_; }
    const std::string& accessToken() const { return accessToken_; }
    const std::string& refreshToken() const { return refreshToken_; }
    const std::string& failReason() const { return failReason_; }
    const std::string& displayName() const { return displayName_; }
    const std::string& friendCode() const { return friendCode_; }
    const std::string& avatarJpeg() const { return avatarJpeg_; }

    void useTokens(const std::string& access, const std::string& refresh);
    bool loadProfile(const std::string& token);
    bool fetchLive(const std::string& token, std::string& body, std::string& error, const std::atomic<bool>& stop);

private:
    bool generatePkce();
    bool exchangeCode(const std::string& code);
    bool refresh();
    bool readProfile(const std::string& token);
    void runPair();

    std::string pairCode_;
    std::string pairUrl_;
    std::thread pairThread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ready_{false};
    mutable std::mutex mu_;
    std::string verifier_;
    std::string challenge_;
    std::string state_;
    std::string accessToken_;
    std::string refreshToken_;
    std::string failReason_;
    std::string displayName_;
    std::string friendCode_;
    std::string avatarJpeg_;
};
