// =====================================================================
//  FLAX Aktarici Sistem - VERICI (Kumanda)
//  FLAX Bluetooth & NRF24 Kumanda Karti (Nanelab) + Arduino Nano
//  + nRF24L01+PA+LNA + 16x2 I2C LCD
//
//  KUMANDA --(AKT01)--> AKTARICI --(ARB02)--> ARABA
//
//  Joystick'i okur, diferansiyel surus karisimini hesaplar ve 20 ms'de
//  bir komut paketi gonderir. Arabanin telemetrisi aktarici uzerinden
//  ACK payload olarak doner ve LCD'de gosterilir.
//
//  Gereksinim : RF24 (TMRh20) >= 1.4.0
//               LiquidCrystal_I2C (Frank de Brabander)
//  Seri port  : 115200 baud
//
//  Not: D0/D1 (Buton A/B) donanimsal seri porttur, D2 WS2812 LED'e
//  ayrilmistir. A6/A7 yalnizca analog okunabilir.
// =====================================================================

#include <SPI.h>
#include <RF24.h>

// =====================================================================
//  1) DERLEME SECENEKLERI
// =====================================================================

// 1: telsiz ve LCD devre disi, yalnizca pin degerleri basilir.
#define PIN_KESIF_MODU   0

#define LCD_KULLAN       1
#define LCD_ADRES        0x27   // bazi modullerde 0x3F

#define BUTON_KULLAN     0
#define POT_KULLAN       0

#if LCD_KULLAN
  #include <Wire.h>
  #include <LiquidCrystal_I2C.h>
  LiquidCrystal_I2C lcd(LCD_ADRES, 16, 2);
#endif

// =====================================================================
//  2) PIN TANIMLARI
// =====================================================================

#define CE_PIN    10
#define CSN_PIN   9

#define JOY_X_PIN   A0
#define JOY_Y_PIN   A1

#define POT_PIN     A3

#define BUTON_1_PIN   A2
#define BUTON_2_PIN   6
#define BUTON_4_PIN   7

// =====================================================================
//  3) TELSIZ AYARLARI
// =====================================================================

const uint8_t ADRES[6] = "AKT01";
const uint8_t KANAL    = 108;

// RF24_250KBPS yalnizca gercek nRF24L01+ cipinde calisir.
#define VERI_HIZI   RF24_250KBPS

// Sahada RF24_PA_MAX kullanin; yakin mesafede MAX alicinin on katini doyurur.
#define RF_GUC      RF24_PA_MIN

// =====================================================================
//  4) SURUS AYARLARI
// =====================================================================

const int SAFE_ZONE = 40;
const unsigned long GONDERIM_ARALIGI = 20;
const unsigned long LOG_ARALIGI      = 300;
const unsigned long LCD_ARALIGI      = 250;
const unsigned long LCD_MOD_SURE     = 2000;

const unsigned int MESAFE_YOK = 999;
const unsigned int DUR_MESAFE = 25;

// =====================================================================
//  5) PAKET YAPILARI
// =====================================================================

const uint8_t KOMUT_IMZA = 0xA5;
const uint8_t TLM_IMZA   = 0x5A;

struct __attribute__((packed)) KomutPaketi {
  uint8_t  imza;
  uint8_t  sayac;
  uint16_t joyX, joyY;  // ham ADC 0..1023
  uint16_t pot;         // ham ADC 0..1023
  uint8_t  butonlar;
  int16_t  sol, sag;    // motor hizlari -255..+255
};

#define BIT_BUTON1   0x01
#define BIT_BUTON2   0x02
#define BIT_BUTON4   0x04

struct __attribute__((packed)) TelemetriPaketi {
  uint8_t  imza;
  uint16_t vcc;       // mV
  uint16_t minVcc;    // mV
  uint16_t mesafe;    // cm, 999 = engel yok
  uint8_t  durum;
};

#define TLM_BIT_ENGEL  0x01

static_assert(sizeof(KomutPaketi)     == 13, "KomutPaketi 13 bayt olmali");
static_assert(sizeof(TelemetriPaketi) == 8,  "TelemetriPaketi 8 bayt olmali");

RF24 radio(CE_PIN, CSN_PIN, 2000000);

// =====================================================================
//  6) DURUM DEGISKENLERI
// =====================================================================

int merkezX = 512, merkezY = 512;
int olcekX  = 470, olcekY  = 470;

uint8_t sayac = 0;
unsigned long sonGonderim = 0;
unsigned long sonLog      = 0;

unsigned long basarili = 0, basarisiz = 0;
unsigned int  ustusteHata = 0;
bool          uyariBasildi = false;

TelemetriPaketi tlm;
bool tlmGecerli = false;
unsigned long sonTlm = 0;

int sonSol = 0, sonSag = 0;

unsigned long sonLcdYazim = 0;
unsigned long sonModDegis = 0;
uint8_t  altMod = 0;
bool     gosterilenBagli = false;
unsigned int gosterilenMesafe = 0;
bool     ilkLcdYazim = true;

// =====================================================================
//  7) YARDIMCI FONKSIYONLAR
// =====================================================================

void cizgi() {
  Serial.println(F("---------------------------------------------"));
}

void voltYaz(unsigned int mv) {
  if (mv < 2000) { Serial.print('-'); return; }
  unsigned int yuzde = (mv % 1000) / 10;
  Serial.print(mv / 1000); Serial.print('.');
  if (yuzde < 10) Serial.print('0');
  Serial.print(yuzde);     Serial.print('V');
}

int eksenCevir(int ham, int merkez, int olcek) {
  long sapma = (long)ham - (long)merkez;
  long d = sapma * 255L / (long)olcek;
  if (d >  255) d =  255;
  if (d < -255) d = -255;
  return (int)d;
}

void joystickKalibreEt() {
  Serial.println(F("[4/6] Joystick kalibrasyonu... DOKUNMA"));

  // ADC multiplexer gecisinden sonraki ilk okumalar atilir.
  for (int i = 0; i < 8; i++) {
    analogRead(JOY_X_PIN); analogRead(JOY_Y_PIN); delay(2);
  }

  long tx = 0, ty = 0;
  int xMin = 1023, xMax = 0, yMin = 1023, yMax = 0;

  for (int i = 0; i < 64; i++) {
    int ax = analogRead(JOY_X_PIN); tx += ax;
    if (ax < xMin) xMin = ax;  if (ax > xMax) xMax = ax;

    int ay = analogRead(JOY_Y_PIN); ty += ay;
    if (ay < yMin) yMin = ay;  if (ay > yMax) yMax = ay;

    delay(3);
  }

  merkezX = tx / 64;
  merkezY = ty / 64;

  Serial.print(F("      Merkez X=")); Serial.print(merkezX);
  Serial.print(F(" Y="));             Serial.print(merkezY);
  Serial.print(F("   Titresim X="));  Serial.print(xMax - xMin);
  Serial.print(F(" Y="));             Serial.println(yMax - yMin);

  bool supheli = false;

  if (merkezX < 300 || merkezX > 724 || merkezY < 300 || merkezY > 724) {
    Serial.println(F("      >> UYARI: merkez 512'den cok uzak."));
    Serial.println(F("         Acilirken joystick'e basiyor olabilirsin."));
    supheli = true;
  }
  if ((xMax - xMin) > 60 || (yMax - yMin) > 60) {
    Serial.println(F("      >> UYARI: okuma gurultulu."));
    supheli = true;
  }

  olcekX = min(merkezX, 1023 - merkezX);
  olcekY = min(merkezY, 1023 - merkezY);
  if (olcekX < 150) { merkezX = 512; olcekX = 470; supheli = true; }
  if (olcekY < 150) { merkezY = 512; olcekY = 470; supheli = true; }

  Serial.println(supheli ? F("      SONUC: joystick SUPHELI")
                         : F("      SONUC: joystick OK"));
}

uint8_t butonlariOku() {
  uint8_t m = 0;
#if BUTON_KULLAN
  if (digitalRead(BUTON_1_PIN) == LOW) m |= BIT_BUTON1;
  if (digitalRead(BUTON_2_PIN) == LOW) m |= BIT_BUTON2;
  if (digitalRead(BUTON_4_PIN) == LOW) m |= BIT_BUTON4;
#endif
  return m;
}

// --------------------------- LCD ------------------------------------
#if LCD_KULLAN

// Ust satir: baglanti durumu ve engel mesafesi ('!' = ileri engellendi)
void lcdUstYaz(bool bagli, unsigned int mes) {
  char buf[17];
  if (!bagli)
    snprintf(buf, sizeof(buf), "KOPUK   M:---cm ");
  else if (mes == 0 || mes >= MESAFE_YOK)
    snprintf(buf, sizeof(buf), "BAGLI   M:---cm ");
  else
    snprintf(buf, sizeof(buf), "BAGLI  %cM:%3ucm ",
             (mes <= DUR_MESAFE) ? '!' : ' ', mes);
  lcd.setCursor(0, 0);
  lcd.print(buf);
}

void lcdHizYaz(int sol, int sag) {
  char buf[17];
  snprintf(buf, sizeof(buf), "S:%-6dG:%-6d", sol, sag);
  lcd.setCursor(0, 1);
  lcd.print(buf);
}

void lcdGerilimYaz(unsigned int vcc, unsigned int mn) {
  char buf[17], v[6], m[6];
  if (vcc >= 2000) snprintf(v, sizeof(v), "%4u", vcc); else strcpy(v, "----");
  if (mn  >= 2000) snprintf(m, sizeof(m), "%4u", mn);  else strcpy(m, "----");
  snprintf(buf, sizeof(buf), "V:%s  MIN:%s", v, m);
  lcd.setCursor(0, 1);
  lcd.print(buf);
}
#endif

// ------------------------- TELSIZ -----------------------------------
void radyoKur() {
  Serial.println(F("[3/6] nRF24L01 baslatiliyor..."));

  if (!radio.begin()) {
    cizgi();
    Serial.println(F("HATA 1: nRF24 modulu hic yanit vermiyor."));
    Serial.println(F("  - Modul kartin soketine TAM oturmus mu?"));
    Serial.println(F("  - Kartin anahtari acik mi, pil dolu mu?"));
    Serial.print  (F("  - CE=")); Serial.print(CE_PIN);
    Serial.print  (F(" CSN=")); Serial.println(CSN_PIN);
    cizgi();
    while (1) delay(1000);
  }

  if (!radio.isChipConnected()) {
    cizgi();
    Serial.println(F("HATA 2: SPI hatti kararsiz, cip okunamiyor."));
    Serial.println(F("  - Modulu cikarip tekrar tak, pili sarj et"));
    cizgi();
    while (1) delay(1000);
  }

  radio.setChannel(KANAL);
  radio.setDataRate(VERI_HIZI);
  radio.setPALevel(RF_GUC);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(true);

  radio.enableDynamicPayloads();
  radio.enableAckPayload();

  radio.setRetries(5, 15);         // 1500 us x 15 deneme

  if (radio.getChannel() != KANAL) {
    cizgi();
    Serial.println(F("HATA 3: Ayarlar module yazilamiyor."));
    Serial.println(F("  Besleme yetersiz. Pili sarj et, JP5'i kontrol et."));
    cizgi();
    while (1) delay(1000);
  }

  radio.openWritingPipe(ADRES);
  radio.stopListening();

  Serial.println(F("      SONUC: telsiz OK"));
}

void radyoBilgisiYaz() {
  cizgi();
  Serial.println(F("TELSIZ AYARLARI (AKTARICI ile AYNI olmali):"));
  Serial.print(F("  Kanal        : ")); Serial.println(radio.getChannel());
  Serial.print(F("  Veri hizi    : "));
  switch (radio.getDataRate()) {
    case RF24_250KBPS: Serial.println(F("250KBPS")); break;
    case RF24_1MBPS:   Serial.println(F("1MBPS"));   break;
    case RF24_2MBPS:   Serial.println(F("2MBPS"));   break;
  }
  Serial.print(F("  RF gucu      : "));
  switch (radio.getPALevel()) {
    case RF24_PA_MIN:  Serial.println(F("MIN"));  break;
    case RF24_PA_LOW:  Serial.println(F("LOW"));  break;
    case RF24_PA_HIGH: Serial.println(F("HIGH")); break;
    case RF24_PA_MAX:  Serial.println(F("MAX"));  break;
  }
  Serial.print(F("  Adres        : ")); Serial.println((const char*)ADRES);
  Serial.print(F("  Komut paketi : ")); Serial.print(sizeof(KomutPaketi));
  Serial.println(F(" bayt"));
  Serial.print(F("  Telemetri    : ")); Serial.print(sizeof(TelemetriPaketi));
  Serial.println(F(" bayt"));
  Serial.print(F("  Modul tipi   : "));
  if (radio.isPVariant()) {
    Serial.println(F("nRF24L01+ (gercek)"));
  } else {
    Serial.println(F("nRF24L01 veya KLON"));
    Serial.println(F("  >> !! Bu modulde RF24_250KBPS CALISMAZ !!"));
    Serial.println(F("  >> VERI_HIZI'ni RF24_1MBPS yap (iki tarafta da)."));
  }
  cizgi();
}

// ---------------------- PIN KESIF MODU ------------------------------
#if PIN_KESIF_MODU
void kesifYaz() {
  Serial.print(F("A0="));   Serial.print(analogRead(A0));
  Serial.print(F(" A1="));  Serial.print(analogRead(A1));
  Serial.print(F(" A2="));  Serial.print(analogRead(A2));
  Serial.print(F(" A3="));  Serial.print(analogRead(A3));
  Serial.print(F(" A6="));  Serial.print(analogRead(A6));
  Serial.print(F(" A7="));  Serial.print(analogRead(A7));
  Serial.print(F("  | D5=")); Serial.print(digitalRead(5));
  Serial.print(F(" D6="));    Serial.print(digitalRead(6));
  Serial.print(F(" D7="));    Serial.println(digitalRead(7));
}
#endif

// =====================================================================
//  8) SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) { }

  delay(300);
  cizgi();
  Serial.println(F("  VERICI : FLAX KUMANDA + nRF24 + LCD"));
  Serial.println(F("  Serial Monitor 115200 baud olmali"));
  cizgi();
  Serial.println(F("[1/6] Kart uyandi."));

#if BUTON_KULLAN
  pinMode(BUTON_1_PIN, INPUT_PULLUP);
  pinMode(BUTON_2_PIN, INPUT_PULLUP);
  pinMode(BUTON_4_PIN, INPUT_PULLUP);
  Serial.println(F("      Butonlar acik (Buton 1/2/4)."));
#else
  Serial.println(F("      Butonlar kapali (BUTON_KULLAN 0)."));
#endif

#if PIN_KESIF_MODU
  pinMode(5, INPUT_PULLUP);
  pinMode(6, INPUT_PULLUP);
  pinMode(7, INPUT_PULLUP);
  cizgi();
  Serial.println(F("  PIN KESIF MODU ACIK -> telsiz ve LCD calismiyor."));
  Serial.println(F("  Joystick'i oynat, degisen kanali gor."));
  Serial.println(F("  Bittiginde PIN_KESIF_MODU'nu 0 yap."));
  cizgi();
  return;
#endif

#if LCD_KULLAN
  Serial.println(F("[2/6] LCD baslatiliyor..."));
  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("KALIBRASYON...  ");
  lcd.setCursor(0, 1);
  lcd.print("JOYSTICK SERBEST");

  Wire.beginTransmission(LCD_ADRES);
  if (Wire.endTransmission() == 0) {
    Serial.print(F("      LCD 0x"));
    Serial.print(LCD_ADRES, HEX);
    Serial.println(F(" adresinde bulundu."));
  } else {
    Serial.print(F("      >> UYARI: 0x"));
    Serial.print(LCD_ADRES, HEX);
    Serial.println(F(" adresinde cevap yok."));
    Serial.println(F("         LCD_ADRES'i 0x3F yapip dene."));
    Serial.println(F("         Ya da SDA/SCL kablolari ters/temassiz."));
  }
#else
  Serial.println(F("[2/6] LCD kapali (LCD_KULLAN 0)."));
#endif

  radyoKur();
  radyoBilgisiYaz();
  joystickKalibreEt();

#if LCD_KULLAN
  lcd.clear();
  lcdUstYaz(false, MESAFE_YOK);
  lcdHizYaz(0, 0);
  gosterilenBagli  = false;
  gosterilenMesafe = MESAFE_YOK;
  sonModDegis = millis();
#endif

  Serial.println(F("[5/6] Aktariciyi ve arabayi ac, bekle..."));
  Serial.println(F("[6/6] Hazir."));
  cizgi();
}

// =====================================================================
//  9) LOOP
// =====================================================================
void loop() {

#if PIN_KESIF_MODU
  static unsigned long sonKesif = 0;
  if (millis() - sonKesif >= 250) { sonKesif = millis(); kesifYaz(); }
  return;
#endif

  unsigned long simdi = millis();

  // ------------------- Komut paketi gonderimi -------------------------
  if (simdi - sonGonderim >= GONDERIM_ARALIGI) {
    sonGonderim = simdi;

    KomutPaketi p;
    p.imza  = KOMUT_IMZA;
    p.sayac = sayac++;

    p.joyX = (uint16_t)analogRead(JOY_X_PIN);
    p.joyY = (uint16_t)analogRead(JOY_Y_PIN);

#if POT_KULLAN
    p.pot = (uint16_t)analogRead(POT_PIN);
#else
    p.pot = 0;
#endif

    p.butonlar = butonlariOku();

    int ileri = eksenCevir(p.joyY, merkezY, olcekY);
    int donus = eksenCevir(p.joyX, merkezX, olcekX);

    if (abs(ileri) < SAFE_ZONE) ileri = 0;
    if (abs(donus) < SAFE_ZONE) donus = 0;

    // Diferansiyel surus karisimi
    int sol = 0, sag = 0;

    if (ileri == 0 && donus == 0) {
      sol = 0; sag = 0;
    } else if (ileri == 0) {
      sol =  donus;
      sag = -donus;
    } else if (ileri > 0) {
      sol = constrain(ileri + donus, 0, 255);
      sag = constrain(ileri - donus, 0, 255);
    } else {
      sol = constrain(ileri + donus, -255, 0);
      sag = constrain(ileri - donus, -255, 0);
    }

    p.sol = (int16_t)sol;
    p.sag = (int16_t)sag;
    sonSol = sol;  sonSag = sag;

    bool ok = radio.write(&p, sizeof(p));

    if (ok) {
      basarili++;
      ustusteHata = 0;
      uyariBasildi = false;

      while (radio.isAckPayloadAvailable()) {
        uint8_t boy = radio.getDynamicPayloadSize();
        if (boy == sizeof(TelemetriPaketi)) {
          TelemetriPaketi gelen;
          radio.read(&gelen, sizeof(gelen));
          if (gelen.imza == TLM_IMZA) {
            tlm = gelen;
            tlmGecerli = true;
            sonTlm = simdi;
          }
        } else {
          uint8_t cop[32];
          radio.read(cop, (boy > 32) ? 32 : boy);
        }
      }
    } else {
      basarisiz++;
      ustusteHata++;
    }

    if (tlmGecerli && (simdi - sonTlm > 1000)) tlmGecerli = false;

    if (simdi - sonLog >= LOG_ARALIGI) {
      sonLog = simdi;

      Serial.print(F("Joystick "));       Serial.print(p.joyX);
      Serial.print(',');                  Serial.print(p.joyY);
#if POT_KULLAN
      Serial.print(F(" | Pot "));         Serial.print(p.pot);
#endif
      Serial.print(F(" | Motor sol "));   Serial.print(p.sol);
      Serial.print(F(" sag "));           Serial.print(p.sag);
#if BUTON_KULLAN
      if (p.butonlar) {
        Serial.print(F(" | Buton"));
        if (p.butonlar & BIT_BUTON1) Serial.print(F(" 1"));
        if (p.butonlar & BIT_BUTON2) Serial.print(F(" 2"));
        if (p.butonlar & BIT_BUTON4) Serial.print(F(" 4"));
      }
#endif
      Serial.print(ok ? F(" | Aktarici VAR") : F(" | Aktarici YOK"));

      unsigned long toplam = basarili + basarisiz;
      if (toplam > 0) {
        Serial.print(F(" %"));
        Serial.print((basarili * 100UL) / toplam);
      }

      if (tlmGecerli) {
        Serial.print(F(" | Araba "));     voltYaz(tlm.vcc);
        Serial.print(F(" (en dusuk "));   voltYaz(tlm.minVcc);
        Serial.print(F(") | Engel "));
        if (tlm.mesafe >= MESAFE_YOK) Serial.print(F("yok"));
        else { Serial.print(tlm.mesafe); Serial.print(F(" cm")); }
        if (tlm.durum & TLM_BIT_ENGEL) Serial.print(F(" DUR"));
      } else {
        Serial.print(F(" | Araba YOK"));
      }
      Serial.println();
    }

    // 50 ardisik hata = ~1 sn boyunca aktaricidan ACK yok
    if (ustusteHata == 50 && !uyariBasildi) {
      uyariBasildi = true;
      cizgi();
      Serial.println(F("!! 1 SANIYEDIR AKTARICIDAN HIC ACK GELMIYOR."));
      Serial.println(F("   1) Aktarici acik mi, aktarici kodu yuklu mu?"));
      Serial.println(F("   2) KANAL iki tarafta da 108 mi?"));
      Serial.println(F("   3) VERI_HIZI iki tarafta ayni mi?"));
      Serial.println(F("   4) Kumanda ADRES = aktarici ADRES_KUMANDA = AKT01 mi?"));
      Serial.println(F("   5) struct KomutPaketi iki tarafta ayni mi?"));
      Serial.println(F("   6) PA_MAX'ta cok yakinsan anten doyar -> uzaklas."));
      Serial.println(F("   7) 3.3V besleme saglam mi?"));
      cizgi();
    }
  }

  // ------------------- LCD guncelleme ---------------------------------
#if LCD_KULLAN
  if (simdi - sonLcdYazim >= LCD_ARALIGI) {
    sonLcdYazim = simdi;

    unsigned int mes = tlmGecerli ? tlm.mesafe : MESAFE_YOK;

    // I2C trafigini azaltmak icin ust satir yalnizca degistiginde yazilir.
    if (ilkLcdYazim || tlmGecerli != gosterilenBagli || mes != gosterilenMesafe) {
      lcdUstYaz(tlmGecerli, mes);
      gosterilenBagli  = tlmGecerli;
      gosterilenMesafe = mes;
    }

    if (simdi - sonModDegis >= LCD_MOD_SURE) {
      sonModDegis = simdi;
      altMod = 1 - altMod;
    }

    if (altMod == 0) lcdHizYaz(sonSol, sonSag);
    else             lcdGerilimYaz(tlmGecerli ? tlm.vcc : 0,
                                   tlmGecerli ? tlm.minVcc : 0);

    ilkLcdYazim = false;
  }
#endif
}
