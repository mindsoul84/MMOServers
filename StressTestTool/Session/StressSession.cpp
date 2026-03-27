#include "StressSession.h"
#include "../Manager/StressManager.h"
#include "../../Common/ConfigManager.h"

#include <iostream>
#include <random>

using boost::asio::ip::tcp;

StressSession::StressSession(boost::asio::io_context& io, StressManager* manager, const std::string& account_id)
    : socket_(io), strand_(io), io_context_(io), action_timer_(io), manager_(manager), account_id_(account_id) {

    // [픽스] 난수 생성기를 스레드 로컬로 안전하게 초기화
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<float> dis(0.0f, 1000.0f);
    x_ = dis(gen);
    y_ = dis(gen);
}

void StressSession::Start() {
    ConnectToLogin();
}

void StressSession::Stop() {
    auto self = shared_from_this();

    // Stop 호출 또한 strand 안에서 안전하게 처리합니다.
    boost::asio::post(strand_, [this, self]() {

        if (state_ != BotState::DISCONNECTED)
        {            
            manager_->OnSessionDisconnected(self, state_ == BotState::IN_GAME);     // 매니저에게 나를 지우고 새로 스폰해달라고 알림
            state_ = BotState::DISCONNECTED;
        }

        boost::system::error_code ec;
        socket_.close(ec);
        action_timer_.cancel();
        send_queue_.clear();
    });
}

void StressSession::ConnectToLogin() {
    state_ = BotState::CONNECTING_LOGIN;

    std::string login_ip = ConfigManager::GetInstance().GetStressLoginServerIp();
    short login_port = ConfigManager::GetInstance().GetStressLoginServerPort();
    tcp::endpoint ep(boost::asio::ip::make_address(login_ip), login_port);

    auto self = shared_from_this();
    // 모든 콜백에 bind_executor 장착
    socket_.async_connect(ep, boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec) {
        if (!ec) {
            state_ = BotState::WAITING_LOGIN_RES;

            Protocol::LoginReq req;
            req.set_id(account_id_);
            req.set_password(BOT_PASSWORD);
            req.set_input_type(1);
            SendPacket(Protocol::PKT_CLIENT_LOGIN_LOGIN_REQ, req);

            ReadHeader();
        }
        else {
            Stop();
        }
    }));
}

void StressSession::ConnectToGateway(const std::string& ip, short port) {
    tcp::endpoint ep(boost::asio::ip::make_address(ip), port);
    auto self = shared_from_this();

    socket_.async_connect(ep, boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec) {
        if (!ec) {
            state_ = BotState::WAITING_GATEWAY_RES;

            Protocol::GatewayConnectReq req;
            req.set_account_id(account_id_);
            req.set_session_token(session_token_);
            SendPacket(Protocol::PKT_CLIENT_GATEWAY_CONNECT_REQ, req);

            ReadHeader();
        }
        else {
            Stop();
        }
    }));
}

void StressSession::ReadHeader() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),

        // ReadHeader 람다에도 bind_executor 누락 복구!
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec)
            {
                ReadPayload(header_.size - sizeof(PacketHeader));
            }
            else
            {
                Stop();
            }
        })
    );
}

// 로그인 -> 게이트웨이 환승 중이 아니면 계속 수신 대기

void StressSession::ReadPayload(uint16_t payload_size) {
    payload_buf_.resize(payload_size);
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_),
        boost::asio::bind_executor(strand_, [this, self, payload_size](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                HandlePacket(header_.id, payload_buf_.data(), payload_size);

                if (state_ != BotState::DISCONNECTED && state_ != BotState::CONNECTING_GATEWAY) {
                    ReadHeader();
                }
            }
            else {
                Stop();
            }
        })
    );
}

void StressSession::HandlePacket(uint16_t pktId, const char* payload, uint16_t size) {
    manager_->AddRecvCount(); // 패킷 수신 통계 증가

    // 1. 로그인 성공 -> 월드 선택 요청
    if (pktId == Protocol::PKT_LOGIN_CLIENT_LOGIN_RES && state_ == BotState::WAITING_LOGIN_RES) {
        Protocol::LoginRes res;
        if (res.ParseFromArray(payload, size) && res.success()) {
            Protocol::WorldSelectReq req;
            req.set_world_id(1); // 기본 월드
            SendPacket(Protocol::PKT_CLIENT_LOGIN_WORLD_SELECT_REQ, req);
        }
    }
    // 2. 월드 선택 성공 -> 게이트웨이로 환승
    else if (pktId == Protocol::PKT_LOGIN_CLIENT_WORLD_SELECT_RES && state_ == BotState::WAITING_LOGIN_RES) {
        Protocol::WorldSelectRes res;
        if (res.ParseFromArray(payload, size) && res.success()) {
            session_token_ = res.session_token();
            state_ = BotState::CONNECTING_GATEWAY;

            boost::system::error_code ec;

            // =========================================================
            // [부하테스트 극한 최적화] TIME_WAIT 생략 (Linger Option)
            // 소켓을 닫을 때 우아한 종료(FIN) 대신 강제 종료(RST)를 날려
            // OS가 포트를 즉시 회수(재사용)할 수 있도록 만듭니다.
            // =========================================================            
            boost::asio::socket_base::linger option(true, 0);       // Linger(활성화 여부, 대기 시간 0초)
            socket_.set_option(option, ec);

            socket_.close(ec);

            ConnectToGateway(res.gateway_ip(), res.gateway_port());
        }
    }
    // 3. 게이트웨이 인증 성공 -> 게임 플레이 시작
    else if (pktId == Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES && state_ == BotState::WAITING_GATEWAY_RES) {
        Protocol::GatewayConnectRes res;
        if (res.ParseFromArray(payload, size) && res.success()) {
            state_ = BotState::IN_GAME;
            manager_->OnSessionConnected(); // 인게임 접속자 통계 증가
            ScheduleNextAction(); // 본격적인 봇 AI 가동
        }
    }
}

void StressSession::ScheduleNextAction() {
    if (state_ != BotState::IN_GAME) return;

    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> delay_dis(BOT_MIN_ACTION_DELAY_MS, BOT_MAX_ACTION_DELAY_MS);

    action_timer_.expires_after(std::chrono::milliseconds(delay_dis(gen)));
    auto self = shared_from_this();

    action_timer_.async_wait(boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec) {
        if (!ec && state_ == BotState::IN_GAME)
        {
            static thread_local std::mt19937 gen_inner(std::random_device{}());
            std::uniform_int_distribution<> action_dis(1, 100);

            int action = action_dis(gen_inner);
            if (action <= BOT_ACTION_MOVE_PER) {
                std::uniform_real_distribution<float> move_dis(-5.0f, 5.0f);
                x_ += move_dis(gen_inner);
                y_ += move_dis(gen_inner);

                Protocol::MoveReq move_req;
                move_req.set_x(x_);
                move_req.set_y(y_);
                move_req.set_z(0.0f);
                move_req.set_yaw(0.0f);
                SendPacket(Protocol::PKT_CLIENT_GATEWAY_MOVE_REQ, move_req);
            }
            else {
                Protocol::AttackReq atk_req;
                atk_req.set_target_uid(0);
                SendPacket(Protocol::PKT_CLIENT_GATEWAY_ATTACK_REQ, atk_req);
            }

            ScheduleNextAction();
        }
    }));
}

void StressSession::SendPacket(uint16_t pktId, const google::protobuf::Message& msg) {
    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    PacketHeader header{ static_cast<uint16_t>(sizeof(PacketHeader) + payloadSize), pktId };

    auto send_buf = std::make_shared<std::vector<char>>(header.size);
    memcpy(send_buf->data(), &header, sizeof(PacketHeader));
    msg.SerializeToArray(send_buf->data() + sizeof(PacketHeader), payloadSize);

    auto self = shared_from_this();
    // 여러 스레드가 동시에 SendPacket을 호출해도, strand를 통해 안전하게 큐에 쌓임
    boost::asio::post(strand_, [this, self, send_buf]() {
        bool write_in_progress = !send_queue_.empty();
        send_queue_.push_back(send_buf);
        if (!write_in_progress) {
            DoWrite();
        }
        });
}

// 큐의 맨 앞 패킷부터 하나씩 꺼내어 전송
void StressSession::DoWrite() {
    auto self = shared_from_this();
    auto buf = send_queue_.front();

    boost::asio::async_write(socket_, boost::asio::buffer(*buf),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                if (state_ == BotState::IN_GAME) {
                    manager_->AddSendCount();
                }
                send_queue_.pop_front();

                // 큐에 대기중인 패킷이 있다면 꼬리물기 전송
                if (!send_queue_.empty()) {
                    DoWrite();
                }
            }
            else {
                Stop();
            }
        })
    );
}