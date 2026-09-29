/* =====================================================================
 *  Worker Cloudflare + banco D1  -  AMF / IoT
 *  Recebe os dados do ESP32 (R4/R5) e responde as perguntas da Carla (R6)
 *
 *  POST /movimento          -> grava um movimento
 *  POST /luz                -> grava uma leitura de luz {"valor":3120,"acesa":1}
 *  GET  /ultimo-movimento   -> ha quantos segundos foi o ultimo movimento
 *  GET  /por-hora           -> quantos movimentos em cada hora do dia
 *  GET  /luz-resumo         -> total de leituras, quantas com luz acesa e a %
 *  GET  /desperdicio        -> leituras com luz acesa sem movimento nos 2 min anteriores
 *
 *  O ts de todas as tabelas e gravado aqui, com Date.now() em ms.
 *  Todas as consultas usam prepare() + bind().
 * ===================================================================== */

const JSON_HEADERS = {
  "content-type": "application/json; charset=utf-8",
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, POST, OPTIONS",
  "access-control-allow-headers": "content-type",
};

function json(dados, status = 200) {
  return new Response(JSON.stringify(dados, null, 2), { status, headers: JSON_HEADERS });
}

// janela usada na pergunta "desperdicio": 2 minutos antes da leitura de luz
const JANELA_DESPERDICIO_MS = 120000;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const rota = url.pathname.replace(/\/+$/, "") || "/";
    const metodo = request.method.toUpperCase();

    if (metodo === "OPTIONS") {
      return new Response(null, { status: 204, headers: JSON_HEADERS });
    }

    try {
      switch (rota) {
        case "/":
          return json({
            servico: "sensor de ocupacao e desperdicio de luz",
            endpoints: {
              "POST /movimento": "grava um movimento detectado pelo PIR",
              "POST /luz": 'grava uma leitura { "valor": 0..4095, "acesa": 0|1 }',
              "GET /ultimo-movimento": "ha quantos segundos houve o ultimo movimento",
              "GET /por-hora": "quantos movimentos em cada hora do dia",
              "GET /luz-resumo": "total de leituras, quantas com luz acesa e a porcentagem",
              "GET /desperdicio": "leituras de luz acesa sem movimento nos 2 min anteriores",
            },
          });

        case "/movimento":
          if (metodo !== "POST") return json({ erro: "use POST" }, 405);
          return await gravarMovimento(request, env);

        case "/luz":
          if (metodo !== "POST") return json({ erro: "use POST" }, 405);
          return await gravarLuz(request, env);

        case "/ultimo-movimento":
          if (metodo !== "GET") return json({ erro: "use GET" }, 405);
          return await ultimoMovimento(env);

        case "/por-hora":
          if (metodo !== "GET") return json({ erro: "use GET" }, 405);
          return await porHora(env);

        case "/luz-resumo":
          if (metodo !== "GET") return json({ erro: "use GET" }, 405);
          return await luzResumo(env);

        case "/desperdicio":
          if (metodo !== "GET") return json({ erro: "use GET" }, 405);
          return await desperdicio(env);

        default:
          return json({ erro: "rota nao encontrada", rota }, 404);
      }
    } catch (e) {
      return json({ erro: "falha no servidor", detalhe: String(e) }, 500);
    }
  },
};

/* ------------------------- R4 / R5: gravacoes ------------------------- */

async function gravarMovimento(request, env) {
  const ts = Date.now();

  const r = await env.DB.prepare("INSERT INTO movimentos (ts) VALUES (?)")
    .bind(ts)
    .run();

  return json({ ok: true, id: r.meta.last_row_id, ts }, 201);
}

async function gravarLuz(request, env) {
  let corpo;
  try {
    corpo = await request.json();
  } catch {
    return json({ erro: "corpo precisa ser JSON" }, 400);
  }

  const valor = Number(corpo?.valor);
  const acesa = Number(corpo?.acesa) ? 1 : 0;

  if (!Number.isFinite(valor) || valor < 0 || valor > 4095) {
    return json({ erro: "valor precisa ser um numero de 0 a 4095" }, 400);
  }

  const ts = Date.now();

  const r = await env.DB.prepare("INSERT INTO luz (valor, acesa, ts) VALUES (?, ?, ?)")
    .bind(Math.round(valor), acesa, ts)
    .run();

  return json({ ok: true, id: r.meta.last_row_id, valor: Math.round(valor), acesa, ts }, 201);
}

/* --------------------- R6: as perguntas da Carla ---------------------- */

// "ha quanto tempo foi o ultimo movimento?"
async function ultimoMovimento(env) {
  const linha = await env.DB.prepare(
    "SELECT id, ts FROM movimentos ORDER BY ts DESC LIMIT ?"
  )
    .bind(1)
    .first();

  if (!linha) {
    return json({ houve_movimento: false, mensagem: "nenhum movimento registrado ainda" });
  }

  const agora = Date.now();
  return json({
    houve_movimento: true,
    id: linha.id,
    ts: linha.ts,
    ha_segundos: Math.round((agora - linha.ts) / 1000),
    quando: new Date(linha.ts).toISOString(),
  });
}

// "em que horarios do dia a sala tem movimento?"
async function porHora(env) {
  const { results } = await env.DB.prepare(
    `SELECT strftime('%H', ts / 1000, 'unixepoch', ?) AS hora,
            COUNT(*)                                  AS total
       FROM movimentos
      GROUP BY hora
      ORDER BY hora`
  )
    .bind("-3 hours") // horario de Brasilia
    .all();

  const total = results.reduce((s, l) => s + l.total, 0);
  return json({ fuso: "America/Sao_Paulo (UTC-3)", total_movimentos: total, por_hora: results });
}

// "quantas leituras de luz, quantas com a luz acesa e a porcentagem"
async function luzResumo(env) {
  const linha = await env.DB.prepare(
    `SELECT COUNT(*)                          AS total_leituras,
            COALESCE(SUM(acesa), 0)           AS leituras_acesa,
            COALESCE(ROUND(AVG(valor), 1), 0) AS media_valor
       FROM luz`
  ).first();

  const total = linha.total_leituras;
  const acesas = linha.leituras_acesa;
  const pct = total > 0 ? Math.round((acesas * 1000) / total) / 10 : 0;

  return json({
    total_leituras: total,
    leituras_com_luz_acesa: acesas,
    porcentagem_acesa: pct,
    media_valor_lido: linha.media_valor,
  });
}

// "quantas leituras de luz acesa nao tiveram nenhum movimento nos 2 minutos anteriores"
async function desperdicio(env) {
  const linha = await env.DB.prepare(
    `SELECT COUNT(*) AS leituras_desperdicio
       FROM luz
      WHERE acesa = 1
        AND NOT EXISTS (
              SELECT 1
                FROM movimentos m
               WHERE m.ts BETWEEN luz.ts - ? AND luz.ts
            )`
  )
    .bind(JANELA_DESPERDICIO_MS)
    .all();

  const acesas = await env.DB.prepare(
    "SELECT COUNT(*) AS total FROM luz WHERE acesa = ?"
  )
    .bind(1)
    .first();

  const desperdicio = linha.results[0].leituras_desperdicio;
  const totalAcesas = acesas.total;
  const pct = totalAcesas > 0 ? Math.round((desperdicio * 1000) / totalAcesas) / 10 : 0;

  return json({
    janela_minutos: JANELA_DESPERDICIO_MS / 60000,
    leituras_com_luz_acesa: totalAcesas,
    leituras_desperdicio: desperdicio,
    porcentagem_desperdicio: pct,
  });
}
