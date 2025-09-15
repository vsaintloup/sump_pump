#include <Arduino.h>
#include <BLEDevice.h>
#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include <Adafruit_NeoPixel.h>

// ===== CONFIG =====
static const char* TARGET_BLE_MAC   = "a5:c2:37:5d:85:67"; // MAC BLE trouvée (insensible à la casse)
static const char* TARGET_NAME_HINT = "DP04S";             // Filtre nom si scan nécessaire

// Service/Chars JBD courants
static BLEUUID SVC_FF00("0000ff00-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FF01("0000ff01-0000-1000-8000-00805f9b34fb"); // notify
static BLEUUID CHR_FF02("0000ff02-0000-1000-8000-00805f9b34fb"); // write

static BLEUUID SVC_FFF0("0000fff0-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FFF1("0000fff1-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FFF2("0000fff2-0000-1000-8000-00805f9b34fb");

static BLEUUID SVC_FFE0("0000ffe0-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FFE1("0000ffe1-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FFE2("0000ffe2-0000-1000-8000-00805f9b34fb");

// LCD I2C
LiquidCrystal_PCF8574 lcd(0x27);  // Mets 0x3F si besoin
static String fit16(const String &s){ if(s.length()>=16) return s.substring(0,16); String t=s; while(t.length()<16) t+=' '; return t; }
static void lcdPrintLine(uint8_t row, const String &s){ lcd.setCursor(0,row); lcd.print(fit16(s)); }
static void lcdBoot(const char* line2="Lecture BLE..."){ lcd.clear(); lcdPrintLine(0,"M.Y. Technologie"); lcdPrintLine(1,line2); }

// Trame JBD basique à envoyer
static const uint8_t CMD_BASIC[] = {0xDD,0xA5,0x03,0x00,0xFF,0xFD,0x77};
static const unsigned POLL_MS = 2000;

// ===== ÉTAT BLE =====
static BLEClient* gClient = nullptr;
static BLERemoteCharacteristic* gChrNotify = nullptr;
static BLERemoteCharacteristic* gChrWrite  = nullptr;
static bool gConnected = false;
static unsigned long gLastPoll = 0;

// Mesures affichage
static volatile float gV=NAN, gI=NAN; static volatile int gSOC=-1; static volatile bool gHaveData=false;

// ===== WS2812B (NeoPixel) =====
#define LED_PIN   5           // <-- DATA sur GPIO 5
#define LED_COUNT 1
Adafruit_NeoPixel px(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// petite fonction de vague triangulaire (0..255) non bloquante
static uint8_t triPulse(uint16_t period_ms, uint8_t minb, uint8_t maxb){
  if(maxb <= minb) return minb;
  uint32_t t = millis() % period_ms;
  uint32_t half = period_ms / 2;
  uint32_t val = (t < half) ? t : (period_ms - t);
  return (uint8_t)(minb + (uint32_t)(maxb - minb) * val / half);
}

// applique couleur + brightness
static void ledColor(uint8_t r, uint8_t g, uint8_t b, uint8_t br){
  px.setBrightness(br);                // 0..255
  px.setPixelColor(0, px.Color(r,g,b));
  px.show();
}

// ===== Parsing trames =====
static bool findFrameJBD(std::vector<uint8_t>& buf, std::vector<uint8_t>& out){
  while(buf.size()>=7){
    auto it = std::find(buf.begin(), buf.end(), 0xDD);
    if(it==buf.end()){ buf.clear(); return false; }
    if(it!=buf.begin()) buf.erase(buf.begin(), it);
    if(buf.size()<7) return false;
    uint16_t length = ((uint16_t)buf[2]<<8)|buf[3];
    size_t total = 1+1+2+length+2+1;
    if(buf.size()<total) return false;
    if(buf[total-1]==0x77){
      out.assign(buf.begin(), buf.begin()+total);
      buf.erase(buf.begin(), buf.begin()+total);
      return true;
    } else { buf.erase(buf.begin()); }
  }
  return false;
}

static bool parseA1A2(const uint8_t* b, size_t n, float& V, float& I, int& SOC){
  if(n<6 || !(b[0]==0xA1 || b[0]==0xA2)) return false;
  uint16_t mv=(b[1]<<8)|b[2]; int16_t ma=(int16_t)((b[3]<<8)|b[4]); if(ma&0x8000) ma-=0x10000;
  V=mv/100.0f; I=ma/100.0f; SOC=b[5]; if(SOC>100) SOC=-1; return true;
}

static bool parseBasicJBD(const std::vector<uint8_t>& f, float& V, float& I, int& SOC){
  if(f.size()<7 || f[0]!=0xDD || f[1]!=0x03) return false;
  uint16_t L=((uint16_t)f[2]<<8)|f[3]; if(f.size()<(size_t)(L+7)) return false;
  const uint8_t* p=&f[4]; if(L<6) return false;
  uint16_t mv=(p[0]<<8)|p[1]; int16_t ma=(int16_t)((p[2]<<8)|p[3]); if(ma&0x8000) ma-=0x10000;
  V=mv/100.0f; I=ma/100.0f; SOC=(L>19 && p[19]<=100)?p[19]:-1; return true;
}

// ===== Notify callback =====
static void notifyCB(BLERemoteCharacteristic* /*chr*/, uint8_t* data, size_t len, bool /*isNotify*/){
  float V=0, I=0; int SOC=-1;
  if(parseA1A2(data, len, V, I, SOC)){ gV=V; gI=I; gSOC=SOC; gHaveData=true; return; }
  static std::vector<uint8_t> rx; rx.insert(rx.end(), data, data+len);
  std::vector<uint8_t> frame; if(findFrameJBD(rx, frame)){
    if(parseBasicJBD(frame, V, I, SOC)){ gV=V; gI=I; gSOC=SOC; gHaveData=true; }
  }
}

// ===== Découverte service/char =====
static bool setupSvcAndChars(BLEClient* cli){
  gChrNotify=nullptr; gChrWrite=nullptr;

  // 1) FF00/FF01/FF02
  BLERemoteService* s = cli->getService(SVC_FF00);
  if(s){ gChrNotify=s->getCharacteristic(CHR_FF01); gChrWrite=s->getCharacteristic(CHR_FF02); }
  // 2) FFF0/FFF1/FFF2
  if(!gChrNotify || !gChrWrite){
    s = cli->getService(SVC_FFF0);
    if(s){ if(!gChrNotify) gChrNotify=s->getCharacteristic(CHR_FFF1);
           if(!gChrWrite)  gChrWrite =s->getCharacteristic(CHR_FFF2); }
  }
  // 3) FFE0/FFE1/FFE2
  if(!gChrNotify || !gChrWrite){
    s = cli->getService(SVC_FFE0);
    if(s){ if(!gChrNotify) gChrNotify=s->getCharacteristic(CHR_FFE1);
           if(!gChrWrite)  gChrWrite =s->getCharacteristic(CHR_FFE2); }
  }

  if(!gChrNotify || !gChrWrite) return false;
  if(!gChrNotify->canNotify())  return false;
  gChrNotify->registerForNotify(notifyCB); 
  return true;
}

// ===== Connexion =====
static bool connectDirectOrScan(){
  static BLEClient* gClientLocal = nullptr;
  if(!gClient) gClient = BLEDevice::createClient();

  // 1) Tentative directe par MAC BLE
  Serial.printf("[BLE] Connexion directe a %s ...\n", TARGET_BLE_MAC);
  if(gClient->connect(BLEAddress(TARGET_BLE_MAC))){
    Serial.println("[BLE] Connecte (direct).");
    if(setupSvcAndChars(gClient)){ Serial.println("[BLE] Notifs OK"); gConnected=true; gLastPoll=0; return true; }
    Serial.println("[BLE] Services/Chars introuvables -> disconnect");
    gClient->disconnect();
  } else {
    Serial.println("[BLE] Echec connexion directe.");
  }

  // 2) Scan par nom/UUID si direct échoue
  Serial.println("[BLE] Scan 12s (nom/UUID)...");
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true); scan->setInterval(45); scan->setWindow(35); scan->clearResults();
  BLEScanResults* results = scan->start(12, false);
  if(!results){ Serial.println("[BLE] start() -> nullptr"); return false; }

  // a) Par nom partiel (DP04S…)
  for(int i=0;i<results->getCount();i++){
    BLEAdvertisedDevice d = results->getDevice(i);
    String n = d.haveName() ? String(d.getName().c_str()) : String("");
    if(n.length()>0 && n.indexOf(TARGET_NAME_HINT)>=0){
      Serial.printf("[SCAN] Match nom: %s (RSSI=%d)\n", d.getAddress().toString().c_str(), d.getRSSI());
      if(gClient->connect(d.getAddress())){
        Serial.println("[BLE] Connecte (scan/nom).");
        if(setupSvcAndChars(gClient)){ Serial.println("[BLE] Notifs OK"); gConnected=true; gLastPoll=0; return true; }
        Serial.println("[BLE] Services/Chars introuvables -> disconnect");
        gClient->disconnect();
      }
    }
  }
  // b) Par service FF00/FFF0/FFE0
  for(int i=0;i<results->getCount();i++){
    BLEAdvertisedDevice d = results->getDevice(i);
    bool hasSvc = (d.haveServiceUUID() &&
                   (d.getServiceUUID().equals(SVC_FF00) || d.getServiceUUID().equals(SVC_FFF0) || d.getServiceUUID().equals(SVC_FFE0)));
    if(hasSvc){
      Serial.printf("[SCAN] Match service: %s (UUID=%s)\n",
                    d.getAddress().toString().c_str(), d.getServiceUUID().toString().c_str());
      if(gClient->connect(d.getAddress())){
        Serial.println("[BLE] Connecte (scan/uuid).");
        if(setupSvcAndChars(gClient)){ Serial.println("[BLE] Notifs OK"); gConnected=true; gLastPoll=0; return true; }
        Serial.println("[BLE] Services/Chars introuvables -> disconnect");
        gClient->disconnect();
      }
    }
  }

  Serial.println("[BLE] Aucun device compatible trouvé au scan.");
  return false;
}

// ===== Setup / Loop =====
void setup(){
  Serial.begin(115200); delay(200);
  Wire.begin(21,22); lcd.begin(16,2); lcd.setBacklight(255); lcdBoot();

  // LED
  px.begin();
  px.clear();
  px.show();

  BLEDevice::init("ESP32-BMS");
  BLEDevice::setPower(ESP_PWR_LVL_P9);  // niveau TX max

  if(!connectDirectOrScan()){
    lcdBoot("Scan/Conn. echoue");
  }
}

// met à jour l’état LED en continu (non bloquant)
static void updateStatusLED(){
  static unsigned long last = 0;
  if(millis() - last < 20) return;  // ~50 Hz
  last = millis();

  bool connected = gConnected && gClient && gClient->isConnected();

  // Si non connecté ou pas encore de data -> pulsation bleue lente
  if(!connected || !gHaveData || isnan(gV) || isnan(gI)){
    uint8_t br = triPulse(1600, 5, 40);    // lent, doux
    ledColor(0, 0, 255, br);
    return;
  }

  // Connecté avec data -> regarde la puissance "signée"
  float Praw = gV * gI;   // >0 = charge, <0 = décharge (d’après ton BMS)
  if(Praw > 5.0f){        // seuil 5 W pour éviter le papillonnement
    uint8_t br = triPulse(500, 10, 200);   // pulsation rapide verte
    ledColor(0, 255, 0, br);
  } else {
    ledColor(0, 255, 0, 38);               // vert fixe ~15% (38/255)
  }
}

void loop(){
  // Si pas connecté, on retente toutes ~4 s
  if(!gConnected || !gClient || !gClient->isConnected()){
    static unsigned long lastTry=0;
    if(millis()-lastTry > 4000){
      lastTry = millis();
      Serial.println("[BLE] Tentative (re)connexion...");
      connectDirectOrScan();
    }
    lcdPrintLine(1, "Scan en cours...");
    updateStatusLED();
    delay(100);
    return;
  }

  // Poll périodique (write)
  if(gChrWrite){
    unsigned long now = millis();
    if(now - gLastPoll > POLL_MS){
      gLastPoll = now;
      gChrWrite->writeValue((uint8_t*)CMD_BASIC, sizeof(CMD_BASIC), false); // sans réponse
      Serial.println("TX");
    }
  }

  // UI
  static unsigned long lastUI=0;
  if(millis()-lastUI>300){
    lastUI=millis();
    if(gHaveData && !isnan(gV) && !isnan(gI)){
      float P=fabsf(gV*gI);
      char l1[17], l2[17];
      snprintf(l1, sizeof(l1), "%5.1fV  %5.2fA", gV, gI);
      if(gSOC>=0) snprintf(l2,sizeof(l2),"SOC:%3d%%  %4dW", gSOC, (int)lroundf(P));
      else        snprintf(l2,sizeof(l2),"SOC:--    %4dW", (int)lroundf(P));
      lcdPrintLine(0, String(l1));
      lcdPrintLine(1, String(l2));
    } else {
      lcdPrintLine(1, "Lecture BLE...");
    }
  }

  updateStatusLED();
  delay(10);
}
