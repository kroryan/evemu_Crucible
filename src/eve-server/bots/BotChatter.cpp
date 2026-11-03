#include "eve-server.h"

#include "bots/BotChatter.h"
#include "EVEServerConfig.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace PlayerBot {

namespace {

const uint64_t kChatCooldown = 30ULL * 10000000ULL;
const size_t kMaxQueued = 32;
const size_t kMaxResponse = 64 * 1024;

#ifndef _WIN32
void CloseSocket(int socket)
{
    if (socket >= 0)
        close(socket);
}
#endif

bool ParseEndpoint(const std::string& endpoint, std::string& host,
                   std::string& port, std::string& path)
{
    const std::string scheme = "http://";
    if (endpoint.compare(0, scheme.size(), scheme) != 0)
        return false; // HTTPS requires a TLS library; use the deterministic fallback.

    const size_t authorityStart = scheme.size();
    const size_t pathStart = endpoint.find('/', authorityStart);
    const std::string authority = endpoint.substr(
        authorityStart, pathStart == std::string::npos ? std::string::npos : pathStart - authorityStart);
    if (authority.empty())
        return false;

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(':') == colon) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    } else {
        host = authority;
        port = "80";
    }
    if (host.empty() || port.empty())
        return false;

    path = pathStart == std::string::npos ? "/v1" : endpoint.substr(pathStart);
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (path.size() < 16 || path.compare(path.size() - 16, 16, "/chat/completions") != 0)
        path += "/chat/completions";
    return true;
}

} // namespace

BotChatter::BotChatter() : m_enabled(false), m_stop(false) {}

BotChatter::~BotChatter()
{
    Shutdown();
}

void BotChatter::Initialize()
{
    Shutdown();
    m_enabled = sConfig.playerbots.llm.enabled &&
                !sConfig.playerbots.llm.endpoint.empty() &&
                !sConfig.playerbots.llm.model.empty();
    if (!m_enabled) {
        if (sConfig.playerbots.llm.enabled)
            sLog.Warning("PlayerBot", "LLM chatter enabled without endpoint/model; deterministic replies remain active.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = false;
    }
    m_worker = std::thread(&BotChatter::Worker, this);
    sLog.Green("PlayerBot", "Asynchronous LLM chatter enabled at %s.", sConfig.playerbots.llm.endpoint.c_str());
}

void BotChatter::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_condition.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.clear();
    m_replies.clear();
    m_nextAllowed.clear();
    m_enabled = false;
}

bool BotChatter::Queue(uint32 characterID, const std::string& name,
                       const std::string& personality, const std::string& message)
{
    if (!m_enabled || characterID == 0 || message.empty())
        return false;

    const uint64_t now = GetFileTimeNow();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stop || m_jobs.size() >= kMaxQueued)
        return false;
    const std::map<uint32, uint64_t>::const_iterator next = m_nextAllowed.find(characterID);
    if (next != m_nextAllowed.end() && next->second > now)
        return false;
    m_nextAllowed[characterID] = now + kChatCooldown;

    Job job;
    job.characterID = characterID;
    job.name = name;
    job.personality = personality;
    job.message = message.substr(0, 2000);
    m_jobs.push_back(std::move(job));
    m_condition.notify_one();
    return true;
}

std::vector<ChatterReply> BotChatter::Drain()
{
    std::vector<ChatterReply> replies;
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_replies.empty()) {
        replies.push_back(std::move(m_replies.front()));
        m_replies.pop_front();
    }
    return replies;
}

void BotChatter::Worker()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop && m_jobs.empty())
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }

        std::string response;
        if (!Request(job, response) || response.empty())
            response = Fallback(job);
        if (response.empty())
            continue;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stop && m_replies.size() < kMaxQueued)
            m_replies.push_back({job.characterID, response});
    }
}

bool BotChatter::Request(const Job& job, std::string& response) const
{
#ifdef _WIN32
    (void)job;
    (void)response;
    return false;
#else
    std::string host, port, path;
    if (!ParseEndpoint(sConfig.playerbots.llm.endpoint, host, port, path))
        return false;

    std::ostringstream body;
    body << "{\"model\":\"" << EscapeJson(sConfig.playerbots.llm.model)
         << "\",\"messages\":[{\"role\":\"system\",\"content\":\""
         << EscapeJson("You are " + job.name + " in EVE Online. Personality: " + job.personality +
                       ". Answer briefly in character. Never claim to have performed an action.")
         << "\"},{\"role\":\"user\",\"content\":\"" << EscapeJson(job.message)
         << "\"}],\"temperature\":0.7,\"max_tokens\":120}";
    const std::string payload = body.str();

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0)
        return false;

    int socketFD = -1;
    for (struct addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        socketFD = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketFD < 0)
            continue;
        struct timeval timeout;
        timeout.tv_sec = 4;
        timeout.tv_usec = 0;
        setsockopt(socketFD, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socketFD, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(socketFD, address->ai_addr, address->ai_addrlen) == 0)
            break;
        CloseSocket(socketFD);
        socketFD = -1;
    }
    freeaddrinfo(addresses);
    if (socketFD < 0)
        return false;

    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\nHost: " << host
            << "\r\nContent-Type: application/json\r\nConnection: close\r\n";
    if (!sConfig.playerbots.llm.apiKey.empty())
        request << "Authorization: Bearer " << sConfig.playerbots.llm.apiKey << "\r\n";
    request << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
    const std::string requestData = request.str();

    size_t sent = 0;
    while (sent < requestData.size()) {
        const ssize_t result = send(socketFD, requestData.data() + sent,
                                    requestData.size() - sent, 0);
        if (result <= 0) {
            CloseSocket(socketFD);
            return false;
        }
        sent += static_cast<size_t>(result);
    }

    std::string raw;
    char buffer[4096];
    for (;;) {
        const ssize_t received = recv(socketFD, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;
        raw.append(buffer, static_cast<size_t>(received));
        if (raw.size() > kMaxResponse)
            break;
    }
    CloseSocket(socketFD);
    const size_t bodyStart = raw.find("\r\n\r\n");
    if (bodyStart == std::string::npos)
        return false;
    response = ExtractContent(raw.substr(bodyStart + 4));
    return !response.empty();
#endif
}

std::string BotChatter::EscapeJson(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char control[7];
                    snprintf(control, sizeof(control), "\\u%04x", ch);
                    escaped += control;
                } else {
                    escaped += static_cast<char>(ch);
                }
        }
    }
    return escaped;
}

std::string BotChatter::ExtractContent(const std::string& body)
{
    const std::string key = "\"content\"";
    const size_t keyPos = body.find(key);
    if (keyPos == std::string::npos)
        return "";
    size_t pos = body.find(':', keyPos + key.size());
    if (pos == std::string::npos)
        return "";
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])))
        ++pos;
    if (pos >= body.size() || body[pos] != '"')
        return "";
    ++pos;

    std::string value;
    bool escaped = false;
    for (; pos < body.size(); ++pos) {
        const char ch = body[pos];
        if (escaped) {
            switch (ch) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            break;
        } else {
            value.push_back(ch);
        }
    }
    return value;
}

std::string BotChatter::Fallback(const Job& job)
{
    static const char* replies[] = {
        "Copy that. I am keeping the operation moving.",
        "Understood. I will report again after the next waypoint.",
        "o7. My ship and wallet are following the normal rules."
    };
    const size_t index = (static_cast<size_t>(job.characterID) + job.message.size()) %
                         (sizeof(replies) / sizeof(replies[0]));
    return replies[index];
}

} // namespace PlayerBot
