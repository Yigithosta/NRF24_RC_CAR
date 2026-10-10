// =====================================================================
//  FLAX Aktarici Sistem - AKTARICI (Menzil Uzatici)
//  Arduino Nano + nRF24L01+PA+LNA
//
//  KUMANDA --(AKT01)--> AKTARICI --(ARB02)--> ARABA
//
//  Kumandadan gelen komut paketini degistirmeden arabaya iletir.
//  Arabanin telemetrisini kendi ACK payload'ina koyarak kumandaya
//  geri tasir. Bayat telemetri (>300 ms) iletilmez.
//
//  Gereksinim : RF24 (TMRh20) >= 1.4.0
//  Seri port  : 115200 baud
// =====================================================================

#include <SPI.h>
#include <RF24.h>

#define DEBUG 1

// ========================= TELSIZ =====================================
#define CE_PIN 9
#define CSN_PIN 10

const uint8_t ADRES_KUMANDA[6] = "AKT01";
const uint8_t ADRES_ARABA[6] = "ARB02";

const uint8_t KANAL = 108;
#define VERI_HIZI RF24_250KBPS
#define RF_GUC RF24_PA_MIN

// Arabaya giden hat: (3+1) x 250 us gecikme, 5 deneme.
// Kisa tutuldu ki araba yanit vermezse aktarici en fazla ~12 ms
// mesgul kalip kumandayi dinlemeye donsun.
#define ARABA_TEKRAR_GECIKME 3
#define ARABA_TEKRAR_SAYI 5

RF24 radio(CE_PIN, CSN_PIN);

// ========================= PAKET YAPILARI =============================
const uint8_t KOMUT_IMZA = 0xA5;
const uint8_t TLM_IMZA = 0x5A;

struct __attribute__((packed)) KomutPaketi {
  uint8_t imza;
  uint8_t sayac;
  uint16_t joyX, joyY;
  uint16_t pot;
  uint8_t butonlar;
  int16_t sol, sag;
};

struct __attribute__((packed)) TelemetriPaketi {
  uint8_t imza;
  uint16_t vcc;
  uint16_t minVcc;
  uint16_t mesafe;
  uint8_t durum;
};

static_assert(sizeof(KomutPaketi) == 13, "KomutPaketi 13 bayt olmali");
static_assert(sizeof(TelemetriPaketi) == 8, "TelemetriPaketi 8 bayt olmali");

// ========================= DURUM ======================================
TelemetriPaketi tlm;
bool tlmVar = false;
unsigned long sonTlm = 0;
bool ackYuklu = false;
const unsigned long TLM_BAYAT = 300;

unsigned long gelen = 0;
unsigned long iletilen = 0;
unsigned long iletilemeyen = 0;
unsigned long cop = 0;
unsigned long sonOzet = 0;
unsigned long ozetGelen = 0, ozetIletilen = 0, ozetIletilemeyen = 0;

// =====================================================================
//  YARDIMCI FONKSIYONLAR
// =====================================================================

void cizgi() {
  Serial.println(F("---------------------------------------------"));
}

void voltYaz(unsigned int mv) {
  if (mv < 2000) {
    Serial.print('-');
    return;
  }
  unsigned int yuzde = (mv % 1000) / 10;
  Serial.print(mv / 1000);
  Serial.print('.');
  if (yuzde < 10) Serial.print('0');
  Serial.print(yuzde);
  Serial.print('V');
}

void copAt(uint8_t boy) {
  uint8_t tampon[32];
  radio.read(tampon, (boy > 32) ? 32 : boy);
  cop++;
}

void arabayaIlet(const KomutPaketi &p) {
  radio.stopListening();
  // Kumanda icin bekleyen ACK payload TX FIFO'da kalirsa write() onu
  // arabaya normal paket gibi yollar; surumden bagimsiz olarak temizle.
  radio.flush_tx();
  ackYuklu = false;

  bool ok = radio.write(&p, sizeof(p));

  if (ok) {
    iletilen++;
    ozetIletilen++;

    uint8_t boru;
    while (radio.available(&boru)) {
      uint8_t boy = radio.getDynamicPayloadSize();
      if (boru == 0 && boy == sizeof(TelemetriPaketi)) {
        TelemetriPaketi t;
        radio.read(&t, sizeof(t));
        if (t.imza == TLM_IMZA) {
          tlm = t;
          tlmVar = true;
          sonTlm = millis();
        } else {
          cop++;
        }
      } else {
        copAt(boy);
      }
    }
  } else {
    iletilemeyen++;
    ozetIletilemeyen++;
  }

  radio.startListening();

  // Yalnizca taze telemetri iletilir; araba yanit vermiyorsa kumanda
  // bunu telemetri yoklugundan anlar.
  if (tlmVar && (millis() - sonTlm <= TLM_BAYAT)) {
    radio.writeAckPayload(1, &tlm, sizeof(tlm));
    ackYuklu = true;
  }
}

void radyoKur() {
  Serial.println(F("[2/3] nRF24L01 baslatiliyor..."));

  if (!radio.begin()) {
    cizgi();
    Serial.println(F("HATA 1: nRF24 modulu hic yanit vermiyor."));
    Serial.println(F("  - CE=D9, CSN=D10, MOSI=D11, MISO=D12, SCK=D13"));
    Serial.println(F("  - VCC kesinlikle 3.3V olacak (5V modulu yakar)"));
    Serial.println(F("  - VCC-GND arasina 10uF kondansator tak"));
    cizgi();
    while (1) delay(1000);
  }

  if (!radio.isChipConnected()) {
    cizgi();
    Serial.println(F("HATA 2: SPI hatti kararsiz, cip okunamiyor."));
    Serial.println(F("  - MISO(D12) ve MOSI(D11) ters olabilir"));
    Serial.println(F("  - Kablolari kisalt"));
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

  radio.setRetries(ARABA_TEKRAR_GECIKME, ARABA_TEKRAR_SAYI);

  if (radio.getChannel() != KANAL) {
    cizgi();
    Serial.println(F("HATA 3: Ayarlar module yazilamiyor."));
    Serial.println(F("  Besleme yetersiz olabilir. 3.3V + 10uF sart."));
    cizgi();
    while (1) delay(1000);
  }

  // Pipe 0 gonderim ve arabanin ACK'i icin ayrildi; kumanda pipe 1'den dinlenir.
  radio.openWritingPipe(ADRES_ARABA);
  radio.openReadingPipe(1, ADRES_KUMANDA);
  radio.startListening();

  Serial.println(F("      SONUC: telsiz OK"));
}

void radyoBilgisiYaz() {
  cizgi();
  Serial.println(F("TELSIZ AYARLARI (kumanda ve araba ile AYNI olmali):"));
  Serial.print(F("  Kanal        : "));
  Serial.println(radio.getChannel());
  Serial.print(F("  Veri hizi    : "));
  switch (radio.getDataRate()) {
    case RF24_250KBPS: Serial.println(F("250KBPS")); break;
    case RF24_1MBPS: Serial.println(F("1MBPS")); break;
    case RF24_2MBPS: Serial.println(F("2MBPS")); break;
  }
  Serial.print(F("  Dinlenen     : "));
  Serial.print((const char *)ADRES_KUMANDA);
  Serial.println(F("  (kumanda)"));
  Serial.print(F("  Gonderilen   : "));
  Serial.print((const char *)ADRES_ARABA);
  Serial.println(F("  (araba)"));
  Serial.print(F("  Komut paketi : "));
  Serial.print(sizeof(KomutPaketi));
  Serial.println(F(" bayt"));
  Serial.print(F("  Telemetri    : "));
  Serial.print(sizeof(TelemetriPaketi));
  Serial.println(F(" bayt"));
  Serial.print(F("  Modul tipi   : "));
  if (radio.isPVariant()) {
    Serial.println(F("nRF24L01+ (gercek)"));
  } else {
    Serial.println(F("nRF24L01 veya KLON"));
    Serial.println(F("  >> !! Bu modulde RF24_250KBPS CALISMAZ !!"));
    Serial.println(F("  >> VERI_HIZI'ni RF24_1MBPS yap (UC cihazda da)."));
  }
  cizgi();
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  delay(300);
  cizgi();
  Serial.println(F("  AKTARICI : KUMANDA -> [AKTARICI] -> ARABA"));
  Serial.println(F("  Serial Monitor 115200 baud olmali"));
  cizgi();
  Serial.println(F("[1/3] Kart uyandi."));

  radyoKur();
  radyoBilgisiYaz();

  Serial.println(F("[3/3] Hazir. Kumandayi ve arabayi ac."));
  cizgi();
}

// =====================================================================
//  LOOP
// =====================================================================
void loop() {

  // FIFO'da birden fazla paket biriktiyse yalnizca en sonuncusu iletilir.
  bool yeni = false;
  KomutPaketi p;
  uint8_t boru;

  while (radio.available(&boru)) {
    uint8_t boy = radio.getDynamicPayloadSize();

    if (boru != 1 || boy != sizeof(KomutPaketi)) {
      copAt(boy);
      continue;
    }

    KomutPaketi gecici;
    radio.read(&gecici, sizeof(gecici));

    if (gecici.imza != KOMUT_IMZA) {
      cop++;
      continue;
    }

    p = gecici;
    yeni = true;
  }

  if (yeni) {
    gelen++;
    ozetGelen++;
    arabayaIlet(p);
  }

  unsigned long simdi = millis();

  // Kumanda uzun sure sustuysa ACK kuyrugunda eski telemetri kalmasin.
  if (ackYuklu && (simdi - sonTlm > TLM_BAYAT)) {
    radio.flush_tx();
    ackYuklu = false;
  }

#if DEBUG
  if (simdi - sonOzet >= 1000) {
    sonOzet = simdi;

    if (ozetGelen == 0) {
      Serial.println(F("Kumanda YOK"));
    } else {
      Serial.print(F("Kumanda "));
      Serial.print(ozetGelen);
      Serial.print(F(" paket/sn | Arabaya "));
      Serial.print(ozetIletilen);
      Serial.print(F(" ulasti, "));
      Serial.print(ozetIletilemeyen);
      Serial.print(F(" ulasmadi"));

      if (tlmVar && (simdi - sonTlm <= 1000)) {
        Serial.print(F(" | Araba "));
        voltYaz(tlm.vcc);
        Serial.print(F(" | Engel "));
        if (tlm.mesafe >= 999) Serial.print(F("yok"));
        else {
          Serial.print(tlm.mesafe);
          Serial.print(F(" cm"));
        }
        if (tlm.durum & 0x01) Serial.print(F(" DUR"));
      } else {
        Serial.print(F(" | Araba YOK"));
      }
      if (cop) {
        Serial.print(F(" | Hatali paket (toplam) "));
        Serial.print(cop);
      }
      Serial.println();
    }

    ozetGelen = 0;
    ozetIletilen = 0;
    ozetIletilemeyen = 0;
  }
#endif
}
