// =====================================================================
//  FLAX Aktarici Sistem - ALICI (Araba)
//  Arduino Uno + nRF24L01 + L298N (4 motor) + HC-SR04
//
//  KUMANDA --(AKT01)--> AKTARICI --(ARB02)--> ARABA
//
//  Aktaricidan gelen komutlari PWM rampasi ve yon degisimi korumasiyla
//  motorlara uygular. HC-SR04 ile engel algilar, besleme gerilimini
//  izler ve telemetriyi ACK payload olarak geri gonderir.
//  400 ms boyunca komut gelmezse araba durur.
//
//  Radar davranisi:
//    mesafe > 60 cm        -> serbest
//    25 < mesafe <= 60 cm  -> ileri hiz 130 PWM ile sinirli
//    mesafe <= 25 cm       -> ileri gidis engellenir
//  Geri gitmek ve yerinde donmek her zaman serbesttir.
//
//  Gereksinim : RF24 (TMRh20) >= 1.4.0
//  Seri port  : 115200 baud
// =====================================================================

#include <SPI.h>
#include <RF24.h>
#include <EEPROM.h>

#define DEBUG       1
#define FREN_AKTIF  1     // 1: baglanti kopunca aktif fren, 0: serbest birak

const int MAX_PWM = 255;

// ========================= TELSIZ =====================================
#define CE_PIN    9
#define CSN_PIN   10

const uint8_t ADRES[6] = "ARB02";
const uint8_t KANAL    = 108;
#define VERI_HIZI   RF24_250KBPS
#define RF_GUC      RF24_PA_MIN

RF24 radio(CE_PIN, CSN_PIN);

// ========================= PAKET YAPILARI =============================
const uint8_t KOMUT_IMZA = 0xA5;
const uint8_t TLM_IMZA   = 0x5A;

struct __attribute__((packed)) KomutPaketi {
  uint8_t  imza;
  uint8_t  sayac;
  uint16_t joyX, joyY;
  uint16_t pot;
  uint8_t  butonlar;
  int16_t  sol, sag;
};

#define BIT_BUTON1   0x01
#define BIT_BUTON2   0x02
#define BIT_BUTON4   0x04

struct __attribute__((packed)) TelemetriPaketi {
  uint8_t  imza;
  uint16_t vcc;
  uint16_t minVcc;
  uint16_t mesafe;
  uint8_t  durum;
};

#define TLM_BIT_ENGEL  0x01

static_assert(sizeof(KomutPaketi)     == 13, "KomutPaketi 13 bayt olmali");
static_assert(sizeof(TelemetriPaketi) == 8,  "TelemetriPaketi 8 bayt olmali");

TelemetriPaketi tlm;

// ========================= RADAR (HC-SR04) ============================
const int TRIG_PIN = A0;
const int ECHO_PIN = A1;

const unsigned long OLCUM_PERIYOT = 60;      // ms
const unsigned long ECHO_TIMEOUT  = 4500;    // us, ~77 cm
const unsigned int  MESAFE_YOK    = 999;

const unsigned int DUR_MESAFE     = 25;      // cm
const unsigned int YAVASLA_MESAFE = 60;      // cm
const int          YAVAS_PWM      = 130;

unsigned int mesafe = MESAFE_YOK;
unsigned long sonOlcum = 0;
bool engelVar = false;
uint8_t yakinSayac = 0;
uint8_t uzakSayac  = 0;

// ========================= EEPROM =====================================
const int EEPROM_MIN_ADRES = 0;
unsigned int oncekiMinVcc = 0;

// ========================= L298N ======================================
const int ENA = 5;
const int IN1 = 2;
const int IN2 = 3;
const int ENB = 6;
const int IN3 = 4;
const int IN4 = 7;

// ========================= DURUM ======================================
unsigned long sonMesaj = 0;
const unsigned long TIMEOUT = 400;
bool linkAktif = false;

unsigned long toplamPaket = 0, bozukPaket = 0, kayipPaket = 0;
uint8_t sonSayac = 0;
bool    ilkPaket = true;
unsigned long sayimBaslangic = 0, sayimPaket = 0;

uint8_t sonButonlar = 0;

int komutSol = 0, komutSag = 0;            // gelen ham komut
int hedefSol = 0, hedefSag = 0;            // radar sinirlamasi sonrasi
int simdikiSol = 0, simdikiSag = 0;        // motorlara uygulanan
unsigned long yonBekleSol = 0, yonBekleSag = 0;
unsigned long sonRampa = 0;
const unsigned long RAMPA_PERIYOT = 20;    // ms
const int RAMPA_ADIM = 30;
const unsigned long YON_BEKLEME = 70;      // ms

unsigned long frenBitis = 0;
const unsigned long FREN_SURE = 250;

unsigned int anlikVcc = 0;
unsigned int minVcc = 9999;
unsigned long sonVccOlcum = 0;
const unsigned long VCC_PERIYOT = 2;       // ms
unsigned long sonDebug = 0;
unsigned long ozetNo = 0;

// =====================================================================
//  YARDIMCI FONKSIYONLAR
// =====================================================================

void cizgi() {
  Serial.println(F("---------------------------------------------"));
}

void voltYaz(unsigned int mv) {
  if (mv < 2000 || mv == 9999) { Serial.print('-'); return; }
  unsigned int yuzde = (mv % 1000) / 10;
  Serial.print(mv / 1000); Serial.print('.');
  if (yuzde < 10) Serial.print('0');
  Serial.print(yuzde);     Serial.print('V');
}

// Dahili 1.1V bandgap referansini Vcc'ye gore okuyarak besleme
// gerilimini olcer (ATmega328P).
unsigned int okuVcc() {
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delayMicroseconds(300);
  ADCSRA |= _BV(ADSC);
  while (ADCSRA & _BV(ADSC));
  unsigned int sonuc = ADC;
  if (sonuc == 0) return 0;
  return (unsigned int)(1125300UL / sonuc);   // 1.1 * 1023 * 1000 / ADC
}

// Yalnizca yeni bir dip deger goruldugunde yazilir.
void minVccKaydet(unsigned int deger) {
  EEPROM.put(EEPROM_MIN_ADRES, deger);
}

// ACK payload onceden yuklenir; bir sonraki paket geldiginde
// donanim bu veriyi otomatik olarak cevaba ekler.
void telemetriyiHazirla() {
  tlm.imza   = TLM_IMZA;
  tlm.vcc    = anlikVcc;
  tlm.minVcc = (minVcc == 9999) ? 0 : minVcc;
  tlm.mesafe = mesafe;
  tlm.durum  = engelVar ? TLM_BIT_ENGEL : 0;

  radio.writeAckPayload(1, &tlm, sizeof(tlm));
}

void mesafeOlc() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long sure = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT);

  if (sure == 0) {
    mesafe = MESAFE_YOK;
  } else {
    unsigned int cm = (unsigned int)(sure / 58UL);
    if (cm < 2) cm = 2;
    mesafe = cm;
  }

  // Histerezis: tek bir hatali olcum karari degistirmez.
  if (mesafe <= DUR_MESAFE) {
    if (yakinSayac < 10) yakinSayac++;
    uzakSayac = 0;
  } else if (mesafe > DUR_MESAFE + 5) {
    if (uzakSayac < 10) uzakSayac++;
    yakinSayac = 0;
  }

  if (yakinSayac >= 2) engelVar = true;
  if (uzakSayac  >= 3) engelVar = false;
}

// Yalnizca ileri giden tekerler kisitlanir; geri ve yerinde donus serbest.
void radarSinirla(int &sol, int &sag) {
  bool yerindeDonus = ((sol > 0 && sag < 0) || (sol < 0 && sag > 0));
  if (yerindeDonus) return;

  if (engelVar) {
    if (sol > 0) sol = 0;
    if (sag > 0) sag = 0;
  } else if (mesafe <= YAVASLA_MESAFE) {
    if (sol > YAVAS_PWM) sol = YAVAS_PWM;
    if (sag > YAVAS_PWM) sag = YAVAS_PWM;
  }
}

// +hiz ileri, -hiz geri. Teker ters donerse HIGH/LOW yer degistirilir.
void solMotor(int hiz) {
  if (hiz >= 0) { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); }
  else          { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); hiz = -hiz; }
  analogWrite(ENA, hiz);
}

void sagMotor(int hiz) {
  if (hiz >= 0) { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
  else          { digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW); hiz = -hiz; }
  analogWrite(ENB, hiz);
}

void dur() {
  analogWrite(ENA, 0);     analogWrite(ENB, 0);
  digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);
}

// Aktif fren: yon uclari ayni seviyede, motor sargisi kisa devre.
void fren() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, HIGH);
  analogWrite(ENA, 255);   analogWrite(ENB, 255);
}

// Hizi hedefe RAMPA_ADIM adimlarla yaklastirir. Yon degisiminde once
// 0'a iner ve YON_BEKLEME kadar bekler.
int rampa(int simdiki, int hedef, unsigned long &bekle) {
  unsigned long simdi = millis();

  bool yonDegisimi = ((long)hedef * (long)simdiki < 0);
  if (yonDegisimi) hedef = 0;

  if (simdiki == 0) {
    if (bekle != 0) {
      if (simdi < bekle) return 0;
      bekle = 0;
    }
  }

  int yeni = simdiki;
  if (hedef > yeni)      { yeni += RAMPA_ADIM; if (yeni > hedef) yeni = hedef; }
  else if (hedef < yeni) { yeni -= RAMPA_ADIM; if (yeni < hedef) yeni = hedef; }

  if (yonDegisimi && yeni == 0 && simdiki != 0) bekle = simdi + YON_BEKLEME;

  return yeni;
}

void motorlariGuncelle() {
  unsigned long simdi = millis();

  if (frenBitis != 0) {
    if (simdi < frenBitis) return;
    frenBitis = 0;
    dur();
    return;
  }

  if (simdi - sonRampa < RAMPA_PERIYOT) return;
  sonRampa = simdi;

  simdikiSol = rampa(simdikiSol, hedefSol, yonBekleSol);
  simdikiSag = rampa(simdikiSag, hedefSag, yonBekleSag);

  solMotor(simdikiSol);
  sagMotor(simdikiSag);
}

void linkKoptu() {
  linkAktif  = false;
  ilkPaket   = true;
  komutSol   = 0;  komutSag   = 0;
  hedefSol   = 0;  hedefSag   = 0;
  simdikiSol = 0;  simdikiSag = 0;
  yonBekleSol = 0; yonBekleSag = 0;

#if FREN_AKTIF
  fren();
  frenBitis = millis() + FREN_SURE;
#else
  dur();
  frenBitis = 0;
#endif

#if DEBUG
  Serial.println(F(">>> BAGLANTI KOPTU - motorlar durdu"));
#endif
}

void radyoKur() {
  Serial.println(F("[2/4] nRF24L01 baslatiliyor..."));

  if (!radio.begin()) {
    cizgi();
    Serial.println(F("HATA 1: nRF24 modulu hic yanit vermiyor."));
    Serial.println(F("  - CE=D9, CSN=D10, MOSI=D11, MISO=D12, SCK=D13"));
    Serial.println(F("  - VCC kesinlikle 3.3V olacak (5V modulu yakar)"));
    Serial.println(F("  - VCC-GND arasina 10uF kondansator tak"));
    Serial.println(F("  - Jumper'lari tek tek oynat, temassizlik cok yaygin"));
    cizgi();
    while (1) { dur(); delay(1000); }
  }

  if (!radio.isChipConnected()) {
    cizgi();
    Serial.println(F("HATA 2: SPI hatti kararsiz, cip okunamiyor."));
    Serial.println(F("  - MISO(D12) ve MOSI(D11) ters olabilir"));
    Serial.println(F("  - Kablolari kisalt"));
    cizgi();
    while (1) { dur(); delay(1000); }
  }

  radio.setChannel(KANAL);
  radio.setDataRate(VERI_HIZI);
  radio.setPALevel(RF_GUC);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(true);

  radio.enableDynamicPayloads();
  radio.enableAckPayload();

  if (radio.getChannel() != KANAL) {
    cizgi();
    Serial.println(F("HATA 3: Ayarlar module yazilamiyor."));
    Serial.println(F("  Besleme yetersiz olabilir. 3.3V + 10uF sart."));
    cizgi();
    while (1) { dur(); delay(1000); }
  }

  radio.openReadingPipe(1, ADRES);
  radio.startListening();

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

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) { }

  pinMode(ENA, OUTPUT); pinMode(ENB, OUTPUT);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  dur();
  linkAktif = false;

  delay(300);
  cizgi();
  Serial.println(F("  ALICI : ARABA + nRF24L01 + L298N + RADAR"));
  Serial.println(F("  Serial Monitor 115200 baud olmali"));
  cizgi();
  Serial.println(F("[1/4] Kart uyandi, motorlar durduruldu."));

  EEPROM.get(EEPROM_MIN_ADRES, oncekiMinVcc);
  if (oncekiMinVcc < 2000 || oncekiMinVcc > 6000) oncekiMinVcc = 0;
  Serial.print(F("      ONCEKI SURUSTE EN DUSUK Vcc: "));
  if (oncekiMinVcc == 0) Serial.println(F("kayit yok"));
  else { Serial.print(oncekiMinVcc); Serial.println(F(" mV")); }
  Serial.println(F("      (4600 mV alti = besleme cokuyor demektir)"));

  radyoKur();
  radyoBilgisiYaz();

  // Ilk pakete verilecek cevapta da telemetri bulunsun.
  anlikVcc = okuVcc();
  telemetriyiHazirla();

  Serial.println(F("[3/4] Aktariciyi ve kumandayi ac, joystick'i oynat."));
  Serial.println(F("[4/4] Hazir."));
  cizgi();

  minVccKaydet(0xFFFF);
  minVcc = 9999;

  sayimBaslangic = millis();
}

// =====================================================================
//  LOOP
// =====================================================================
void loop() {
  unsigned long simdi = millis();

  // ------------------- Paket alimi ------------------------------------
  // Tampondaki tum paketler okunur, yalnizca en sonuncusu uygulanir.
  bool yeni = false;
  KomutPaketi p;

  while (radio.available()) {
    uint8_t boy = radio.getDynamicPayloadSize();

    if (boy != sizeof(KomutPaketi)) {
      uint8_t cop[32];
      radio.read(cop, (boy > 32) ? 32 : boy);
      bozukPaket++;
      continue;
    }

    KomutPaketi gecici;
    radio.read(&gecici, sizeof(gecici));
    toplamPaket++;

    if (gecici.imza != KOMUT_IMZA) {
      bozukPaket++;
      continue;
    }

    if (!ilkPaket) {
      uint8_t fark = (uint8_t)(gecici.sayac - sonSayac);
      if (fark > 1) kayipPaket += (fark - 1);
    }
    sonSayac = gecici.sayac;
    ilkPaket = false;

    p = gecici;
    yeni = true;
    sayimPaket++;
  }

  // ------------------- Komut uygulama ---------------------------------
  if (yeni) {
    sonMesaj = simdi;

    if (!linkAktif) {
      linkAktif = true;
      Serial.println(F(">>> BAGLANDI"));
    }

    if (p.sol >= -255 && p.sol <= 255 && p.sag >= -255 && p.sag <= 255) {
      komutSol = constrain(p.sol, -MAX_PWM, MAX_PWM);
      komutSag = constrain(p.sag, -MAX_PWM, MAX_PWM);
    }

    sonButonlar = p.butonlar;

    telemetriyiHazirla();
  }

  // ------------------- Baglanti zaman asimi ---------------------------
  if (linkAktif && (simdi - sonMesaj > TIMEOUT)) {
    linkKoptu();
  }

  // ------------------- Radar ------------------------------------------
  if (simdi - sonOlcum >= OLCUM_PERIYOT) {
    sonOlcum = simdi;
    mesafeOlc();
  }

  // Joystick sabit olsa bile yeni bir engele aninda tepki verilmesi icin
  // sinirlama her dongude yeniden hesaplanir.
  hedefSol = komutSol;
  hedefSag = komutSag;
  radarSinirla(hedefSol, hedefSag);

  // ------------------- Besleme izleme ---------------------------------
  if (simdi - sonVccOlcum >= VCC_PERIYOT) {
    sonVccOlcum = simdi;
    unsigned int v = okuVcc();
    if (v > 2000) {
      anlikVcc = v;
      if (v < minVcc) {
        minVcc = v;
        minVccKaydet(minVcc);
      }
    }
  }

  motorlariGuncelle();

  // ------------------- Paket istatistigi ------------------------------
  if (simdi - sayimBaslangic >= 3000) {
#if DEBUG
    if (linkAktif) {
      Serial.print(F("Paket "));            Serial.print(sayimPaket / 3);
      Serial.print(F("/sn | Kayip (toplam) ")); Serial.print(kayipPaket);
      if (bozukPaket) { Serial.print(F(" | Hatali (toplam) ")); Serial.print(bozukPaket); }
      Serial.println();
    } else {
      Serial.println(F("Paket YOK"));
    }
#endif
    sayimBaslangic = simdi;
    sayimPaket = 0;
  }

  // ------------------- Debug ozeti ------------------------------------
#if DEBUG
  if (simdi - sonDebug >= 500) {
    sonDebug = simdi;
    Serial.print('#');                      Serial.print(ozetNo++);
    Serial.print(linkAktif ? F(" | Baglanti VAR") : F(" | Baglanti YOK"));
    Serial.print(F(" | Motor sol "));       Serial.print(simdikiSol);
    Serial.print(F(" sag "));               Serial.print(simdikiSag);
    Serial.print(F(" | Pil "));             voltYaz(anlikVcc);
    Serial.print(F(" (en dusuk "));         voltYaz(minVcc);
    Serial.print(F(") | Engel "));
    if (mesafe >= MESAFE_YOK) Serial.print(F("yok"));
    else { Serial.print(mesafe); Serial.print(F(" cm")); }
    if (engelVar)                        Serial.print(F(" DUR"));
    else if (mesafe <= YAVASLA_MESAFE)   Serial.print(F(" YAVAS"));
    if (sonButonlar) {
      Serial.print(F(" | Buton"));
      if (sonButonlar & BIT_BUTON1) Serial.print(F(" 1"));
      if (sonButonlar & BIT_BUTON2) Serial.print(F(" 2"));
      if (sonButonlar & BIT_BUTON4) Serial.print(F(" 4"));
    }
    Serial.println();
  }
#endif
}
