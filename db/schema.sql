CREATE DATABASE IF NOT EXISTS music_server
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE music_server;

CREATE TABLE IF NOT EXISTS users (
  user_id       BIGINT AUTO_INCREMENT PRIMARY KEY,
  username      VARCHAR(64)  NOT NULL UNIQUE,
  password_hash VARCHAR(128) NOT NULL,
  email         VARCHAR(128),
  created_at    TIMESTAMP    DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS download_logs (
  log_id        BIGINT AUTO_INCREMENT PRIMARY KEY,
  user_id       BIGINT      NOT NULL,
  file_hash     VARCHAR(64) NOT NULL,
  downloaded_at TIMESTAMP   DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (user_id) REFERENCES users(user_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS file_meta (
  file_hash    VARCHAR(64)  PRIMARY KEY,
  file_path    VARCHAR(512) NOT NULL,
  file_size    BIGINT       NOT NULL DEFAULT 0,
  content_type VARCHAR(128),
  created_at   TIMESTAMP    DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;
