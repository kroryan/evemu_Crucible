-- +migrate Up
-- Durable navigation state only. Planetary commodities, containers, orders,
-- wallets and colony production remain owned by the regular game services.
CREATE TABLE IF NOT EXISTS botPlanetaryTasks (
    characterID INT UNSIGNED NOT NULL PRIMARY KEY,
    pickupID INT UNSIGNED NOT NULL DEFAULT 0,
    solarSystemID INT UNSIGNED NOT NULL DEFAULT 0,
    saleStationID INT UNSIGNED NOT NULL DEFAULT 0,
    cargoTypeID SMALLINT UNSIGNED NOT NULL DEFAULT 0
) ENGINE=InnoDB;

-- +migrate Down
-- Runtime task state is deliberately retained.
