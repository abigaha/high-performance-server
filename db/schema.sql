CREATE DATABASE IF NOT EXISTS music_server
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE music_server;

CREATE TABLE IF NOT EXISTS users (
  user_id       BIGINT AUTO_INCREMENT PRIMARY KEY,
  username      VARCHAR(64)  NOT NULL UNIQUE,
  password_hash VARCHAR(128) NOT NULL,
  salt          VARCHAR(32)  NOT NULL DEFAULT '',
  role          TINYINT      NOT NULL DEFAULT 0 COMMENT '0=GUEST 1=NORMAL 2=VIP 3=ADMIN',
  email         VARCHAR(128),
  vip_expires_at DATETIME(6) NULL,
  admin_slot    TINYINT GENERATED ALWAYS AS (CASE WHEN role = 3 THEN 1 ELSE NULL END) STORED,
  created_at    TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_users_username (username),
  INDEX idx_users_vip_expires_at (vip_expires_at),
  UNIQUE KEY uk_users_email (email),
  UNIQUE KEY uk_users_single_admin (admin_slot)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS schema_migrations (
  migration_key VARCHAR(128) PRIMARY KEY,
  applied_at    DATETIME(6) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS pending_chunk_deletions (
  chunk_hash      VARCHAR(64) PRIMARY KEY,
  state           ENUM('PENDING', 'CLAIMED') NOT NULL DEFAULT 'PENDING',
  claim_token     VARCHAR(64) NULL,
  claimed_at      DATETIME(6) NULL,
  retry_count     INT NOT NULL DEFAULT 0,
  next_attempt_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  last_error      VARCHAR(512) NULL,
  created_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  INDEX idx_pending_chunk_due (state, next_attempt_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS music_meta (
  music_id     BIGINT AUTO_INCREMENT PRIMARY KEY,
  title        VARCHAR(256) NOT NULL,
  artist       VARCHAR(256) NOT NULL DEFAULT '',
  album        VARCHAR(256) NOT NULL DEFAULT '',
  genre        VARCHAR(64)  NOT NULL DEFAULT '',
  duration_sec INT NOT NULL DEFAULT 0,
  track_number INT NOT NULL DEFAULT 0,
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_music_title  (title(64)),
  INDEX idx_music_artist (artist(64)),
  INDEX idx_music_album  (album(64)),
  INDEX idx_music_genre  (genre(32))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS file_records (
  file_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
  music_id     BIGINT DEFAULT NULL,
  file_name    VARCHAR(256) NOT NULL,
  file_hash    VARCHAR(64) NOT NULL UNIQUE,
  file_size    BIGINT NOT NULL DEFAULT 0,
  content_type VARCHAR(64) NOT NULL DEFAULT 'application/octet-stream',
  chunk_size   INT NOT NULL DEFAULT 2097152,
  uploaded_by  BIGINT NOT NULL DEFAULT 0,
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_file_music    (music_id),
  INDEX idx_file_hash     (file_hash),
  INDEX idx_file_type     (content_type(32)),
  INDEX idx_file_uploader (uploaded_by),
  FOREIGN KEY (music_id) REFERENCES music_meta(music_id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS file_chunks (
  chunk_id     BIGINT AUTO_INCREMENT PRIMARY KEY,
  file_hash    VARCHAR(64) NOT NULL,
  chunk_index  INT NOT NULL,
  chunk_hash   VARCHAR(64) NOT NULL,
  chunk_offset BIGINT NOT NULL DEFAULT 0,
  chunk_size   INT NOT NULL DEFAULT 0,
  INDEX idx_chunks_file_hash (file_hash),
  FOREIGN KEY (file_hash) REFERENCES file_records(file_hash) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS user_playlists (
  playlist_id  BIGINT AUTO_INCREMENT PRIMARY KEY,
  user_id      BIGINT NOT NULL,
  name         VARCHAR(128) NOT NULL DEFAULT '默认歌单',
  description  VARCHAR(512) NOT NULL DEFAULT '',
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_user_playlist (user_id, created_at DESC),
  FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS playlist_items (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  playlist_id  BIGINT NOT NULL,
  music_id     BIGINT NOT NULL,
  sort_order   INT NOT NULL DEFAULT 0,
  added_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_playlist_music (playlist_id, music_id),
  INDEX idx_playlist_item_lookup (playlist_id, sort_order, music_id),
  FOREIGN KEY (playlist_id) REFERENCES user_playlists(playlist_id) ON DELETE CASCADE,
  FOREIGN KEY (music_id) REFERENCES music_meta(music_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 可重复执行的历史库迁移：补齐 salt，并将旧 files 表的记录复制到新表。
DROP PROCEDURE IF EXISTS migrate_legacy_schema;

DELIMITER //
CREATE PROCEDURE migrate_legacy_schema()
BEGIN
  IF NOT EXISTS (
    SELECT 1
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'users'
      AND column_name = 'salt'
  ) THEN
    ALTER TABLE users
      ADD COLUMN salt VARCHAR(32) NOT NULL DEFAULT '' AFTER password_hash;
  END IF;

  IF EXISTS (
    SELECT 1
    FROM information_schema.tables
    WHERE table_schema = DATABASE()
      AND table_name = 'files'
  ) THEN
    INSERT IGNORE INTO file_records
      (file_id, file_name, file_hash, file_size, content_type, chunk_size, uploaded_by, created_at)
    SELECT file_id,
           file_name,
           file_hash,
           file_size,
           COALESCE(NULLIF(LEFT(content_type, 64), ''), 'application/octet-stream'),
           COALESCE(chunk_size, 2097152),
           0,
           COALESCE(created_at, CURRENT_TIMESTAMP)
    FROM files;
  END IF;
END//
CALL migrate_legacy_schema()//
DROP PROCEDURE migrate_legacy_schema//
DELIMITER ;
