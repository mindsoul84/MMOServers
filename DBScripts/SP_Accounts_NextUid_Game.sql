USE game_db;
GO

-- ==========================================
-- [SP] Accounts_NextUid_Game
--
-- 다음 account_uid 값을 발급하여 반환합니다.
-- C++ 소스코드에서 신규 계정의 account_uid를 생성할 때 호출합니다.
--
-- SEQUENCE 객체(Seq_Account_Uid)를 사용하여 원자적으로 고유 값을 발급합니다.
-- 동시에 여러 DB 스레드가 호출해도 중복 값이 발생하지 않습니다.
--
-- 에러 발생 시 DBError 테이블에 기록 후 에러를 재전파합니다.
--
-- [사용 예]
--   EXEC Accounts_NextUid_Game
-- ==========================================
CREATE OR ALTER PROCEDURE Accounts_NextUid_Game
AS
BEGIN
    SET NOCOUNT ON;

    BEGIN TRY
        SELECT NEXT VALUE FOR dbo.Seq_Account_Uid AS next_account_uid;
    END TRY
    BEGIN CATCH
        INSERT INTO DBError (error_proc, error_message, error_number, error_severity, error_state, error_line)
        VALUES ('Accounts_NextUid_Game', ERROR_MESSAGE(), ERROR_NUMBER(), ERROR_SEVERITY(), ERROR_STATE(), ERROR_LINE());

        THROW;
    END CATCH
END
GO
