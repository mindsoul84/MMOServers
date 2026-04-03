USE game_db;
GO

-- ==========================================
-- [Sequence] Seq_Account_Uid
--
-- account_uid 발급 전용 SEQUENCE 객체입니다.
-- NEXT VALUE FOR 호출 시 SQL Server가 내부적으로 원자적 증가를 보장하므로
-- 동시에 여러 스레드가 호출해도 절대 중복 값이 발생하지 않습니다.
--
-- [주의] 기존에 Accounts 테이블에 데이터가 존재하는 경우
-- 아래 초기화 스크립트가 자동으로 MAX(account_uid) + 1 부터 시작하도록 설정합니다.
-- ==========================================
--DROP SEQUENCE dbo.Seq_Account_Uid
CREATE SEQUENCE dbo.Seq_Account_Uid
    AS BIGINT
    START WITH 1
    INCREMENT BY 1
    NO CACHE;
GO

-- 기존 데이터가 있으면 SEQUENCE 시작 값을 MAX(account_uid) + 1로 조정
DECLARE @max_uid BIGINT;
SELECT @max_uid = ISNULL(MAX(account_uid), 0) FROM Accounts;
IF @max_uid > 0
BEGIN
    DECLARE @sql NVARCHAR(100) = N'ALTER SEQUENCE dbo.Seq_Account_Uid RESTART WITH ' + CAST(@max_uid + 1 AS NVARCHAR(20));
    EXEC sp_executesql @sql;
    PRINT '[Seq_Account_Uid] 기존 데이터 감지. 시작 값을 ' + CAST(@max_uid + 1 AS NVARCHAR(20)) + '(으)로 설정.';
END
GO
