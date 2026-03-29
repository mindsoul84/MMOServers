USE game_db;
GO

-- ==========================================
-- [SP] Accounts_Login_Game
--
-- 계정명(account_name)으로 계정 존재 여부를 조회하고,
-- account_uid, password_hash, salt를 반환합니다.
-- 계정이 존재하지 않으면 빈 결과셋을 반환합니다.
-- 에러 발생 시 DBError 테이블에 기록 후 에러를 재전파합니다.
--
-- [사용 예]
--   EXEC Accounts_Login_Game @account_name = 'myid'
-- ==========================================
CREATE OR ALTER PROCEDURE Accounts_Login_Game
    @account_name VARCHAR(50)
AS
BEGIN
    SET NOCOUNT ON;

    BEGIN TRY
        SELECT account_uid, password_hash, salt
        FROM Accounts
        WHERE account_name = @account_name;
    END TRY
    BEGIN CATCH
        INSERT INTO DBError (error_proc, error_message, error_number, error_severity, error_state, error_line)
        VALUES ('Accounts_Login_Game', ERROR_MESSAGE(), ERROR_NUMBER(), ERROR_SEVERITY(), ERROR_STATE(), ERROR_LINE());

        THROW;
    END CATCH
END
GO
