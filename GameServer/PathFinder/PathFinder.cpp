#include "PathFinder.h"

#ifdef DEF_ADD_RECASTNAVI
#include <iostream>
#include <fstream>
#include <recastnavigation/DetourNavMesh.h>
#include <recastnavigation/DetourNavMeshQuery.h>

// =========================================================
//   [핵심 추가] GameServer.cpp에서 선언한 thread_local 객체를 가져옵니다.
// =========================================================
extern thread_local dtNavMeshQuery* t_navQuery;

// RecastDemo 표준 바이너리 헤더 구조체 (실무 표준)
struct NavMeshSetHeader {
    int magic;
    int version;
    int numTiles;
    dtNavMeshParams params;
};

struct NavMeshTileHeader {
    dtTileRef tileRef;
    int dataSize;
};

const int NAVMESHSET_MAGIC = 'M' << 24 | 'S' << 16 | 'E' << 8 | 'T'; // 'MSET'
const int NAVMESHSET_VERSION = 1;

NavMesh::NavMesh() : m_navMesh(nullptr), m_navQuery(nullptr), m_filter(nullptr) {
    m_filter = new dtQueryFilter();
    m_filter->setIncludeFlags(0xFFFF);
    m_filter->setExcludeFlags(0);
}

NavMesh::~NavMesh() {
    if (m_navQuery) dtFreeNavMeshQuery(m_navQuery);
    if (m_navMesh) dtFreeNavMesh(m_navMesh);
    if (m_filter) delete m_filter;
}

// ==========================================
//   진짜 .bin 파일을 읽어 메모리에 할당하는 함수
// ==========================================
bool NavMesh::LoadNavMeshFromFile(const char* filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "🚨 [NavMesh] 파일 열기 실패! 경로를 확인하세요: " << filepath << "\n";
        return false;
    }

    // 1. 헤더 읽기
    NavMeshSetHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(NavMeshSetHeader));
    if (header.magic != NAVMESHSET_MAGIC || header.version != NAVMESHSET_VERSION) {
        std::cerr << "🚨 [NavMesh] 잘못된 파일 포맷입니다.\n";
        return false;
    }

    // 2. NavMesh 객체 할당 및 초기화
    m_navMesh = dtAllocNavMesh();
    if (dtStatusFailed(m_navMesh->init(&header.params))) {
        std::cerr << "🚨 [NavMesh] 초기화(init) 실패.\n";
        return false;
    }

    // 3. 타일(Tile) 데이터 읽어서 메모리에 밀어넣기
    for (int i = 0; i < header.numTiles; ++i) {
        NavMeshTileHeader tileHeader;
        file.read(reinterpret_cast<char*>(&tileHeader), sizeof(NavMeshTileHeader));

        if (!tileHeader.tileRef || !tileHeader.dataSize) break;

        unsigned char* data = (unsigned char*)dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM);
        if (!data) break;

        file.read(reinterpret_cast<char*>(data), tileHeader.dataSize);
        m_navMesh->addTile(data, tileHeader.dataSize, DT_TILE_FREE_DATA, tileHeader.tileRef, 0);
    }

    // 4. 길찾기 연산을 담당할 Query 객체 초기화
    m_navQuery = dtAllocNavMeshQuery();
    m_navQuery->init(m_navMesh, 2048); // 최대 2048개의 노드 탐색 허용

    std::cout << "🗺️ [NavMesh] 지형 데이터 로드 완료! (타일 수: " << header.numTiles << ")\n";
    return true;
}
// ==========================================
//   진짜 A* 알고리즘 및 Funnel 알고리즘 복원 (Lock-Free 지원)
// ==========================================
std::vector<Vector3> NavMesh::FindPath(Vector3 start, Vector3 end) {
    std::vector<Vector3> final_path;

    // =========================================================
    //   [스레드 안전성 수정]
    // 이 함수에서 dtNavMeshQuery를 사용하는 모든 호출은 반드시 'query' 변수를 통해야 합니다.
    //
    // ❌ 기존 문제:
    //   - findNearestPoly, findStraightPath 에서 공유 객체 m_navQuery 를 직접 호출
    //   - AI 스레드 여러 개가 동시에 m_navQuery 를 건드릴 경우 → 레이스 컨디션 (크래시)
    //
    // ✅ 수정 방식:
    //   - query = t_navQuery(스레드 전용) or m_navQuery(fallback) 로 한 번만 결정
    //   - 이후 findNearestPoly / findPath / findStraightPath 모두 query 를 통해 호출
    // =========================================================
    dtNavMeshQuery* query = t_navQuery ? t_navQuery : m_navQuery;

    // 맵 데이터 또는 유효한 query 가 없으면 직선 반환 (에러 방지)
    if (!m_navMesh || !query) {
        final_path.push_back(start);
        final_path.push_back(end);
        return final_path;
    }

    float startPos[3] = { start.x, start.z, start.y };
    float endPos[3] = { end.x, end.z, end.y };
    float extents[3] = { 2.0f, 4.0f, 2.0f };

    dtPolyRef startRef = 0, endRef = 0;
    float nearestStart[3], nearestEnd[3];

    // m_filter는 읽기 전용(Read-Only) 이므로 멀티스레드 공유 가능.
    // findNearestPoly 는 query 내부 상태를 변경하므로 반드시 thread-local query 사용.
    query->findNearestPoly(startPos, extents, m_filter, &startRef, nearestStart);
    query->findNearestPoly(endPos, extents, m_filter, &endRef, nearestEnd);

    // 폴리곤을 못 찾았다면 직선 경로를 반환하여 AI가 멈추는 것을 방지
    if (!startRef || !endRef) {
        final_path.push_back(start);
        final_path.push_back(end);
        return final_path;
    }

    const int MAX_POLYS = 256;
    dtPolyRef path[MAX_POLYS];
    int pathCount = 0;

    query->findPath(startRef, endRef, nearestStart, nearestEnd, m_filter, path, &pathCount, MAX_POLYS);

    if (pathCount > 0) {
        float straightPath[MAX_POLYS * 3];
        unsigned char straightPathFlags[MAX_POLYS];
        dtPolyRef straightPathPolys[MAX_POLYS];
        int straightPathCount = 0;

        //   [핵심 수정] m_navQuery → query 로 교체 (스레드 안전성 확보)
        query->findStraightPath(nearestStart, nearestEnd, path, pathCount,
            straightPath, straightPathFlags, straightPathPolys,
            &straightPathCount, MAX_POLYS, 0);

        for (int i = 0; i < straightPathCount; ++i) {
            final_path.push_back({
                straightPath[i * 3],
                straightPath[i * 3 + 2],
                straightPath[i * 3 + 1]
                });
        }
    }
    else {
        // 길찾기 실패 시에도 무조건 직선 경로 부여
        final_path.push_back(start);
        final_path.push_back(end);
    }

    return final_path;
}
#else //DEF_ADD_RECASTNAVI
#include <iostream>

bool NavMesh::LoadNavMeshFromFile(const char* filepath) {
    // 실제로는 물리 엔진이나 RecastNavigation 라이브러리를 통해 파일 파싱
    std::cout << "[NavMesh] 데이터 로드 완료: " << filepath << "\n";
    return true;
}

std::vector<Vector3> NavMesh::FindPath(Vector3 start, Vector3 end) {
    std::vector<Vector3> final_path;

    //   크래시 방지 우회 로직  
    // 실제 .bin 파일을 파싱해서 m_navMesh->init() 과 m_navQuery->init() 을 
    // 완료하기 전까지는 Detour API를 호출하면 안 됩니다! (Assertion Error 발생)

    // [TODO: 진짜 맵 데이터 로딩 구현 전까지 아래의 Detour 로직은 주석 처리합니다]
    /*
    if (!m_navMesh || !m_navQuery) return final_path;

    float startPos[3] = { start.x, start.y, start.z };
    float endPos[3] = { end.x, end.y, end.z };
    float extents[3] = { 2.0f, 4.0f, 2.0f };

    dtPolyRef startRef, endRef;
    float nearestStart[3], nearestEnd[3];

    m_navQuery->findNearestPoly(startPos, extents, m_filter, &startRef, nearestStart);
    m_navQuery->findNearestPoly(endPos, extents, m_filter, &endRef, nearestEnd);

    // ... (이하 기존 Detour A* 및 Funnel 알고리즘 로직) ...
    */

    //   임시 조치: 시작점과 끝점을 잇는 단순 직선 경로를 반환합니다.
    // 이렇게 하면 서버가 죽지 않고 몬스터가 유저를 향해 직선으로 날아옵니다!
    final_path.push_back(start);
    final_path.push_back(end);

    return final_path;
}
#endif//DEF_ADD_RECASTNAVI