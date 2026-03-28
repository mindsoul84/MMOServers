#include "../WorldServer/WorldServer.h"
#include "AdminAPI/AdminService.h"
#include "Handlers/WorldHandlers.h"
#include "../Common/MemoryPool.h"
#include "../Common/ConfigManager.h"
#include "../Common/Utils/NetworkErrorHandler.h"
#include "../Common/Utils/Logger.h"

#include <iostream>
#include <windows.h>
#include <thread>

using boost::asio::ip::tcp;

PacketDispatcher<ServerSession> g_s2s_dispatcher;
std::vector<std::shared_ptr<ServerSession>> g_serverSessions;
std::mutex g_serverSessionMutex;

// [추가] 토큰 저장소 전역 인스턴스
TokenStore g_tokenStore;

// ==========================================
// Graceful Shutdown
// ==========================================
static boost::asio::io_context* g_main_io_context_world = nullptr;

static BOOL WINAPI WorldConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        LOG_INFO("System", "종료 신호 수신. Graceful Shutdown 시작...");
        if (g_main_io_context_world) {
            g_main_io_context_world->stop();
        }
        return TRUE;
    }
    return FALSE;
}

// ==========================================
// ServerSession 구현부
// ==========================================
ServerSession::ServerSession(tcp::socket socket) noexcept
    : socket_(std::move(socket))
    , strand_(static_cast<boost::asio::io_context&>(socket_.get_executor().context()))
{}

void ServerSession::start() {
    ReadHeader();
}

// ==========================================
// Send() - strand + send_queue 패턴 적용 (전 세션 일관성 확보)
//
// 변경 전: async_write를 strand 없이 직접 호출
//   -> LoginServer/GameServer 등 여러 서버가 동시에 Send() 호출 시
//      같은 소켓에 concurrent async_write 발생 -> TCP 스트림 오염
//
// 변경 후: strand_ 내부에서 큐에 쌓고 DoWrite()로 순차 전송
//   -> GatewaySession, ClientSession, GameConnection과 동일한 패턴
// ==========================================
void ServerSession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) {
        LOG_WARN("WorldServer", "Send 시도했으나 소켓이 이미 닫혀있음 (PktID: " << pktId << ")");
        return;
    }

    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    if (totalSize > MAX_PACKET_SIZE) {
        LOG_ERROR("WorldServer", "패킷 크기 초과! (PktID: " << pktId << ", Size: " << totalSize << " bytes) - 전송 취소");
        return;
    }

    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    PacketHeader header{ totalSize, pktId };
    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    msg.SerializeToArray(send_buf->buffer_.data() + sizeof(PacketHeader), payloadSize);

    auto self(shared_from_this());
    boost::asio::post(strand_, [this, self, send_buf, totalSize]() {
        bool write_in_progress = !send_queue_.empty();
        send_queue_.emplace_back(send_buf, static_cast<size_t>(totalSize));
        if (!write_in_progress) {
            DoWrite();
        }
    });
}

void ServerSession::DoWrite() {
    auto self(shared_from_this());
    auto& front_msg = send_queue_.front();

    boost::asio::async_write(socket_,
        boost::asio::buffer(front_msg.first->buffer_.data(), front_msg.second),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                send_queue_.pop_front();
                if (!send_queue_.empty()) {
                    DoWrite();
                }
            }
            else {
                LOG_ERROR("WorldServer", "S2S 패킷 전송 실패 (DoWrite): " << ec.message());
                send_queue_.clear();
            }
        }));
}

void ServerSession::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) {
                    LOG_WARN("WorldServer", "잘못된 패킷 헤더 크기: " << header_.size);
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
                    LOG_INFO("WorldServer", "연동된 서버와의 연결이 해제되었습니다.");
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

void ServerSession::OnDisconnected() {
    std::lock_guard<std::mutex> lock(g_serverSessionMutex);
    auto it = std::find(g_serverSessions.begin(), g_serverSessions.end(), shared_from_this());
    if (it != g_serverSessions.end()) {
        g_serverSessions.erase(it);
        LOG_INFO("WorldServer", "서버 세션 자원이 안전하게 회수되었습니다.");
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
                        LOG_INFO("WorldServer", "S2S 통신: 새로운 서버 접속 확인! [" 
                            << remote_ep.address().to_string() << ":" << remote_ep.port() << "]");
                    }
                    catch (std::exception&) {
                        LOG_INFO("WorldServer", "S2S 통신: 서버 접속 확인! (엔드포인트 읽기 실패)");
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

    SetConsoleCtrlHandler(WorldConsoleCtrlHandler, TRUE);

    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\WorldServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_FATAL("Error", "WorldServer가 이미 실행 중입니다. 창을 닫습니다.");
        CloseHandle(hMutex);
        return 1;
    }

    if (!ConfigManager::GetInstance().LoadConfig("config.json"))
    {
        LOG_FATAL("System", "config 설정 파일 오류로 인해 WorldServer 종료합니다.");
        system("pause");
        return -1;
    }

    SendBufferPool::GetInstance().Initialize(PoolConfig::LIGHT_SERVER);

    g_s2s_dispatcher.RegisterHandler(Protocol::PKT_LOGIN_WORLD_SELECT_REQ, Handle_WorldLoginSelectReq);

    try {
        boost::asio::io_context io_context;

        g_main_io_context_world = &io_context;

        short port = ConfigManager::GetInstance().GetWorldServerPort();

        WorldServer server(io_context, port);
        LOG_INFO("WorldServer", "월드 중앙 서버 가동 시작 (Port: " << port << ") Created by Jeong Shin Young");

        std::thread grpc_thread(RunGrpcServer);

        io_context.run();

        if (grpc_thread.joinable()) grpc_thread.join();
    }
    catch (std::exception& e) {
        LOG_FATAL("Error", "월드 서버 예외 발생: " << e.what());
    }

    g_main_io_context_world = nullptr;
    return 0;
}
