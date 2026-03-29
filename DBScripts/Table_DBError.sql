USE game_db;
GO

-- ==========================================
-- [Table] DBError
--
-- SP 실행 중 발생한 에러를 기록하는 테이블입니다.
-- 각 SP의 TRY/CATCH 블록에서 에러 발생 시
-- 이 테이블에 INSERT 한 뒤 THROW로 에러를 재전파합니다.
-- ==========================================

--DROP TABLE DBError

CREATE TABLE DBError (
    error_id        INT IDENTITY(1,1) PRIMARY KEY,
    error_proc      NVARCHAR(128) NOT NULL,     -- 에러가 발생한 SP 이름
    error_message   NVARCHAR(4000),             -- 에러 메시지
    error_number    INT,                        -- 에러 번호
    error_severity  INT,                        -- 에러 심각도
    error_state     INT,                        -- 에러 상태
    error_line      INT,                        -- 에러 발생 라인
    error_date      DATETIME DEFAULT GETDATE()  -- 에러 발생 시각
);
GO
