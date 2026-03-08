@echo off
chcp 65001 > nul
echo ==============================================
echo Protobuf C++ 클래스 생성 중...
echo ==============================================

:: 현재 폴더(./)에 있는 protocol.proto 파일을 읽어서, 
:: 현재 폴더(./)에 C++ 파일(--cpp_out)로 출력하라는 명령어입니다.

:: vcpkg가 설치된 경로에 맞춰 플러그인(.exe)의 위치를 지정해 주세요.
:: 보통 [vcpkg폴더위치]\installed\x64-windows\tools\grpc\grpc_cpp_plugin.exe 에 있습니다.
set GRPC_PLUGIN="C:\vcpkg\installed\x64-windows\tools\grpc\grpc_cpp_plugin.exe"

echo [1/2] protocol.proto 컴파일 중... (TCP 패킷용)
protoc.exe -I=./ --cpp_out=./ ./protocol.proto

echo [2/2] protocol_grpc.proto 컴파일 중... (gRPC API용)
:: --grpc_out 과 --plugin 옵션이 추가되었습니다!
protoc.exe -I=./ --cpp_out=./ --grpc_out=./ --plugin=protoc-gen-grpc=%GRPC_PLUGIN% ./protocol_grpc.proto

echo.
echo 생성이 완료되었습니다!
pause