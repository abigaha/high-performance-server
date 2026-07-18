CREATE DATABASE IF NOT EXISTS music_server
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE music_server;

CREATE TABLE IF NOT EXISTS users (
  user_id       BIGINT AUTO_INCREMENT PRIMARY KEY,
  username      VARCHAR(64)  NOT NULL UNIQUE,
  password_hash VARCHAR(128) NOT NULL,
  role          TINYINT      NOT NULL DEFAULT 0 COMMENT '0=GUEST 1=NORMAL 2=VIP',
  email         VARCHAR(128),
  created_at    TIMESTAMP    DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS files (
  file_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
  file_name    VARCHAR(256) NOT NULL,
  file_hash    VARCHAR(64) NOT NULL,
  file_size    BIGINT NOT NULL DEFAULT 0,
  content_type VARCHAR(128),
  chunk_size   INT NOT NULL DEFAULT 2097152,
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_file_hash (file_hash),
  INDEX idx_file_name (file_name)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS file_chunks (
  file_hash    VARCHAR(64) NOT NULL,
  chunk_index  INT NOT NULL,
  chunk_hash   VARCHAR(64) NOT NULL,
  chunk_offset BIGINT NOT NULL,
  chunk_size   INT NOT NULL,
  PRIMARY KEY (file_hash, chunk_index),
  INDEX idx_chunk_hash (chunk_hash)
) ENGINE=InnoDB;
