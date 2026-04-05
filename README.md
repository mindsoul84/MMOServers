# MMOServers

[게임서버 실행 가이드]
1. 우측 Release에 Server Binary Release를 클릭해서 MMOServersBin.zip 파일을 다운로드 받아서 압축을 풉니다. 
2. MMOServers.bat 배치 파일을 실행합니다. 
3. 처음 실행할때는 각 서버 바이너리 관련 설정을 허용 해야합니다. 
4. DummyClient.exe을 실행합니다. (DummyClient는 여러번 실행 가능합니다.) 
5. 방향키, 채팅을 하면서 테스트 합니다. (여러개의 클라이언트로 이동 시 게임서버 로그를 통해 확인 가능합니다.) 
6. 게임서버 부하테스트 실행 가능 (서버 바이너리 우선 실행 후 StressTestTool.exe 실행) 

※ sln, csproj 등 환경 세팅 파일들은 제외 하였습니다.

※ 이해를 돕기위해 콘솔 출력은 대부분 한글로 처리했습니다. (실무에서는 영어 사용)

※ config.json 파일에서 db_conn 으로 MSSQL DB연결 및 테스트 가능합니다. (SSMS 설치 필요)

※ config.json 파일에서 redis_conn 으로 Redis 연결 및 테스트 가능합니다. (redis-server 설치 필요)
