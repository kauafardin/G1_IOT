/* =====================================================================
 *  Sensor de ocupacao e desperdicio de luz  -  AMF / IoT
 *  ESP32 DevKit V1  +  PIR (GPIO 27)  +  modulo LDR AO (GPIO 34)  +  LED (GPIO 2)
 *
 *  R1 leituras   R2 movimento por interrupcao   R3 aviso visual
 *  R4 envios     R7 serial monitor
 * ===================================================================== */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ============================ CONFIGURACAO ============================
const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_PASS  = "";
const int   WIFI_CANAL = 6;

// URL do Worker, SEM barra no final:
const char* WORKER_URL = "https://sensor-sala.kauafardin2004.workers.dev";

// ============================== PINOS =================================
const int PIN_PIR = 27;   // OUT do PIR
const int PIN_LDR = 34;   // AO do modulo LDR (pino so de entrada analogica)
const int PIN_LED = 2;    // anodo do LED -> catodo -> 220R -> GND

// ========================= R1: LIMIAR DE LUZ ==========================
// analogRead(34) devolve 0..4095. Valores medidos no Wokwi movendo o
// slider de lux do modulo LDR:
//     0.1 lux (escuro)      -> ~4067   luz APAGADA
//      14 lux (penumbra)    -> ~3446   luz APAGADA
//     229 lux (sala acesa)  -> ~1988   luz ACESA
//  100000 lux (sol)         -> ~128    luz ACESA
// Ou seja: o AO do modulo e INVERTIDO -> quanto mais luz, MENOR a leitura.
// Por isso o limiar fica no meio (2000) e "acesa" e a leitura ABAIXO dele.
const int  LIMIAR_LUZ = 2000;
const bool MAIS_LUZ_MAIOR_VALOR = false;  // no Wokwi o AO CAI quando a luz sobe

// =============================== TEMPOS ===============================
const unsigned long JANELA_OCUPADA_MS    = 30000;  // R3: LED aceso ate 30 s apos o ultimo movimento
const unsigned long ESPERA_ALERTA_MS     = 60000;  // R3: luz acesa > 60 s com sala vazia -> pisca
const unsigned long INTERVALO_LUZ_MS     = 20000;  // R4: POST /luz a cada 20 s
const unsigned long INTERVALO_LEITURA_MS = 500;    // leitura do LDR
const unsigned long PISCA_MS             = 300;    // meio periodo do pisca

// ==================== R2: ESTADO COMPARTILHADO COM A ISR ==============
portMUX_TYPE muxMovimento = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t      movimentosPendentes = 0;  // eventos ainda nao enviados
volatile unsigned long ultimoMovimentoISR  = 0;  // millis() do ultimo movimento

// ISR curtissima: so conta o evento e guarda a hora. Nada de HTTP aqui.
void IRAM_ATTR isrMovimento() {
  portENTER_CRITICAL_ISR(&muxMovimento);
  movimentosPendentes++;
  ultimoMovimentoISR = millis();
  portEXIT_CRITICAL_ISR(&muxMovimento);
}

// ============================ ESTADO GERAL ============================
uint32_t      totalMovimentos = 0;
unsigned long ultimoMovimento = 0;
bool          houveMovimento  = false;

unsigned long ultimaLeitura  = 0;
unsigned long ultimoEnvioLuz = 0;
unsigned long ultimoPisca    = 0;

int  valorLuz      = 0;
int  luzMin        = 4095;
int  luzMax        = 0;
bool luzAcesa      = false;
bool luzAcesaAntes = false;
unsigned long luzAcesaDesde = 0;

bool salaOcupadaAntes = false;
bool desperdicioAntes = false;
bool ledLigado        = false;

// ============================= WI-FI ==================================
bool conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("[WIFI] conectando em ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    delay(200);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" FALHOU (tenta de novo no proximo envio)");
    return false;
  }
  Serial.print(" OK  IP=");
  Serial.println(WiFi.localIP());
  return true;
}

// ===================== R4: ENVIO HTTPS PARA O WORKER ==================
int postJson(const char* caminho, const String& corpo) {
  if (!conectarWiFi()) return -2;

  WiFiClientSecure client;
  client.setInsecure();               // HTTPS da Cloudflare dentro do simulador

  HTTPClient http;
  String url = String(WORKER_URL) + caminho;
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setConnectTimeout(4000);       // nao deixa o loop travar se o Worker nao responder
  http.setTimeout(4000);
  http.addHeader("Content-Type", "application/json");
  int codigo = http.POST(corpo);
  http.end();
  return codigo;
}

int enviarMovimento() {
  return postJson("/movimento", "{}");
}

int enviarLuz(int valor, bool acesa) {
  String corpo = String("{\"valor\":") + valor + ",\"acesa\":" + (acesa ? 1 : 0) + "}";
  return postJson("/luz", corpo);
}

// ======================= R1: LEITURA DA LUZ ===========================
void lerLuz() {
  valorLuz = analogRead(PIN_LDR);
  if (valorLuz < luzMin) luzMin = valorLuz;
  if (valorLuz > luzMax) luzMax = valorLuz;

  bool acesaAgora = MAIS_LUZ_MAIOR_VALOR ? (valorLuz > LIMIAR_LUZ)
                                         : (valorLuz < LIMIAR_LUZ);

  if (acesaAgora && !luzAcesaAntes) {
    luzAcesaDesde = millis();         // marca o instante em que a luz acendeu
  }
  luzAcesaAntes = acesaAgora;
  luzAcesa      = acesaAgora;
}

// =============================== SETUP ================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LDR, INPUT);

  Serial.println();
  Serial.println("======================================================");
  Serial.println(" Sensor de ocupacao e desperdicio de luz - sala AMF");
  Serial.printf (" PIR=GPIO%d  LDR=GPIO%d  LED=GPIO%d\n", PIN_PIR, PIN_LDR, PIN_LED);
  Serial.printf (" LIMIAR_LUZ=%d (luz acesa = leitura %s limiar)\n",
                 LIMIAR_LUZ, MAIS_LUZ_MAIOR_VALOR ? "acima do" : "abaixo do");
  Serial.println("======================================================");

  conectarWiFi();

  // R2: movimento por interrupcao na borda de subida do PIR
  attachInterrupt(digitalPinToInterrupt(PIN_PIR), isrMovimento, RISING);
  Serial.println("[BOOT] interrupcao do PIR armada (RISING)");

  lerLuz();
  ultimoEnvioLuz = millis() - INTERVALO_LUZ_MS;   // primeiro envio de luz logo no inicio
}

// ================================ LOOP ================================
void loop() {
  unsigned long agora = millis();

  // ------- R2: consome os eventos da ISR e envia (HTTP nunca dentro da ISR) -------
  uint32_t      pendentes = 0;
  unsigned long carimbo   = 0;
  portENTER_CRITICAL(&muxMovimento);
  pendentes = movimentosPendentes;
  movimentosPendentes = 0;        // zera: nenhum evento se perde durante o envio
  carimbo   = ultimoMovimentoISR;
  portEXIT_CRITICAL(&muxMovimento);

  if (pendentes > 0) {
    ultimoMovimento = carimbo;
    houveMovimento  = true;
  }

  // ---------- R1: leitura da luz ----------
  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;
    lerLuz();
  }

  // ---------- estado da sala ----------
  bool ocupada = houveMovimento && (agora - ultimoMovimento < JANELA_OCUPADA_MS);
  bool desperdicio = (!ocupada) && luzAcesa &&
                     (agora - luzAcesaDesde >= ESPERA_ALERTA_MS);

  // ---------- R3: aviso visual na sala ----------
  if (ocupada) {
    ledLigado = true;                                  // aceso enquanto ocupada
    digitalWrite(PIN_LED, HIGH);
  } else if (desperdicio) {
    if (agora - ultimoPisca >= PISCA_MS) {             // pisca = luz esquecida acesa
      ultimoPisca = agora;
      ledLigado = !ledLigado;
      digitalWrite(PIN_LED, ledLigado ? HIGH : LOW);
    }
  } else {
    ledLigado = false;
    digitalWrite(PIN_LED, LOW);
  }

  // ---------- R7: mudancas de estado no Serial ----------
  if (ocupada != salaOcupadaAntes) {
    if (ocupada) {
      Serial.println("[SALA] OCUPADA -> LED aceso (30 s, reiniciados a cada movimento)");
    } else {
      Serial.printf("[SALA] VAZIA   -> sem movimento ha %lu s\n",
                    (unsigned long)((agora - ultimoMovimento) / 1000));
    }
    salaOcupadaAntes = ocupada;
  }
  if (desperdicio != desperdicioAntes) {
    if (desperdicio) {
      Serial.printf("[ALERT] DESPERDICIO: sala vazia e luz acesa ha %lu s -> LED PISCANDO\n",
                    (unsigned long)((agora - luzAcesaDesde) / 1000));
    } else {
      Serial.println("[ALERT] desperdicio encerrado");
    }
    desperdicioAntes = desperdicio;
  }

  // ---------- R4: um POST /movimento para cada evento pendente ----------
  // (o LED ja foi atualizado acima, entao o aviso visual nunca espera a rede)
  for (uint32_t i = 0; i < pendentes; i++) {
    totalMovimentos++;
    int codigo = enviarMovimento();
    Serial.printf("[MOV ] movimento detectado | total acumulado=%lu | POST /movimento -> HTTP %d\n",
                  (unsigned long)totalMovimentos, codigo);
  }

  // ---------- R4: POST /luz a cada 20 s ----------
  if (agora - ultimoEnvioLuz >= INTERVALO_LUZ_MS) {
    ultimoEnvioLuz = agora;
    int codigo = enviarLuz(valorLuz, luzAcesa);
    Serial.printf("[LUZ ] valor=%d (min=%d max=%d) acesa=%s | sala=%s | POST /luz -> HTTP %d\n",
                  valorLuz, luzMin, luzMax,
                  luzAcesa ? "SIM" : "NAO",
                  ocupada ? "OCUPADA" : (desperdicio ? "VAZIA/DESPERDICIO" : "VAZIA"),
                  codigo);
  }

  delay(10);   // deixa a simulacao do Wokwi mais rapida
}
