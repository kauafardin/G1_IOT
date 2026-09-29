-- R5: banco D1 - tabelas do sensor de sala
-- O ts e sempre gravado pelo Worker com Date.now() (milissegundos).

CREATE TABLE IF NOT EXISTS movimentos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS luz (
  id    INTEGER PRIMARY KEY AUTOINCREMENT,
  valor INTEGER NOT NULL,
  acesa INTEGER NOT NULL,
  ts    INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_movimentos_ts ON movimentos (ts);
CREATE INDEX IF NOT EXISTS idx_luz_ts        ON luz (ts);
