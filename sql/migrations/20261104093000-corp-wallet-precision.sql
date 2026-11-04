-- Preserve the currently stored values while preventing future FLOAT rounding loss.
ALTER TABLE crpWalletDivisons
    MODIFY balance1 DOUBLE NOT NULL DEFAULT 0,
    MODIFY balance2 DOUBLE NOT NULL DEFAULT 0,
    MODIFY balance3 DOUBLE NOT NULL DEFAULT 0,
    MODIFY balance4 DOUBLE NOT NULL DEFAULT 0,
    MODIFY balance5 DOUBLE NOT NULL DEFAULT 0,
    MODIFY balance6 DOUBLE NOT NULL DEFAULT 0,
    MODIFY balance7 DOUBLE NOT NULL DEFAULT 0;
