#include "StressSession.h"
#include "../Manager/StressManager.h"
#include "../../Common/ConfigManager.h"
#include "../../Common/Define/StressConstants.h"
#include "../../Common/Define/SecurityConstants.h"

#include <iostream>
#include <random>

using boost::asio::ip::tcp;

StressSession::StressSession(boost::asio::io_context& io, StressManager* manager, const std::string& account_id)
    : socket_(io), strand_(io), io_context_(io), action_timer_(io), manager_(manager), account_id_(account_id) {

    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<float> dis(0.0f, StressConstants::BotAI::SPAWN_RANGE);
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
            manager_->OnSessionDisconnected(self, state_ == BotState::IN_GAME);
            state_ = BotState::DISCONNECTED;
        }

        // 암호화 상태 초기화 (재접속 시 깨끗한 상태로 시작)
        crypto_enabled_ = false;

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
    socket_.async_connect(ep, boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec) {
        if (!ec) {
            state_ = BotState::WAITING_LOGIN_RES;

            Protocol::LoginReq req;
            req.set_id(account_id_);
            req.set_password(StressConstants::Auth::BOT_PASSWORD);
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

void StressSession::ReadPayload(uint16_t payload_size) {
    payload_buf_.resize(payload_size);
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_),
        boost::asio::bind_executor(strand_, [this, self, payload_size](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                // 암호화 활성 시 수신 페이로드 복호화
                char* dispatch_data = payload_buf_.data();
                uint16_t dispatch_size = payload_size;
                std::vector<char> decrypted_buf;

                if (crypto_enabled_ && crypto_.IsInitialized() && payload_size > 0) {
                    auto result = crypto_.Decrypt(dispatch_data, payload_size);
                    if (result.success) {
                        decrypted_buf = std::move(result.data);
                        dispatch_data = decrypted_buf.data();
                        dispatch_size = static_cast<uint16_t>(decrypted_buf.size());
                    }
                    else {
                        // 복호화 실패 시 평문으로 시도 (핸드셰이크 패킷일 수 있음)
                        dispatch_data = payload_buf_.data();
                        dispatch_size = payload_size;
                    }
                }

                HandlePacket(header_.id, dispatch_data, dispatch_size);

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
    manager_->AddRecvCount();

    // 1. 로그인 성공 -> 월드 선택 요청
    if (pktId == Protocol::PKT_LOGIN_CLIENT_LOGIN_RES && state_ == BotState::WAITING_LOGIN_RES) {
        Protocol::LoginRes res;
        if (res.ParseFromArray(payload, size) && res.success()) {
            Protocol::WorldSelectReq req;
            req.set_world_id(StressConstants::Auth::DEFAULT_WORLD_ID);
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

            boost::asio::socket_base::linger option(true, 0);
            socket_.set_option(option, ec);

            socket_.close(ec);

            ConnectToGateway(res.gateway_ip(), res.gateway_port());
        }
    }
    // 3. 게이트웨이 인증 성공 -> 암호화 활성화 + 게임 플레이 시작
    else if (pktId == Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES && state_ == BotState::WAITING_GATEWAY_RES) {
        Protocol::GatewayConnectRes res;
        if (res.ParseFromArray(payload, size) && res.success()) {
            state_ = BotState::IN_GAME;

            // 핸드셰이크 성공 후 암호화 활성화
            if (crypto_.InitializeWithPassphrase(SecurityConstants::Crypto::SHARED_PASSPHRASE)) {
                crypto_enabled_ = true;
            }

            manager_->OnSessionConnected(); // 인게임 접속자 통계 증가
            ScheduleNextAction(); // 본격적인 봇 AI 가동
        }
        // ==========================================
        // GatewayConnectRes 실패 시 처리 추가
        // ==========================================
        else {
            Stop();
        }
    }
}

void StressSession::ScheduleNextAction() {
    if (state_ != BotState::IN_GAME) return;

    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> delay_dis(
        StressConstants::BotAI::MIN_ACTION_DELAY_MS,
        StressConstants::BotAI::MAX_ACTION_DELAY_MS);

    action_timer_.expires_after(std::chrono::milliseconds(delay_dis(gen)));
    auto self = shared_from_this();

    action_timer_.async_wait(boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec) {
        if (!ec && state_ == BotState::IN_GAME)
        {
            static thread_local std::mt19937 gen_inner(std::random_device{}());
            std::uniform_int_distribution<> action_dis(1, 100);

            int action = action_dis(gen_inner);
            if (action <= StressConstants::BotAI::ACTION_MOVE_PERCENT) {
                std::uniform_real_distribution<float> move_dis(
                    -StressConstants::BotAI::MOVE_RANGE,
                     StressConstants::BotAI::MOVE_RANGE);
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

// ==========================================
// SendPacket — 암호화 통합
//
// IN_GAME 상태에서 암호화가 활성화되어 있으면
// Protobuf 직렬화 후 AES-128-CBC로 암호화하여 전송합니다.
// LoginServer 통신(WAITING_LOGIN_RES)에서는 암호화가 비활성이므로 평문 전송됩니다.
// ==========================================
void StressSession::SendPacket(uint16_t pktId, const google::protobuf::Message& msg) {
    std::string serialized;
    msg.SerializeToString(&serialized);

    // 암호화 (활성화된 경우)
    std::vector<char> final_payload;
    if (crypto_enabled_ && crypto_.IsInitialized()) {
        auto result = crypto_.Encrypt(serialized.data(), static_cast<uint16_t>(serialized.size()));
        if (result.success) {
            final_payload = std::move(result.data);
        }
        else {
            final_payload.assign(serialized.begin(), serialized.end());
        }
    }
    else {
        final_payload.assign(serialized.begin(), serialized.end());
    }

    PacketHeader header{ static_cast<uint16_t>(sizeof(PacketHeader) + final_payload.size()), pktId };

    auto send_buf = std::make_shared<std::vector<char>>(header.size);
    memcpy(send_buf->data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->data() + sizeof(PacketHeader), final_payload.data(), final_payload.size());

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
