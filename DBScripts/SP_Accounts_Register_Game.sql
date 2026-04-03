USE game_db;
GO

-- ==========================================
-- [SP] Accounts_Register_Game
--
-- 신규 계정을 등록합니다.
-- account_uid, password_hash, salt는 C++ 소스코드에서 생성하여 전달합니다.
-- 에러 발생 시(PK 중복, 파라미터 오류 등) DBError 테이블에 기록 후
-- 에러를 재전파하여 ODBC 호출부에서 실패를 감지할 수 있도록 합니다.
--
-- [사용 예]
--   EXEC Accounts_Register_Game
--       @account_uid = 1501,
--       @account_name = 'myid',
--       @password_hash = 'abcdef0123456789...',
--       @salt = 'RandomSalt16Char',
--       @input = 1
-- ==========================================
CREATE OR ALTER PROCEDURE Accounts_Register_Game
    @account_uid    BIGINT,
    @account_name   VARCHAR(50),
    @password_hash  VARCHAR(64),
    @salt           VARCHAR(16),
    @input          INT
AS
BEGIN
    SET NOCOUNT ON;

    BEGIN TRY
        INSERT INTO Accounts (account_uid, account_name, password_hash, salt, input)
        VALUES (@account_uid, @account_name, @password_hash, @salt, @input);
    END TRY
    BEGIN CATCH
        INSERT INTO DBError (error_proc, error_message, error_number, error_severity, error_state, error_line)
        VALUES ('Accounts_Register_Game', ERROR_MESSAGE(), ERROR_NUMBER(), ERROR_SEVERITY(), ERROR_STATE(), ERROR_LINE());

        THROW;
    END CATCH
END
GO
