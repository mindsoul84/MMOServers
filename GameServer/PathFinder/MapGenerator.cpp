#include "MapGenerator.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <recastnavigation/DetourNavMesh.h>
#include <recastnavigation/DetourNavMeshBuilder.h>

void GenerateDummyMapFile(const char* filepath) {
    std::ifstream check_file(filepath);
    if (check_file.is_open()) return;

    std::cout << "[System] 맵 파일이 없습니다. [장애물 회피 테스트용 L자 맵]을 베이킹합니다...\n";

    dtNavMeshCreateParams params;
    std::memset(&params, 0, sizeof(params));

    unsigned short verts[] = {
        0, 0, 0,        50, 0, 0,       50, 0, 50,      0, 0, 50,
        250, 0, 0,      250, 0, 50,     50, 0, 250,     0, 0, 250
    };
    params.verts = verts;
    params.vertCount = 8;

    unsigned short polys[] = {
        0, 1, 2, 3,    0xffff, 1, 2, 0xffff,
        1, 4, 5, 2,    0xffff, 0xffff, 0xffff, 0,
        3, 2, 6, 7,    0, 0xffff, 0xffff, 0xffff
    };
    params.polys = polys;
    params.polyCount = 3;
    params.nvp = 4;

    unsigned char polyAreas[] = { 1, 1, 1 };
    unsigned short polyFlags[] = { 1, 1, 1 };
    params.polyAreas = polyAreas;
    params.polyFlags = polyFlags;

    params.bmin[0] = 0.0f; params.bmin[1] = 0.0f; params.bmin[2] = 0.0f;
    params.bmax[0] = 50.0f; params.bmax[1] = 1.0f; params.bmax[2] = 50.0f;

    params.cs = 0.2f; params.ch = 0.2f;
    params.walkableHeight = 2.0f; params.walkableRadius = 0.5f; params.walkableClimb = 0.5f;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) {
        std::cerr << "🚨 NavMesh 베이킹 실패!\n";
        return;
    }

    struct NavMeshSetHeader { int magic; int version; int numTiles; dtNavMeshParams params; };
    struct NavMeshTileHeader { dtTileRef tileRef; int dataSize; };

    NavMeshSetHeader header;
    std::memset(&header, 0, sizeof(NavMeshSetHeader));
    header.magic = 'M' << 24 | 'S' << 16 | 'E' << 8 | 'T';
    header.version = 1; header.numTiles = 1;
    header.params.tileWidth = 50.0f; header.params.tileHeight = 50.0f;
    header.params.maxTiles = 1; header.params.maxPolys = 10;

    NavMeshTileHeader tileHeader;
    tileHeader.tileRef = 1; tileHeader.dataSize = navDataSize;

    std::ofstream file(filepath, std::ios::binary);
    file.write(reinterpret_cast<char*>(&header), sizeof(NavMeshSetHeader));
    file.write(reinterpret_cast<char*>(&tileHeader), sizeof(NavMeshTileHeader));
    file.write(reinterpret_cast<char*>(navData), navDataSize);
    file.close();

    dtFree(navData);
    std::cout << "[System] ✅ dummy_map.bin (장애물 미로 버전) 베이킹 완료!\n";
}