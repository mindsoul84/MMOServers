\[WorldServer gRPC API Test]



1.API\_Test\_Before.jpg

API 호출 전 화면 입니다. 화면과 같이 기존에 몬스터 체력은 100입니다.



2.API\_Test\_Failure.jpg

Postman으로 Service definition을 gRPC로 선택해서 protocol\_grpc.proto 파일 첨부하여

AdminAPI / BuffMonsterHp 선택 후 호출합니다. 0보다 작아서 유효값이 아니므로 에러를 리턴합니다.



3.API\_Test\_Success.jpg

Message에 add\_hp를 100으로 입력하여 호출 후 Response에 success:true를 확인할 수 있습니다.



4.API\_Test\_After.jpg

실제로 몬스터 체력이 증가했는지 테스트한 화면입니다.

몬스터 체력이 200으로 증가되었음을 확인할 수 있습니다.

