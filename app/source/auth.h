#pragma once

#include <atomic>
#include <string>

enum class AuthPhase { Idle, Waiting, Ok, Failed };

class NextendoAuth {
public:
    NextendoAuth();
    ~NextendoAuth();

    bool generatePkce();
    const std::string& codeVerifier() const { return verifier_; }
    const std::string& codeChallenge() const { return challenge_; }
    std::string authorizeUrl() const;

    const std::string& accessToken() const { return accessToken_; }
    const std::string& refreshToken() const { return refreshToken_; }
    const std::string& failReason() const { return failReason_; }
    const std::string& lanUrl() const { return lanUrl_; }
    const std::string& pairCode() const { return pairCode_; }
    const std::string& displayName() const { return displayName_; }
    const std::string& avatarJpeg() const { return avatarJpeg_; }
    bool profileReady() const { return profileReady_.load(); }
    bool relayDown() const { return relayDown_.load(); }

    void loadProfile(const std::string& token, std::atomic<bool>* stop = nullptr);

    // Opens http://<ip-консоли>:47823/ and waits for the OAuth redirect.
    bool startLanLogin();
    AuthPhase poll();
    void cancelLan();

    bool exchangeCode(const std::string& code, std::string& responseBody);

private:
    void serve();
    void joinThread();

    std::string verifier_;
    std::string challenge_;
    std::string accessToken_;
    std::string refreshToken_;
    std::string failReason_;
    std::string lanUrl_;
    std::string pairCode_;
    std::string state_;
    std::string displayName_;
    std::string avatarJpeg_;
    std::atomic<bool> profileReady_{false};
    std::atomic<bool> relayDown_{false};

    struct Lan;
    Lan* lan_ = nullptr;
};
