#include "AdminService.h"
#include <iostream>
#include <grpcpp/grpcpp.h>

#include "..\Common\Protocol\protocol_grpc.grpc.pb.h"
#include "../WorldServer.h" // ★ 패킷 전송을 위해 공통 헤더 포함

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using Protocol::BuffMonsterHpAdminReq;
using Protocol::BuffMonsterHpAdminRes;
using Protocol::AdminAPI;

constexpr uint64_t MIN_MON_ID = 10000;
constexpr uint64_t MAX_MON_ID = 100000;

class AdminServiceImpl final : public AdminAPI::Service {
    Status BuffMonsterHp(ServerContext* context, const BuffMonsterHpAdminReq* request, BuffMonsterHpAdminRes* reply) override {
        int32_t add_hp = request->add_hp();
        std::cout << "\n[Admin API] 🚨 외부 운영툴로부터 몬스터 체력 버프(+" << add_hp << ") 요청 수신!\n";

        if (add_hp <= 0) {
            std::cerr << "\n[Admin API] ❌ 잘못된 버프 요청 차단 (요청된 add_hp: " << add_hp << ")\n";
            return Status(grpc::StatusCode::INVALID_ARGUMENT, "add_hp must be strictly greater than 0");
        }

        Protocol::WorldGameMonsterBuffReq buff_req;
        buff_req.set_min_uid(MIN_MON_ID);
        buff_req.set_max_uid(MAX_MON_ID);
        buff_req.set_add_hp(add_hp);

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

        reply->set_success(true);
        reply->set_message("GameServer로 n번 몬스터 체력 버프 명령을 성공적으로 전달했습니다.");

        return Status::OK;
    }
};

void RunGrpcServer() {
    std::string server_address("0.0.0.0:50051");
    AdminServiceImpl service;

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "🌐 [WorldServer] Admin gRPC 서버 가동 시작 (Port: 50051)\n";
    server->Wait();
}