#include <iostream>
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <windows.h>
#include <thread>

#include "protocol.pb.h"
#include "PacketDispatcher.h"

// gRPC 관련 헤더
#include <grpcpp/grpcpp.h>
#include "protocol_grpc.grpc.pb.h"

#include "../Common/MemoryPool.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

using boost::asio::ip::tcp;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using Protocol::BuffMonsterHpAdminReq;
using Protocol::BuffMonsterHpAdminRes;
using Protocol::AdminAPI;

// ==========================================
// S2S(Server-to-Server) 통신 세션
// ==========================================
class ServerSession;
PacketDispatcher<ServerSession> g_s2s_dispatcher;

// ★ [추가] 연결된 하위 서버(GameServer, LoginServer) 세션들을 관리할 전역 리스트
std::vector<std::shared_ptr<ServerSession>> g_serverSessions;
std::mutex g_serverSessionMutex;

class ServerSession : public std::enable_shared_from_this<ServerSession> {
private:
    tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    ServerSession(tcp::socket socket) noexcept : socket_(std::move(socket)) {}

    void start() {
        ReadHeader();
    }

    void Send(uint16_t pktId, const google::protobuf::Message& msg) {
        // (안전 장치: 소켓이 닫혀있으면 무시)
        if (!socket_.is_open()) return;

        std::string payload;
        msg.SerializeToString(&payload);
        PacketHeader header;
        header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
        header.id = pktId;

        // =========================================================
        // ★ 1. 풀에서 버퍼 대여 (new 없음)
        SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();

        // ★ 2. shared_ptr에 '커스텀 딜리터'를 달아서 포장
        // 이렇게 하면 send_buf가 소멸될 때 메모리 해제(delete) 대신 Pool.Release()가 자동 호출됩니다!
        std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());
        // =========================================================

        // 3. 빌려온 버퍼에 데이터 복사
        memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
        memcpy(send_buf->buffer_.data() + sizeof(PacketHeader), payload.data(), payload.size());

        // 4. 비동기 전송
        auto self(shared_from_this());
        // 주의: buffer_.data()에서 딱 header.size 만큼만 잘라서 보냅니다.
        boost::asio::async_write(socket_, boost::asio::buffer(send_buf->buffer_.data(), header.size),
            [this, self, send_buf](boost::system::error_code ec, std::size_t) {
                // 이 콜백 함수가 끝나는 순간 send_buf의 수명이 다하면서
                // 우리가 만든 SendBufferDeleter가 작동하여 버퍼가 큐로 쏙! 반납됩니다.
                if (ec) {
                    // 에러 로깅 (필요 시)
                    std::cerr << "[WorldServer] S2S 패킷 전송 실패\n";
                }
            });
    }

private:
    void ReadHeader() {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    if (header_.size < sizeof(PacketHeader) || header_.size > 4096) return;
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));

                    if (payload_size == 0) {
                        auto session_ptr = self;
                        g_s2s_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                        ReadHeader();
                    }
                    else {
                        payload_buf_.resize(payload_size);
                        ReadPayload(payload_size);
                    }
                }
                else {
                    std::cout << "[WorldServer] 연동된 서버(LoginServer 등)와의 연결이 해제되었습니다.\n";
                }
            });
    }

    void ReadPayload(uint16_t payload_size) {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
            [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    auto session_ptr = self;
                    g_s2s_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                    ReadHeader();
                }
            });
    }
};

// ==========================================
// 핸들러: LoginServer가 보낸 월드 선택 요청(1010) 처리
// ==========================================
void Handle_WorldLoginSelectReq(std::shared_ptr<ServerSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::LoginWorldSelectReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        std::cout << "[WorldServer] LoginServer로부터 유저(" << req.account_id() << ")의 월드 " << req.world_id() << "번 입장 요청 수신.\n";

        // 응답 패킷 세팅
        Protocol::WorldLoginSelectRes res;
        res.set_account_id(req.account_id());
        res.set_success(true);
        res.set_gateway_ip("127.0.0.1"); // 해당 월드를 담당하는 Gateway IP
        res.set_gateway_port(8888);      // Gateway Port

        // 월드서버가 직접 고유한 접속 토큰 발급
        std::string new_token = "WORLD_" + std::to_string(req.world_id()) + "_TOKEN_" + req.account_id();
        res.set_session_token(new_token);

        session->Send(Protocol::PKT_WORLD_LOGIN_SELECT_RES, res);
        std::cout << "[WorldServer] 유저(" << req.account_id() << ") 토큰 발급 및 LoginServer로 응답 완료.\n";
    }
}

// ==========================================
// WorldServer 대기열
// ==========================================
class WorldServer {
private:
    tcp::acceptor acceptor_;

public:
    WorldServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    try {
                        // ★ 접속한 상대방의 IP와 할당된 임시 포트(Ephemeral Port)를 출력합니다.
                        auto remote_ep = socket.remote_endpoint();
                        std::cout << "[WorldServer] S2S 통신: 새로운 서버 접속 확인! " << "[연결처: " << remote_ep.address().to_string() << ":" << remote_ep.port() << "]\n";
                    }
                    catch (std::exception&) {
                        std::cout << "[WorldServer] S2S 통신: 서버 접속 확인! (엔드포인트 읽기 실패)\n";
                    }
                    
                    // ★ 세션을 생성하고 리스트에 추가한 뒤 시작시킵니다!
                    auto new_session = std::make_shared<ServerSession>(std::move(socket));
                    {
                        std::lock_guard<std::mutex> lock(g_serverSessionMutex);
                        g_serverSessions.push_back(new_session);
                    }
                    new_session->start();
                }
                do_accept();
            });
    }
};


constexpr uint64_t MIN_MON_ID = 10000;
constexpr uint64_t MAX_MON_ID = 100000;

// =========================================================
// ★ [추가] gRPC API 서비스 구현체
// 운영 툴에서 /monster_hp API를 쏘면 이 안의 코드가 실행됩니다!
// =========================================================
class AdminServiceImpl final : public AdminAPI::Service {
    Status BuffMonsterHp(ServerContext* context, const BuffMonsterHpAdminReq* request, BuffMonsterHpAdminRes* reply) override {
        int32_t add_hp = request->add_hp();
        std::cout << "\n[Admin API] 🚨 외부 운영툴로부터 몬스터 체력 버프(+" << add_hp << ") 요청 수신!\n";

        // ★ 입력값 유효성 검사 (0 이하의 값이 들어오면 즉시 차단!)
        if (add_hp <= 0) {
            std::cerr << "\n[Admin API] ❌ 잘못된 버프 요청 차단 (요청된 add_hp: " << add_hp << ")\n";

            // gRPC 표준 에러 코드(INVALID_ARGUMENT)와 함께 Postman으로 에러를 튕겨냅니다.
            return Status(grpc::StatusCode::INVALID_ARGUMENT, "add_hp must be strictly greater than 0");
        }

        // 1. GameServer에게 보낼 S2S 패킷 세팅
        Protocol::WorldGameMonsterBuffReq buff_req;
        buff_req.set_min_uid(MIN_MON_ID);
        buff_req.set_max_uid(MAX_MON_ID);
        buff_req.set_add_hp(add_hp);

        // 2. ★ [수정] 연결된 서버 패킷 전송
        int send_count = 0;
        {
            std::lock_guard<std::mutex> lock(g_serverSessionMutex);
            for (auto& session : g_serverSessions) {
                if (session) {
                    session->Send(Protocol::PKT_WORLD_GAME_MONSTER_BUFF, buff_req);
                    send_count++;
                }
            }
        }
        std::cout << "▶ [WorldServer] 연결된 GameServer로 몬스터 버프 지시 패킷 전송 완료!\n";

        // 4. API 요청자(Postman)에게 성공 응답 반환
        reply->set_success(true);
        reply->set_message("GameServer로 n번 몬스터 체력 버프 명령을 성공적으로 전달했습니다.");

        return Status::OK;
    }
};

// ★ [추가] gRPC 서버를 실행하고 대기하는 함수
void RunGrpcServer() {
    std::string server_address("0.0.0.0:50051");
    AdminServiceImpl service;

    ServerBuilder builder;
    // 인증(SSL/TLS) 없이 평문으로 포트를 엽니다 (개발용)
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "🌐 [WorldServer] Admin gRPC 서버 가동 시작 (Port: 50051)\n";

    // 이 함수는 서버가 꺼질 때까지 여기서 무한 대기(블로킹)합니다.
    server->Wait();
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // 디스패처(Dispatcher)에 1010 패킷 핸들러 등록
    g_s2s_dispatcher.RegisterHandler(Protocol::PKT_LOGIN_WORLD_SELECT_REQ, Handle_WorldLoginSelectReq);

    try {
        boost::asio::io_context io_context;
        WorldServer server(io_context, 7000);
        std::cout << "[WorldServer] 월드 중앙 서버 가동 시작 (Port: 7000) Created by Jeong Shin Young\n";
        std::cout << "=================================================\n";

        // =========================================================
        // 메인 스레드가 블로킹 되기 전에 gRPC 서버 스레드를 띄웁니다!
        // =========================================================
        std::thread grpc_thread(RunGrpcServer);
        grpc_thread.detach(); // 백그라운드에서 알아서 돌도록 메인 스레드와 분리

        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "[Error] 월드 서버 예외 발생: " << e.what() << "\n";
    }
    return 0;
}