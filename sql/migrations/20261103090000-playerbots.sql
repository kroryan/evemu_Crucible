-- +migrate Up
-- Persistent PlayerBot metadata.  This migration is additive: it never
-- touches seed market data, player accounts, characters, inventories or orders.
CREATE TABLE IF NOT EXISTS `botCharacters` (
    `characterID` INT UNSIGNED NOT NULL,
    `enabled` TINYINT(1) NOT NULL DEFAULT 1,
    `profession` VARCHAR(32) NOT NULL DEFAULT 'MINER',
    `homeStationID` INT UNSIGNED NOT NULL DEFAULT 0,
    `currentBeltID` INT UNSIGNED NOT NULL DEFAULT 0,
    `state` VARCHAR(32) NOT NULL DEFAULT 'DOCKED',
    `respawnAt` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `personality` TEXT NOT NULL,
    `createdAt` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updatedAt` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`characterID`),
    KEY `botCharacters_enabled_profession` (`enabled`, `profession`),
    CONSTRAINT `botCharacters_character_fk`
        FOREIGN KEY (`characterID`) REFERENCES `chrCharacters` (`characterID`)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Optional: register an existing dedicated character explicitly.  When
-- <playerbots><autoProvision>true</autoProvision> is enabled the server
-- creates EVEMU Bot ### accounts/characters idempotently on first startup.
-- INSERT INTO botCharacters(characterID, profession, homeStationID, personality)
-- VALUES (90000001, 'MINER', 60003760, 'Calm industrial pilot');
-- Characters outside the EVEMU Bot prefix are never repurposed.

-- +migrate Down
-- PlayerBot state is persistent runtime data and has no automatic down migration.
