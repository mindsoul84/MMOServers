USE game_db;
GO

CREATE TABLE Accounts (
    account_id VARCHAR(50) PRIMARY KEY,
    password VARCHAR(50) NOT NULL,
    input INT NOT NULL DEFAULT 0,
    create_date DATETIME DEFAULT GETDATE()
);