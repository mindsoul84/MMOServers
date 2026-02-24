@echo off
chcp 65001 >nul
title MMORPG Server Launcher
color 0A

echo ===================================================
echo     MMORPG 게임 서버 부팅 시작
echo ===================================================
echo.

echo [1/4] 코어 로직 서버 (GameServer) 가동 중...
start "GameServer (Port: 9000)" GameServer.exe
:: GameServer가 포트를 열 시간을 1초 줍니다.
timeout /t 1 /nobreak >nul

echo [2/4] 월드 관리 서버 (WorldServer) 가동 중...
start "WorldServer" WorldServer.exe
timeout /t 1 /nobreak >nul

echo [3/4] 로그인 서버 (LoginServer) 가동 중...
start "LoginServer (Port: 7777)" LoginServer.exe
timeout /t 1 /nobreak >nul

echo [4/4] 게이트웨이 서버 (GatewayServer) 가동 중...
start "GatewayServer (Port: 8888)" GatewayServer.exe
echo.

echo ===================================================
echo     모든 서버가 성공적으로 실행되었습니다!
echo     (이제 DummyClient를 실행해 접속 테스트 진행 가능합니다.)
echo ===================================================
pause