#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <deque>
#include <utility>
#include <mutex>
#include <unordered_map>
#include <chrono>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "PacketDispatcher.h"
#include "../Common/Network/PacketAssembler.h"
#include "../Common/Define/SecurityConstants.h"
#include "../Common/Utils/Lock.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class ServerSession;

extern PacketDispatcher<ServerSession> g_s2s_dispatcher;
extern std::vector<std::shared_ptr<ServerSession>> g_serverSessions;
extern UTILITY::Lock g_serverSessionMutex;

// ==========================================
// 세션 토큰 저장소
//
// WorldServer가 발급한 토큰을 보관하고,
// 연결된 GameServer를 통해 GatewayServer에 전달합니다.
// 토큰 만료 시각(expire_ms)을 통해 오래된 토큰을 자동 정리합니다.
//
// 하드코딩된 TOKEN_LIFETIME_MS를 SecurityConstants로 교체
// ==========================================
struct TokenEntry {
    std::string account_id;
    std::string token;
    int64_t expire_ms;
};

class TokenStore {
private:
    std::unordered_map<std::string, TokenEntry> tokens_; // key: account_id
    UTILITY::Lock mutex_;

public:
    // 토큰 발급 및 저장
    void StoreToken(const std::string& account_id, const std::string& token) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        UTILITY::LockGuard lock(mutex_);
        tokens_[account_id] = { account_id, token,
            now_ms + SecurityConstants::Token::TOKEN_LIFETIME_MS };
    }

    // 만료된 토큰 정리 (주기적 호출)
    void CleanExpired() {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        UTILITY::LockGuard lock(mutex_);
        for (auto it = tokens_.begin(); it != tokens_.end(); ) {
            if (it->second.expire_ms < now_ms) {
                it = tokens_.erase(it);
            } else {
                ++it;
            }
        }
    }

    int64_t GetExpireTime(const std::string& account_id) {
        UTILITY::LockGuard lock(mutex_);
        auto it = tokens_.find(account_id);
        if (it != tokens_.end()) return it->second.expire_ms;
        return 0;
    }
};

extern TokenStore g_tokenStore;

// ==========================================
// ServerSession: strand + send_queue + PacketAssembler 적용
//
// std::vector<char> payload_buf_ 제거 -> PacketAssembler로 교체
// ==========================================
class ServerSession : public std::enable_shared_from_this<ServerSession> {
private:
    boost::asio::ip::tcp::socket socket_;

    // strand + send_queue: concurrent async_write 방지
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<struct SendBuffer>, size_t>> send_queue_;

    PacketHeader header_;

    // std::vector<char> payload_buf_ 제거 -> PacketAssembler로 교체
    PacketAssembler assembler_;

public:
    ServerSession(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
    void OnDisconnected();
    void DoWrite();  // 큐에서 패킷을 꺼내 실제로 전송하는 내부 함수
};
