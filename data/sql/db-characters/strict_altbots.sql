CREATE TABLE IF NOT EXISTS `strict_altbots` (
    `character_guid` INT UNSIGNED NOT NULL,
    `account_id` INT UNSIGNED NOT NULL,
    `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `always_online` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `retired_at` DATETIME NULL DEFAULT NULL,
    `first_login_at` DATETIME NULL DEFAULT NULL,
    `first_login_played_seconds` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`character_guid`),
    KEY `idx_strict_altbots_enabled` (`enabled`),
    KEY `idx_strict_altbots_retired_at` (`retired_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Keep existing installations upgradeable when this file is re-applied.
SET @strict_altbots_sql = (
    SELECT IF(
        COUNT(*) = 0,
        'ALTER TABLE `strict_altbots` ADD COLUMN `first_login_at` DATETIME NULL DEFAULT NULL',
        'SELECT 1'
    )
    FROM `information_schema`.`COLUMNS`
    WHERE `TABLE_SCHEMA` = DATABASE()
      AND `TABLE_NAME` = 'strict_altbots'
      AND `COLUMN_NAME` = 'first_login_at'
);
PREPARE strict_altbots_stmt FROM @strict_altbots_sql;
EXECUTE strict_altbots_stmt;
DEALLOCATE PREPARE strict_altbots_stmt;

SET @strict_altbots_sql = (
    SELECT IF(
        COUNT(*) = 0,
        'ALTER TABLE `strict_altbots` ADD COLUMN `retired_at` DATETIME NULL DEFAULT NULL AFTER `always_online`',
        'SELECT 1'
    )
    FROM `information_schema`.`COLUMNS`
    WHERE `TABLE_SCHEMA` = DATABASE()
      AND `TABLE_NAME` = 'strict_altbots'
      AND `COLUMN_NAME` = 'retired_at'
);
PREPARE strict_altbots_stmt FROM @strict_altbots_sql;
EXECUTE strict_altbots_stmt;
DEALLOCATE PREPARE strict_altbots_stmt;

SET @strict_altbots_sql = (
    SELECT IF(
        COUNT(*) = 0,
        'ALTER TABLE `strict_altbots` ADD COLUMN `first_login_played_seconds` INT UNSIGNED NOT NULL DEFAULT 0',
        'SELECT 1'
    )
    FROM `information_schema`.`COLUMNS`
    WHERE `TABLE_SCHEMA` = DATABASE()
      AND `TABLE_NAME` = 'strict_altbots'
      AND `COLUMN_NAME` = 'first_login_played_seconds'
);
PREPARE strict_altbots_stmt FROM @strict_altbots_sql;
EXECUTE strict_altbots_stmt;
DEALLOCATE PREPARE strict_altbots_stmt;

CREATE TABLE IF NOT EXISTS `strict_altbot_levelups` (
    `character_guid` INT UNSIGNED NOT NULL,
    `level` TINYINT UNSIGNED NOT NULL,
    `level_up_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `total_played_seconds` INT UNSIGNED NOT NULL,
    `played_since_first_login_seconds` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`character_guid`, `level`),
    KEY `idx_strict_altbot_levelups_time` (`level_up_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
