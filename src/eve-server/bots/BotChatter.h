#ifndef EVE_SERVER_BOTS_BOTCHATTER_H
#define EVE_SERVER_BOTS_BOTCHATTER_H

#include "eve-server.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PlayerBot {

struct ChatterReply {
    uint32 characterID = 0;
    std::string text;
};

/**
 * Optional voice layer for PlayerBots.
 *
 * The worker only exchanges text with an OpenAI-compatible HTTP endpoint.  It
 * never touches Client, inventory, destiny, or database objects; replies are
 * drained by BotManager on the normal world thread before they are sent via
 * Client::SelfChatMessage().
 */
class BotChatter {
public:
    BotChatter();
    ~BotChatter();

    void Initialize();
    void Shutdown();
    bool Queue(uint32 characterID, const std::string& name,
               const std::string& personality, const std::string& message);
    std::vector<ChatterReply> Drain();
    bool Enabled() const { return m_enabled; }

private:
    struct Job {
        uint32 characterID = 0;
        std::string name;
        std::string personality;
        std::string message;
    };

    void Worker();
    bool Request(const Job& job, std::string& response) const;
    static std::string EscapeJson(const std::string& value);
    static std::string ExtractContent(const std::string& body);
    static std::string Fallback(const Job& job);

    bool m_enabled;
    bool m_stop;
    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<Job> m_jobs;
    std::deque<ChatterReply> m_replies;
    std::map<uint32, uint64_t> m_nextAllowed;
};

} // namespace PlayerBot

#endif
