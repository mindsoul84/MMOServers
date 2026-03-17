USE game_db;
GO

--DROP TABLE Accounts

CREATE TABLE Accounts (
    account_id VARCHAR(50) PRIMARY KEY,
    password_hash VARCHAR(64) NOT NULL,
    salt VARCHAR(16) NOT NULL,
    input INT NOT NULL DEFAULT 0,
    create_date DATETIME DEFAULT GETDATE()
);