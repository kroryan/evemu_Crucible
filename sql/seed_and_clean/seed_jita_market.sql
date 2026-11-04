-- Optional bootstrap for a new server. Seed a single Jita station only.
-- Existing orders are left untouched, including partially consumed stock.
INSERT INTO mktOrders
    (typeID, ownerID, regionID, stationID, solarSystemID, price,
     volEntered, volRemaining, issued, minVolume, duration, jumps, bid)
SELECT t.typeID, s.corporationID, s.regionID, s.stationID, s.solarSystemID,
       CASE WHEN COALESCE(t.basePrice, 0) <= 0 THEN 100
            ELSE t.basePrice / GREATEST(s.security, 0.1) END, 10000, 10000,
       CAST((UNIX_TIMESTAMP(NOW(6)) + 11644473600) * 10000000 AS UNSIGNED),
       1, 250, 1, 0
FROM staStations AS s
JOIN invTypes AS t ON t.published = 1
JOIN invGroups AS g ON g.groupID = t.groupID
WHERE s.stationID = 60003760 AND s.solarSystemID = 30000142
  AND g.categoryID IN (4,5,6,7,8,9,16,17,18,22,23,24,25,32,34,35,39,40,41,42,43,46)
  AND NOT EXISTS (
      SELECT 1 FROM mktOrders AS existing
      WHERE existing.stationID = s.stationID AND existing.typeID = t.typeID
        AND existing.ownerID = s.corporationID AND existing.bid = 0
  );
