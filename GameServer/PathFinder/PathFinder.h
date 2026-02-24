#include "..\Define/Define.h"

#ifdef DEF_ADD_RECASTNAVI
#pragma once
#include <vector>

// Detour 라이브러리 전방 선언 (헤더 포함 최소화)
class dtNavMesh;
class dtNavMeshQuery;
class dtQueryFilter;

struct Vector3 {
    float x, y, z;
};

class NavMesh {
private:
    dtNavMesh* m_navMesh;           // 실제 맵 폴리곤 데이터
    dtNavMeshQuery* m_navQuery;     // 길찾기 연산을 수행하는 쿼리 객체
    dtQueryFilter* m_filter;        // 길찾기 필터 (예: 물 위는 못 감 등 설정용)

public:
    NavMesh();
    ~NavMesh();

    // 클라이언트/엔진에서 구워낸(Bake) .bin 네비메시 파일을 로드
    bool LoadNavMeshFromFile(const char* filepath);

    // ★ Detour 엔진을 이용한 진짜 A* 및 Funnel 길찾기
    std::vector<Vector3> FindPath(Vector3 start, Vector3 end);
};

#else
#pragma once
#include <vector>

// 3D 좌표 구조체
struct Vector3 {
    float x, y, z;
};

// NavMesh 위에서 길찾기를 수행하는 클래스
class NavMesh {
public:
    // 초기화 시 맵의 폴리곤 데이터를 로드 (클라이언트에서 추출한 파일)
    bool LoadNavMeshFromFile(const char* filepath);

    // A* 와 Funnel을 이용하여 시작점에서 목적지까지의 경로를 반환
    std::vector<Vector3> FindPath(Vector3 start, Vector3 end);
};
#endif//DEF_RECASTLIB_