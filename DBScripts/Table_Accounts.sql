USE game_db;
GO

--DROP TABLE Accounts

CREATE TABLE Accounts (
    account_uid BIGINT PRIMARY KEY,
    account_name VARCHAR(50) NOT NULL,
    password_hash VARCHAR(64) NOT NULL,
    salt VARCHAR(16) NOT NULL,
    input INT NOT NULL DEFAULT 0,
    create_date DATETIME DEFAULT GETDATE()
);