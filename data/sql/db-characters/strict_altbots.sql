CREATE TABLE IF NOT EXISTS `strict_altbots` (
    `character_guid` INT UNSIGNED NOT NULL,
    `account_id` INT UNSIGNED NOT NULL,
    `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `always_online` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`character_guid`),
    KEY `idx_strict_altbots_enabled` (`enabled`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
