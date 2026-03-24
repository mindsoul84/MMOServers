#include "../WorldServer/WorldServer.h"
#include "AdminAPI/AdminService.h"
#include "Handlers/WorldHandlers.h"
#include "../Common/MemoryPool.h"
#include "../Common/ConfigManager.h"
#include "../Common/Utils/NetworkErrorHandler.h"

#include <iostream>
#include <windows.h>
#include <thread>

using boost::asio::ip::tcp;

// ==========================================
// 전역 실제 메모리 할당 (정의부)
// ==========================================
PacketDispatcher<ServerSession> g_s2s_dispatcher;
std::vector<std::shared_ptr<ServerSession>> g_serverSessions;
std::mutex g_serverSessionMutex;

// ==========================================
// ServerSession 클래스 구현부
// ==========================================
ServerSession::ServerSession(tcp::socket socket) noexcept : socket_(std::move(socket)) {}

void ServerSession::start() {
    ReadHeader();
}

void ServerSession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) {
        std::cerr << "[WorldServer] ⚠️ Send 시도했으나 소켓이 이미 닫혀있음 (PktID: " << pktId << ")\n";
        return;
    }

    std::string payload;
    msg.SerializeToString(&payload);
    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->buffer_.data() + sizeof(PacketHeader), payload.data(), payload.size());

    auto self(shared_from_this());
    boost::asio::async_write(socket_, boost::asio::buffer(send_buf->buffer_.data(), header.size),
        [this, self, send_buf, pktId](boost::system::error_code ec, std::size_t bytes_sent) {
            if (ec) {
                auto result = NetworkUtils::HandleError(
                    "ServerSession::Send(PktID:" + std::to_string(pktId) + ")", 
                    ec
                );
                if (result.should_disconnect) {
                    // 세션 정리는 ReadHeader에서 수행
                    std::cerr << "[WorldServer] S2S 패킷 전송 실패로 연결 종료 예정\n";
                }
            }
        });
}

void ServerSession::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) {
                    std::cerr << "[WorldServer] ⚠️ 잘못된 패킷 헤더 크기: " << header_.size << "\n";
                    return;
                }
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
                auto result = NetworkUtils::HandleError("ServerSession::ReadHeader", ec);
                if (result.should_disconnect || 
                    NetworkUtils::ClassifyError(ec) != NetworkUtils::ErrorSeverity::IGNORED_ERROR) {
                    std::cout << "[WorldServer] 연동된 서버와의 연결이 해제되었습니다.\n";
                    OnDisconnected();
                }
            }
        });
}

void ServerSession::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                g_s2s_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else {
                auto result = NetworkUtils::HandleError("ServerSession::ReadPayload", ec);
                if (result.should_disconnect || 
                    NetworkUtils::ClassifyError(ec) != NetworkUtils::ErrorSeverity::IGNORED_ERROR) {
                    OnDisconnected();
                }
            }
        });
}

// ★ [추가] 세션 정리 함수
void ServerSession::OnDisconnected() {
    std::lock_guard<std::mutex> lock(g_serverSessionMutex);
    auto it = std::find(g_serverSessions.begin(), g_serverSessions.end(), shared_from_this());
    if (it != g_serverSessions.end()) {
        g_serverSessions.erase(it);
        std::cout << "[WorldServer] ♻️ 서버 세션 자원이 안전하게 회수되었습니다.\n";
    }
}

// ==========================================
// WorldServer 대기열 구현
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
                        auto remote_ep = socket.remote_endpoint();
                        std::cout << "[WorldServer] S2S 통신: 새로운 서버 접속 확인! "
                            << "[연결처: " << remote_ep.address().to_string() << ":" << remote_ep.port() << "]\n";
                    }
                    catch (std::exception&) {
                        std::cout << "[WorldServer] S2S 통신: 서버 접속 확인! (엔드포인트 읽기 실패)\n";
                    }

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

// ==========================================
// 메인 함수
// ==========================================
int main() {
        
    SetConsoleOutputCP(CP_UTF8);

    // =========================================================
    // ★ [중복 실행 방지] 고유한 이름의 Named Mutex 생성
    // =========================================================
    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\WorldServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "🚨 [Error] WorldServer가 이미 실행 중입니다. 창을 닫습니다.\n";
        CloseHandle(hMutex);
        return 1;
    }
    // =========================================================

    if (!ConfigManager::GetInstance().LoadConfig("config.json"))
    {
        std::cerr << "🚨 config 설정 파일 오류로 인해 WorldServer 종료합니다.\n";
        system("pause"); // 디버깅 창이 바로 꺼지지 않게 대기
        return -1;
    }

    // ★ 분리된 패킷 핸들러 등록
    g_s2s_dispatcher.RegisterHandler(Protocol::PKT_LOGIN_WORLD_SELECT_REQ, Handle_WorldLoginSelectReq);

    try {
        boost::asio::io_context io_context;

        short port = ConfigManager::GetInstance().GetWorldServerPort();

        WorldServer server(io_context, port);
        std::cout << "[WorldServer] 월드 중앙 서버 가동 시작 (Port: " << port << ") Created by Jeong Shin Young\n";
        std::cout << "=================================================\n";

        // ★ 분리된 gRPC 서버 스레드 구동
        std::thread grpc_thread(RunGrpcServer);
        grpc_thread.detach();

        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "[Error] 월드 서버 예외 발생: " << e.what() << "\n";
    }
    return 0;
}