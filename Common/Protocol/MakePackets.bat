@echo off
chcp 65001 > nul
echo ==============================================
echo Protobuf C++ 클래스 생성 중...
echo ==============================================

:: 현재 폴더(./)에 있는 protocol.proto 파일을 읽어서, 
:: 현재 폴더(./)에 C++ 파일(--cpp_out)로 출력하라는 명령어입니다.
protoc.exe -I=./ --cpp_out=./ ./protocol.proto

echo.
echo 생성이 완료되었습니다!
pause