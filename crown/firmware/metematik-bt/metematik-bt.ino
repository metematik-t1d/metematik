/*
  MeteMatik - CGM glucose display for CrowPanel Advance 2.8" (ESP32-S3, V1.2)
  Ikincil bir ekrandir, tibbi cihaz degildir. / A secondary display, not a medical device.

  Copyright (C) 2026 Burak - "Oglum Mete icin yaptim." / "Made for my son Mete."
  SPDX-License-Identifier: GPL-3.0-only
  This program is free software: you can redistribute it and/or modify it under the terms of the
  GNU General Public License version 3. It is distributed WITHOUT ANY WARRANTY; see the LICENSE file.
  Not affiliated with Dexcom, Abbott, Medtrum or Nightscout.

  Arduino IDE (Araclar / Tools):
    Kart / Board ........... ESP32S3 Dev Module   (esp32 board package 3.3.x)
    USB CDC On Boot ........ Disabled
    Flash Size ............. 16MB (128Mb)
    Partition Scheme ....... Custom  (klasordeki partitions.csv: 2 x 4 MB uygulama + ~7.9 MB SPIFFS)
    PSRAM .................. OPI PSRAM
    Upload Speed ........... 921600
  Kutuphaneler / Libraries: LovyanGFX 1.2.x, WiFiManager (tzapu) 2.0.17, ArduinoJson 7.x
  Klasorde / In this folder: metematik_assets.h, metematik_lang.h, glukagon_video.h, glukagon_video_en.h,
                             qrcode.h, qrcode.c, partitions.csv, lang/ (ceviri kaynaklari / translation sources)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "qrcode.h"  // Ayni klasore qrcode.h + qrcode.cpp koy (artifacts/metematik/)
#include <Wire.h>
#include <ESP_I2S.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
// TFT_eSPI ile ayni isimler (GFXfont, FreeSansTR9pt, MC_DATUM ...) kullanilabilsin
using lgfx::GFXfont;
using lgfx::GFXglyph;
using namespace lgfx::v1::fonts;
#include <Preferences.h>
#include <mbedtls/sha1.h>
#include <nvs_flash.h>
#include <esp_sleep.h>
#include <time.h>
#include <sys/time.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <WebServer.h>
#include <SPIFFS.h>
#if __has_include("glukagon_video.h")
#include "glukagon_video.h"            // const uint8_t GLK_MMV[] + GLK_MMV_LEN (MMV1: JPEG kareler)
#else
static const uint8_t GLK_MMV_DATA[1] = {0};
#define GLK_MMV GLK_MMV_DATA
static const uint32_t GLK_MMV_LEN = 0;
#endif
#if __has_include("glukagon_video_en.h")
#include "glukagon_video_en.h"         // 3.6.0: Ingilizce yazi yamalari (MMVP) - Turkce disindaki diller
#else
static const uint8_t GLK_EN_DATA0[1] = {0};
#define GLK_EN GLK_EN_DATA0
static const uint32_t GLK_EN_LEN = 0;
#endif
#include <Update.h>
#include <ESPmDNS.h>
#include <math.h>

#include "metematik_lang.h"     // 3.6.0: ceviri tablosu (lang/gen_lang.py uretir)
#include "metematik_assets.h"   // yazi tipleri + logo (ayni klasorde olmali)


#define FIRMWARE_VERSION "4.0.5"

// ---- forward declarations (Arduino sirasi icin) ----
bool fetchData();
bool loadFotoFromFS();
void clearOldFotoFiles();
String fotoPath(int idx);
bool fotoFileValid(int idx);
void drawFotoGlucoseBadge();
void readBattery();
void drawCurrentScreen();
void drawSettingsScreen();
void drawTeacherFlow();
void drawFreeTitle(const char* text, int x, int y, const GFXfont* font = &FreeSansTR9pt);


// ---- CrowPanel Advance 2.8" (V1.2) pinleri ----
// Ekran ST7789 (SPI2): SCK 42, MOSI 39, DC 41, CS 40, arka isik 38
#define LCD_SCK      42
#define LCD_MOSI     39
#define LCD_DC       41
#define LCD_CS       40
// Kapasitif dokunmatik FT6336U (I2C 0x38) - J7 I2C konnektoruyle ayni hat
#define TP_SDA       15
#define TP_SCL       16
#define TP_INT       47
#define TP_RST       48
#define FT6336_ADDR  0x38
// Pil olcumu: INA219 modulu (I2C 0x40), pil kablosu modulun VIN+/VIN- ucundan
// gecer (pil+ -> VIN+, VIN- -> kartin pil girisi +). Modul yoksa pil gizlenir.
#define INA219_ADDR  0x40
#define BAT_CAPACITY_MAH 1750.0f   // Power-Xtra PX505060: etiket 2000; 3.4.27: 05-06.10 sarj olcumu ~1765 mAh (desarj 80 mA x 22,7 sa ile uyumlu)
#define BOOT_PIN     0
#define BACKLIGHT_PIN 38
// Ses: karttaki pasif buzzer (GPIO8, NPN surucu). Rezonansi ~4 kHz civarinda:
// alcak tonlar cok kisik cikiyor, bu yuzden tonlar spkTone() ile yukari
// tasiniyor. Hoparlor (I2S, NS4168 amfi: BCLK 13, LRCK 11, DATA 12,
// amfi 21) ileride eklenecek.
#define SPEAKER_PIN  8
#define AMP_CTRL_PIN 21   // hoparlor amfisi: HIGH = sessiz (kullanilmadigi icin kapali tutuluyor)
#define MAX_WIFI     10
#define HIST_MAX     288
#define BOOT_PIN_CODE "1234"

#define BRIGHT_LOW    40
#define BRIGHT_MED    120
#define BRIGHT_HIGH   255

class LGFX_CrowPanel28 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX_CrowPanel28() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 80000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = LCD_SCK;
      cfg.pin_mosi    = LCD_MOSI;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = LCD_CS;
      cfg.pin_rst          = -1;
      cfg.pin_busy         = -1;
      cfg.memory_width     = 240;
      cfg.memory_height    = 320;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 3;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};
LGFX_CrowPanel28 tft;

// ---- Tema: siyah zemin (2.2.0'da lacivertten geri donuldu) ----
uint16_t uiBg = 0x0000;          // ana ekran sprite'i cizilirken gecici olarak anahtar renk
#define UI_BG        uiBg     // siyah ana zemin
#define UI_CARD      0x18E4   // rgb(28,30,34)  liste satiri / kart
#define UI_CARD_ON   0x3186   // basili satir
#define UI_LINE      0x2104   // ayirici cizgi
#define UI_SUBTEXT   0xA514   // ikincil yazi (gri)
#define UI_ACCENT    0x2E5F   // camgobegi vurgu (kaydirma cubugu, geri oku)
#define UI_TOG_ON    0x1E8F   // anahtar acik (yesil)
#define UI_TOG_OFF   0x3A0C   // anahtar kapali

// =====================================================================
//  2.5.0 - TELEFON GORUNUMU: ana ekran (tema) + kaydirinca uygulamalar
// =====================================================================
// Sabit ekran numaralari (eski "sayfa sirasi" yerine)
#define SCR_GRAPH    2
#define SCR_SENSOR   3
#define SCR_STATS    4
#define SCR_TIMER    5
#define SCR_SETTINGS 6
#define SCR_APPS     7
#define SCR_KARB     8
#define SCR_VIDEO    9
#define SCR_THEME    10
#define SCR_COUNT    11

#define THEME_N 19           // 0 Ana 1 Klasik 2 Buyuk 3 Takvim 4 Foto 5 Halka 6 Kart 7 Dikey 8 Saat 9 Dalga 10 Minimal 11 Termometre 12 Panel 13 Nabiz 14 Yildizlar 15 Balon 16 Uzay 17 Akvaryum 18 Gulen yuz
#define BG_N    15           // 0 Siyah 1 Gece mavisi 2 Antrasit 3 Orman 4 Murdum 5 Karbon 6 Nokta 7 Degrade 8 Cizgi 13 Kareli | acik: 9 Krem 10 Buz 11 Pembe 12 Nane 14 Lavanta
#define HOME_KEY 0x0020      // ana ekran sprite'inda "arka plan" anahtar rengi
int homeTheme = 0;
int homeBg = 0;
bool themePreview = false;
int thSel = 0, bgSel = 0;
unsigned long homeHintUntil = 0;
bool homeHintShown = false;

// Dexcom / Libre icin elle girilen takma zamanlari (unix sn, 0 = girilmedi)
uint32_t sensInsTs = 0;
uint32_t setInsTs = 0;

// Form (telefon tarzi ayar sayfasi)
int formId = 0;
int formStack[4];
int formDepth = 0;
int formScroll = 0;
int formDlg = 0;             // 0 yok, 1 kaydedilmemis degisiklik, 2 onay (sifirla), 3 onay (kapat)
bool formDirty = false;

// Karb-Not
#define KARB_MAX 60
struct KarbItem { String name; uint8_t type; float carb; uint16_t uses; };
KarbItem karbItems[KARB_MAX];
int karbCount = 0;
int karbScroll = 0;
int karbView = 0;            // 0 liste, 1 hesap
int karbSel = -1;
char karbAmt[8] = {0};
int karbAmtLen = 0;
float karbMenuTotal = 0;
int karbMenuCount = 0;
struct MealRec { String name; float amt; uint8_t type; float carb; };
#define MEAL_MAX 15
MealRec mealRecs[MEAL_MAX];            // 3.4.2: menuye eklenenler
int mealN = 0;

// Video (glukagon)
#define VIDEO_PATH "/glk.mmv"
#define VIDEO_MAX_BYTES (4400UL * 1024UL)
bool videoPlaying = false;
bool videoPaused = false;
bool videoShadow = false;              // 3.4.6: video kareleri PSRAM tamponunda da tutulur (duraklat izi kalmasin)
bool previewVideo = false;             // 3.4.6: Onizlemeler'den acilan video
int teacherPreview = 0;                // 3.4.6: 1 dusuk, 2 yuksek ornegiyle Dikkat onizlemesi
bool previewFromInfo = false;
int previewInfoScroll = 0;
int previewRetScreen = 6;
bool videoEnded = false;
int videoReturnScreen = -1;  // -1: uygulamalar
bool videoFromTeacher = false;

// Form veri yapilari (Arduino otomatik prototipleri icin en ustte)
enum FormIds { F_ALARMS = 30, F_TARGETS, F_DISPLAY, F_LANG, F_NIGHT, F_SCHOOL, F_SLEEP, F_RESET, F_MODES, F_SENSOR, F_PAGES, F_PREVIEW };
enum FRowType { FR_HEAD = 1, FR_SEG, FR_TOG, FR_NUMSND, FR_TIME, FR_ACT, FR_NOTE, FR_COLORS, FR_DT, FR_LIFE };

#define FROW_MAX 26
struct FRow {
  uint8_t type; uint8_t id; String label; String value;
  int* ip; int* ip2; bool* bp; uint32_t* tp;
  int8_t nOpt; const char* opt[5]; int ov[5];
  uint8_t help; int16_t y, h, infoX; uint16_t col;
};
struct FSnap { int* ip; bool* bp; uint32_t* tp; int iv; bool bv; uint32_t tv; };

// 2.6.0
bool frameActive = false;     // ekran PSRAM tamponunda (arka plan desenli) ciziliyor
int themeScroll = 0;
bool lowBatWarned = false;    // %20 pil uyarisi verildi
bool lowBatDim = false;       // dusuk pil: ekran kisik
LGFX_Sprite homeSpr(&tft);    // tam ekran tamponu (arka plan desenli cizim)
bool lightBg = false;         // cizilen ana ekranin arka plani acik renk mi (2.7.0)
int themeTab = 0;             // tema sayfasi sekmesi: 0 Temalar 1 Animasyon 2 Cocuk 3 Arka plan
// 2.9.0
volatile uint8_t wifiDiscReason = 0;  // son WiFi kopma nedeni (15/202/204: sifre, 201: ag yok)
bool wifiSetupFailed = false;         // kurulumda girilen aga baglanilamadi
int dexLastCode = 0;                  // son Dexcom giris HTTP kodu (<=0: ag hatasi)
int libreLastCode = 0, libreLastStatus = -1;
// 2.9.3: "Kurulum" temiz yeniden baslatma ile acilir (yazilimsal resette korunur)
#define PORTAL_MAGIC 0xB007CAFEUL
RTC_NOINIT_ATTR uint32_t portalOnBoot;
bool wokeFromLowBat = false;                 // 3.4.27: "pil bitti" uykusundan sarjla uyandi -> yuzde ~0'dan baslar
RTC_NOINIT_ATTR uint32_t lowBatSleepFlag;     // 3.4.16: pil bitti uykusu (5 dk'da bir sarj kontrolu)
#define LOWBAT_MAGIC 0x4C4F5742UL
bool forcePortalBoot = false;
// 3.0.0: ana ekran sayfalari (kaydirarak gezilen)
enum PageCodes { P_HOME = 0, P_TH2, P_TH3, P_SENSOR, P_GRAPH, P_STATS, P_TIMER, P_KARB, P_APPS, P_N };
bool pgOn[P_N] = {true, false, false, false, false, false, false, false, true};
int pgTh2Theme = 5, pgTh3Theme = 8;   // 2. ve 3. sayfa temalari
int curPage = P_HOME;                 // -1: uygulama simgesinden acildi (sayfa dizisinde degil)
int pageThemeOverride = -1;           // 2./3. tema sayfasinda gosterilen tema
int thPickSlot = 0;                   // tema sayfasi "sayfa temasi sec" modunda (2 / 3)
// 3.0.0: Medtrum (EasyFollow hesabi)
String medEmail = "", medPass = "", medCookie = "";
int medLastCode = 0;
int homeBgDark = 0;                   // 3.1.0: acik arka plan secilince, uygulanamayan yerlerde kullanilan son koyu arka plan
int thDarkSel = 0;                    // tema sayfasinda secili koyu (kaydedilince homeBgDark)
int thPickFrom = -1;                  // sayfa temasi secimine hangi ekrandan gelindi
// 3.2.0: erteleme sadece ertelenen uyari turu icin; kritik dusuk en fazla 10 dk
bool snoozeActive = false;
int snoozedKind = 0;
// 3.3.0
bool homeStaleGrey = false;           // ana ekran cizilirken veri eskiyse seker renkleri gri
bool quickBoot = false;               // donma sonrasi otomatik yeniden baslatma: sessiz, hizli acilis


// Ikon fonksiyonlari hem ekrana hem liste sprite'ina cizebilsin diye
// ortak bir hedef isaretcisi kullaniyor (varsayilan: ekran).
lgfx::LovyanGFX* cv = &tft;
// Kaydirmali listeler titremesin diye PSRAM'de tampon (320x200)
LGFX_Sprite listSpr(&tft);
bool listSprOk = false;
bool textPageActive = false;    // kaydirmali metin sayfasi (Bilgi, Kilavuz, (i) yardimlari)

// ---- Rahatsız etme modu (2.3.0) ----
// Seker hedef araliktaysa ekran kapanir; dokunma veya iki parmak siklatmasi
// (mikrofon) ile ana ekran 15 sn gorunur. Seker aralik disina cikarsa,
// hizli dusus/yukselis varsa veya veri gelmezse ekran kendiliginden acilir.
#define MIC_CLK_PIN     9
#define MIC_DATA_PIN    10
#define AUDIO_SEL_PIN   45     // HIGH = mikrofon yolu (SGM3799 anahtari)
#define SLEEP_SHOW_MS   15000UL
bool sleepModeOn = false;
bool micSnapOn = true;         // siklatma ile uyandirma (mikrofon)
int  snapSens = 1;             // 0 dusuk, 1 orta, 2 yuksek
bool screenAsleep = false;
unsigned long sleepWakeUntil = 0;
volatile bool micWanted = false;
volatile bool snapFlag = false;       // cift siklatma algilandi
volatile int  micPeak = 0;            // son 10 ms tepe (test ekrani)
volatile int  micBg = 0;              // arka plan seviyesi
volatile int  micThr = 0;             // o anki esik
volatile int  snapSingles = 0;        // tekli siklatma sayaci (test)
volatile bool micRunning = false;
volatile unsigned long lastToneMs = 0; // buzzer calarken mikrofon algilamasi yok sayilir
unsigned long snapShownUntil = 0;
bool timePickerActive = false;  // saat giris tus takimi
void drawVisualAlarmPage();

// Dokunmatik ham koordinatlari panelin dogal (240x320 dikey) cercevesinde gelir.
// tftEffRot: panelin gercek (etkin) donusu; eski CYD kodundaki setRotation(r)
// cagrisi LovyanGFX'te (r+3)%4 olarak yapiliyor, etkin donus = (r+2)%4.
int tftEffRot = 3;
Preferences prefs;
WiFiMulti wifiMulti;
WiFiClientSecure secureClient;
WebServer server(80);

String nightscoutUrl = "https://";
String apiSecret = "";
// Web + OTA sifresi SABIT — web formundan degistirilemez (sadece kaynak kod)
static const char* WEB_USER = "metematik";
String webUser = WEB_USER;
// 2.1.0: Web/OTA sifresi artik koda yazili degil. Her cihaz ilk acilista
// kendine ozel rastgele bir sifre uretip NVS'e kaydeder (cihaz etiketine
// yazilir; Bilgi ekraninda gorulebilir). Cihaz
// Sifirla bu sifreye dokunmaz.
String webPass = "";
// Tarayiciyi 30 gun hatirlayan oturum cerezi (NVS, cihaza ozel)
String webToken = "";
// 3.5.0: cihaz kilidi (Chip ID listesi + /admin) acik kaynak surumde kaldirildi.
String thisChipId = "";
String dataSource = "nightscout";
// Son basarili Nightscout cekisi 288 istegi basarisiz olup 200'e (yaklasik
// 16-17 saat) mi dustu - Istatistik/Grafik ekranlarinda saat etiketini buna
// gore ayarlamak icin (bkz. estimatedHoursLabel(), graphHourLabelFor()).
bool nsUsedFallback200 = false;
String dexUser = "", dexPass = "", dexServer = "ous";
String libreEmail = "", librePass = "", libreRegion = "EU";

String wifiSsid[MAX_WIFI];
String wifiPass[MAX_WIFI];
int wifiCount = 0;

int currentScreen = 0;
unsigned long lastFetch = 0;
const unsigned long FETCH_INTERVAL = 25000;
unsigned long lastNsExtraFetch = 0;
const unsigned long NS_EXTRA_INTERVAL = 180000UL; // IOB/COB/bolus 3 dk
unsigned long lastUserTouchMs = 0;

int sgv = 0, delta = 0;
bool deltaInterp = false, deltaUnknown = false;   // 3.6.1: delta 5 dk'ya cevrildi (*) / bilinmiyor (--)
String direction = "Flat";
unsigned long readingAgeSec = 0;
uint64_t lastEntryDateMs = 0;
unsigned long entrySeenAtMillis = 0;
String lastDateString = "";
bool dataValid = false;
volatile uint32_t credGen = 0;          // 3.4.0: web'den ayar kaydedilince artar
bool fetchDiscarded = false;           // 3.4.1: son cekme ayar degisikligi yuzunden yok sayildi
bool srcChangedPending = false;        // 3.4.1: kaynak ayari degisti, ilk sonuc hataysa hemen hata ekrani
bool holdSwallow = false;              // 3.4.2: basili tutma sonrasi parmak kalkana kadar dokunma yok
String secKey = "", caKey = "";        // 3.4.2: TLS baglantisi hangi sunucuya acik (degisirse kapatilir)
bool libreRingReset = false;           // 3.4.3: kaynak degisince Libre degisim halkasi sifirlansin
bool touchReset = false;               // 3.4.3: uyari/kilit ekranindan cikinca yarim kalan dokunma iptal
bool themeHoldShown = false;           // 3.4.3: tema basili-tut hapi ekranda (animasyon beklesin)
bool retryPillShown = false;           // 3.4.3: "Yeniden deneniyor" hapi ekranda
bool medFillOk = true;                 // 3.4.3: Medtrum gecmis indirme basarili mi
bool medHistOk = false;                // 3.4.3: Medtrum 24 saatlik gecmis tamam mi
bool medResetPending = false;          // 3.4.3: ilk basarili parcada eski gecmis silinsin
int micTestFrom = -1;                  // 3.4.3: siklatma testine hangi ekrandan gelindi
int consecutiveFetchFails = 0; // ard arda basarisiz fetchData() sayisi — kisa/gecici
                                // aksakliklarda hemen tam ekran hata gostermemek icin
int lastErrorCode = 0;
volatile bool netNoInternet = false;   // 3.4.8: WiFi bagli ama internete cikilamiyor
unsigned long netFullTry = 0;          // internet yokken kaynagin en son tam denendigi an
String lastErrorDetail = "";
bool showErrorScreen = false;
bool errorDismissed = false; // Geri ile kapatildiysa dokununca tekrar zorla acma
unsigned long lastErrorShownAt = 0;
bool alarmBlinkState = false;
unsigned long lastBlink = 0, lastBarRedraw = 0;
float sensorAgeHours = -1, cannulaAgeHours = -1;
// Beklenen kullanim omru (SAAT cinsinden, gun+saat hassasiyetiyle web
// arayuzunden giriliyor) - Nightscout "kalan sure" alanini DOGRUDAN vermiyor,
// sadece degisimden bu yana gecen saati veriyor. "Kalan sure"yi gosterebilmek
// icin bu ayarlardan hesapliyoruz. Varsayilanlar ornek:
// sensor 10 gun 12 saat (=252 saat), set/pompa 3 gun 8 saat (=80 saat).
int sensorLifeHours = 10 * 24 + 12;
int cannulaLifeHours = 3 * 24 + 8;
// Pompa kullanmayanlar icin Set/Pompa takibi ayri kapatilabiliyor; Sensor
// icin de ayni sekilde ayri bir ac/kapa var.
bool sensorTrackOn = true;
bool cannulaTrackOn = true;

// Sure dolmaya 12sa / 3sa / 1sa / 15dk kala birer defaya mahsus tam ekran
// uyari sayfasi (kucukten buyuge, index sirasiyla asagidaki dizilerle
// birebir eslesir). Sensor ve set/pompa icin ayri ayri takip ediliyor.
const float AGE_WARN_THRESH_H[4] = {0.25f, 1.0f, 3.0f, 12.0f};
const unsigned long AGE_WARN_SHOW_MS = 7000UL; // birkac saniye sonra otomatik kapanir
bool sensorWarnFired[4] = {false, false, false, false};
bool cannulaWarnFired[4] = {false, false, false, false};
float lastSensorAgeForWarn = -1;
float lastCannulaAgeForWarn = -1;
bool ageWarnActive = false;
int ageWarnKind = 0;       // 0=sensor 1=set/pompa
int ageWarnThreshIdx = 0;  // AGE_WARN_THRESH_H indeksi
unsigned long ageWarnShowUntil = 0;
int ageWarnQueueKind[4];
int ageWarnQueueIdx[4];
int ageWarnQueueLen = 0;

#define PERSON_MAX 3                // 4.0: aile modu - en fazla 3 kisi
int histStore[PERSON_MAX][HIST_MAX];
uint32_t histDayStore[PERSON_MAX][HIST_MAX];
uint32_t histTsStore[PERSON_MAX][HIST_MAX];
int* history = histStore[0];                 // 4.0: o an etkin kisinin dizileri (kisi degisince isaretci degisir)
uint32_t* historyDay = histDayStore[0];
uint32_t* historyTsSec = histTsStore[0];  // unix sec for time filter
int historyCount = 0;

// --- Libre icin cihaz-tarafli 24 saatlik gunluk (local ring) ---
// LibreView'in /graph API'si guvenilir sekilde 24 saat/288 olcum
// vermiyor (bkz. GRAPH_HOURS'daki 12 saatlik Libre siniri). Bu yuzden
// cihaz kendi gordugu her YENI Libre okumasini burada biriktirip
// fetchLibre() icinde API sonucuyla birlestiriyor - zamanla (cihaz ne
// kadar uzun suredir acik/baglıysa o kadar) 24 saate/288'e yaklasilir.
#define LOCAL_HIST_MAX 288
int lhStore[PERSON_MAX][LOCAL_HIST_MAX];
uint32_t lhTsStore[PERSON_MAX][LOCAL_HIST_MAX];
int* localHist = lhStore[0];
uint32_t* localHistTs = lhTsStore[0];
int localHistCount = 0;

String dexAccountId = "", dexSessionId = "";
String libreToken = "", libreAccountId = "", librePatientId = "";

bool otaServerStarted = false;
int brightnessLevel = 2;

int alarmLow = 70;
int alarmHigh = 180;
int alarmVeryHigh = 250;
int alarmCritLow = 55;
// Esik basina ses ac/kapa - varsayilan hepsi acik (eski davranis degismiyor).
// Kapatilan bir esik icin GORSEL alarm (visualAlarmOn acikken) yine calisir,
// sadece ses calmaz - nightscout-clock projesindeki gibi ama basitlestirilmis:
// tek bir aç/kapa var, ayri melodi/erteleme/sessiz-saat yok (karmasiklastirmamak icin).
bool alarmLowSound = true;
bool alarmHighSound = true;
bool alarmVeryHighSound = true;
bool alarmCritLowSound = true;
bool alarmStaleSound = true; // "Veri yok" alarmi icin ayni ac/kapa
// Cift ok (hizli degisim) alarmlari: trend "DoubleDown"/"DoubleUp" iken,
// seker henuz esik disina cikmadan once uyarir. Libre cift ok vermedigi
// icin sadece Nightscout ve Dexcom'da tetiklenir.
bool alarmFastFallOn = true;   // hizli dusus - varsayilan ACIK (guvenlik)
bool alarmFastRiseOn = false;  // hizli yukselis - varsayilan KAPALI (yemek sonrasi sik tetiklenir)
int staleAlarmMin = 15;
int alarmColorIdx = 0;
bool visualAlarmOn = true;
int snoozeMinutes = 30;  // 0 = ertele butonu yok
unsigned long alarmSnoozeUntil = 0;
bool requestAlarmBurst = false;
bool libBurstReset = false;      // 3.4.20: kaynak degisince Libre alarm tekrar sayaci sifirlansin
// Sekerin esik disi olsa da yon+miktarca normale dogru belirgin gittigi
// durumlarda SESLI alarmi otomatik susturma (gorsel alarm etkilenmez).
// Kritik Dusuk'te hicbir zaman gecerli degil - bkz. isRecovering().
bool recoverySoundSuppressOn = true;
#define RECOVERY_DELTA_MGDL 5
unsigned long visualBurstEnd = 0;  // gorsel alarm kisa burst (ses gibi)
unsigned long alarmBurstEnd = 0;
int alarmBurstKind = 0; // 0 yok 1 kritik 2 dusuk 3 yuksek 4 cok yuksek 5 stale 6 hizli dusus 7 hizli yukselis
int fastBeepSeq = 0;    // cift ok alarmlarinda 3'lu bip sirasini tutar
bool nightModeOn = false;
int nightStartHour = 22;
int nightEndHour = 7;
int nightStartMin = 0;   // dakika (0-59) - 2.2.0'dan once sadece :00/:30
int nightEndMin = 0;
int nightBrightLevel = 0;
int nightSoundLevel = 1;
// Okul modu (gece mantigi — gun icinde)
bool schoolModeOn = false;
int schoolStartHour = 8;
int schoolEndHour = 15;
int schoolStartMin = 0;  // dakika (0-59)
int schoolEndMin = 0;
int schoolBrightLevel = 1;
int schoolSoundLevel = 1;
bool teacherModeOn = false;
int teacherStep = 0; // 0=kapali/ozet 1=bilinc? 2=olcum? 3=aksiyon karti
bool teacherConscious = true;
bool teacherFingerOk = true;
int teacherCat = 0;        // 3.4.5: 1 dusuk, 2 normal, 3 cok yuksek (karar verilen durum)
int teacherBack = 0;       // 3.4.5: aksiyon kartindan geri donulecek adim
int teacherGlucagonPage = 0; // 0=kapali 1..4 glukagon rehberi (4. sayfa: video QR)
bool showGuide = false;
int guidePage = 0;
const int GUIDE_PAGES = 6;
// Bilgi ekrani (subScreen 4) su an tek yerden aciliyor (QR sayfasi), ama
// ileride baska bir giris noktasi eklenirse "Geri" basildiginda hangi
// ekrandan gelindiyse oraya donmesi icin giris noktasi burada saklaniyor.
int infoReturnScreen = 14;

int subScreen = 0; // 0=ayar 1=alarm 2=ses ... 16=ogretmen 18=hedefler
int alarmEditStep = 0;
int soundLevel = 2;     // 0=sessiz 1=dusuk 2=orta 3=yuksek
String bootPinBuf = ""; // artik kullanilmiyor (Reset ekrani sifre yerine Evet/Hayir onayi kullaniyor)
int resetAction = 0; // Reset ekrani ic durumu: 0=secim, 1=wifi sifirla onayi, 2=kapat onayi
unsigned long lastBeepPhase = 0;
int beepPhase = 0;

// Faz 0: dil / birim iskeleti
int langIdx = 0;        // 0=TR 1=EN 2=ES 3=PT 4=FR 5=DE 6=IT 7=RU 8=ID (3.6.0)
#define LANG_COUNT 9
static const char* const LANG_NAMES[LANG_COUNT] = {"Türkçe", "English", "Español", "Português", "Français", "Deutsch", "Italiano", "Русский", "Indonesia"};
static const char* const LANG_CODES[LANG_COUNT] = {"TR", "EN", "ES", "PT", "FR", "DE", "IT", "RU", "ID"};
static const char* const LANG_HTML[LANG_COUNT]  = {"tr", "en", "es", "pt", "fr", "de", "it", "ru", "id"};
int unitIdx = 0;        // 0=mg/dL 1=mmol/L
bool colorBlindMode = false; // renk korlugu dostu palet (kirmizi/yesil yerine mavi/turuncu tonlari)
// Arayuz onizleme (ilk sayfaya bagli degil)
// 0=yok 1=BIG 2=Takvim 3=AnimA 4=AnimB 5=Dikey
int uiPreviewMode = 0;   // onizleme
bool uiPreviewActive = false;
int uiPageMode = 0;      // 0 Sade 1 Buyuk 2 Takvim 3 Dikey 4 Foto
// free~113KB (kullanici raporu). 256x192=98304 guvenli pay.
#define FOTO_W 256
#define FOTO_H 192
#define FOTO_BYTES (FOTO_W * FOTO_H * 2)
// Ilk test karti: 4MB flash, "App with OTA" bolumlemesiyle SPIFFS ~1.9MB —
// ~19 foto kadar yer var. 10 ile sinirlandirildi (rahat pay birakildi).
#define FOTO_MAX 20
bool fotoLoaded = false;
int fotoCount = 0;        // su an SPIFFS'te gecerli, sirali (0..fotoCount-1) kac foto var
int fotoActiveIdx = 0;    // Foto Cerceve'de su an gosterilen slot
int fotoRotateMin = 5;    // kac dakikada bir sonraki fotoya gecilsin (yalniz fotoCount>1 iken)
unsigned long fotoLastRotate = 0;

int batteryPercent = -1;   // -1 = olculemiyor (INA219 yok / pil takili degil)
int batteryMv = 0;
int batteryMa = 0;         // + = pilden cekiliyor, - = pil sarj oluyor
bool batteryCharging = false;
bool inaPresent = false;
unsigned long lastBatRead = 0;
int lastDrawnBatPct = -2;
bool lastDrawnBatChg = false;

// ---- Tum fonksiyonlarin on bildirimleri (Arduino IDE surumunden bagimsiz
//      derlensin diye elle eklendi; ham HTML metinleri bazi ctags
//      surumlerinde otomatik bildirim uretimini bozabiliyor) ----
bool inaRead16(uint8_t reg, int16_t& v);
void inaInit();
int liionPercent(int mv);
void readBattery();
void drawBatteryIndicator();
void clearOldFotoFiles();
int graphHourLabelFor(int idx);
const char* uiModeName(int m);
void drawUiDropdownChrome();
String sourceLabel();
int screenCount();
uint16_t getAlarmColor();
bool isAlarmCondition();
bool isAlarmActive();
bool shouldShowDikkat();
bool shouldShowSnoozeButton();
int getAlarmKind();
bool isRecovering(int kind);
bool alarmSoundEnabledForKind(int kind);
void stepHalfHour(int &hour, int &mins, int dir);
bool isNightHours();
bool isSchoolHours();
void applyBrightness();
int effectiveSoundLevel();
void cycleBrightness();
void applyRotation();
bool ftReadRegs(uint8_t reg, uint8_t* buf, uint8_t n);
bool touchReadRaw(int& rx, int& ry);
void touchInit();
void mapTouchToScreen(int rx, int ry, int& x, int& y);
String brightnessLabel();
String formatGlucose(int mgdl);
String formatDelta(int dMg);
const char* L(const char* tr, const char* en);
const char* WT(const char* tr);
const char* monthName(int m);
void openLangPicker();
void dsN(lgfx::LovyanGFX* g, const char* t, int x, int y, int f);
void dsN(lgfx::LovyanGFX* g, const String& t, int x, int y, int f);
void drawLangPicker();
void langPickerTap(int x, int y);
const char* monthShortName(int m);
String batteryPctText(const char* fmt);
String fmtS(const char* fmt, const String& a);
String soundLabel();
int buzzFreq(int f);
void stopSpeaker();
void serviceAlarmSound();
void playTestBeep();
void playAgeWarnBeep();
uint32_t localDayStamp();
String todayDateStr();
uint32_t dayStampFromMs(uint64_t ms);
void appendLocalHistory(int sgvVal, uint32_t tsSec);
void trackLocalReading(int sgvVal, uint32_t tsSec, uint32_t &lastAppendTs);
void mergeLocalHistory();
int estimatedHours(int used);
String estimatedHoursLabel(int used);
String approxHoursShort(int used);
String ageText();
void detectDataSource();
uint16_t sevColor(int tier);
uint16_t getGlucoseColor(int v);
String sha256Hex(const String& s);
void drawTrendArrow(int cx, int cy, String dir, uint16_t col, int size);
void drawMinuteBars(int y, unsigned long ageSec, bool withAgeText);
void drawAgeFooter(int y);
void updateAge();
bool markNewReading(uint64_t dateMs, const String& dateStr);
void loadSettings();
void saveSettings();
void addWifiCredential(const String& ssid, const String& pass);
void buildWifiMulti();
bool checkWebAuth();
String getChipIdStr();
String deviceName();
String pwMask(const String& p);
void applyPw(const String& v, String& target);
void drawListHeader(const char* title, bool nextArrow);
void parseHM(const String& s, int &hour, int &mins, int defHour);
String htmlEscape(const String& s);
void handleRoot();
void handleSave();
void handleFotoUpload();
void handleFotoDone();
void handleFotoPage();
void handleFotoRaw();
void startOtaServer();
void stopOtaServer();
float parseAgeField(JsonVariant v);
void fetchSageCage();
void pushAgeWarn(int kind, int threshIdx);
void checkAgeOne(int kind, bool trackOn, float ageHours, int lifeHours, bool* warnFired, float &lastAgeForWarn);
void checkAgeWarnings();
void pumpAgeWarnQueue();
static uint32_t parseIsoToUnix(const char* iso);
void fetchNsIobCobBolus(bool force);
void setFetchError(int code, const String& detail);
void clearFetchError();
const char* errorTitle();
const char* errorHint();
void drawErrorScreenFull();
void drawWifiBanner();
bool fetchNightscout();
String dexBase();
bool dexGetAccountId();
bool dexGetSessionId();
bool fetchDexcom();
String libreBase();
void libreAddHeaders(HTTPClient& http, bool auth);
bool libreLogin();
bool fetchLibre();
bool fetchData();
void drawArrowSimple(int cx, int cy, const String& dir, uint16_t col);
void drawUiBigDigits();
void drawUiCalendar();
void drawUiPortrait();
bool fotoFileValid(int idx);
bool loadFotoFromFS();
void drawFotoGlucoseBadge();
void restoreFotoTopBand();
void drawUiFotoPlaceholder();
void drawScreen0();
void drawUiSade();
void drawScreen1();
void drawNsDetailScreen();
void drawScreen2();
static void formatAgeShort(float hours, char* out, size_t outSize);
void drawAgeCard(int cardY, const char* label, float ageHours, int lifeHours);
void drawScreen3_Sensor();
void drawAgeWarnScreen();
void drawStatsScreen();
void drawGuideFooterDots();
void drawGuidePage();
void drawAlarmSetupScreen();
void drawSoundScreen();
void drawBrightnessScreen();
void drawQrScreen();
void drawResetScreen();
void drawInfoScreen();
void drawSourcesScreen();
void drawLangUnitScreen();
void drawUiModeScreen();
void drawRotationScreen();
void drawGeceScreen();
void drawOkulScreen();
void drawTeacherFlow();
void drawTeacherScreen();
void drawDigerScreen();
void drawBrightnessIcon(int cx, int cy, uint16_t c);
void drawBrightnessLevelIcon(int cx, int cy, uint16_t c, int level);
void drawSoundIcon(int cx, int cy, uint16_t c);
void drawBellIcon(int cx, int cy, uint16_t c);
void drawTargetIcon(int cx, int cy, uint16_t c);
void drawQrIcon(int cx, int cy, uint16_t c);
void drawMoreIcon(int cx, int cy, uint16_t c);
void drawGlobeIcon(int cx, int cy, uint16_t c);
void drawInfoIconM(int cx, int cy, uint16_t c);
void drawLockIcon(int cx, int cy, uint16_t c);
void drawRotateIcon(int cx, int cy, uint16_t c);
void drawRefreshIcon(int cx, int cy, uint16_t c);
void drawBookIcon(int cx, int cy, uint16_t c);
void drawPowerIcon(int cx, int cy, uint16_t c);
void drawBackIcon(int cx, int cy, uint16_t c);
void drawFlashIcon(int cx, int cy, uint16_t c);
void drawChevronRight(int cx, int cy, uint16_t c);
void drawClockIcon(int cx, int cy, uint16_t c);
void drawGridIcon(int cx, int cy, uint16_t c);
void drawCheckIcon(int cx, int cy, uint16_t c);
void drawXIcon(int cx, int cy, uint16_t c);
void drawWarnIcon(int cx, int cy, uint16_t c, uint16_t bg);
void drawChevronUp(int cx, int cy, uint16_t c);
void drawChevronDown(int cx, int cy, uint16_t c);
void drawDoubleChevronUp(int cx, int cy, uint16_t c);
void drawDoubleChevronDown(int cx, int cy, uint16_t c);
void drawNoDataIcon(int cx, int cy, uint16_t c);
void drawMuteIcon(int cx, int cy, uint16_t c);
void drawHourglassIcon(int cx, int cy, uint16_t c);
void drawSparkleIcon(int cx, int cy, uint16_t c);
void drawForkIcon(int cx, int cy, uint16_t c);
void drawSoundLevelIcon(int cx, int cy, uint16_t c, int waves);
void drawMoonIcon(int cx, int cy, uint16_t c, uint16_t bg);
void drawFreeTitle(const char* text, int x, int y, const GFXfont* font);
void drawSettingsScreen();
void updateTimerCountdownOnly();
void drawTimerScreen();
void playMelody(const int seq[][2], int count);
void playBootChime();
void playMealChime();
void playTimerDoneMelody();
void serviceTimer();
void drawCurrentScreen();
void showPortalScreen();
void startConfigPortal();
bool connectWifi(bool silent);
void showSplash();
void showBootWarning();
// ---- 2.5.0 ----
int themeUiMode(int th);
int activeTheme();
int homeScreenIdx();
bool isHomeScreen();
const char* themeName(int t);
const char* bgName(int b);
uint16_t bgPixelH(int b, int x, int y, int h);
uint16_t bgPixel(int b, int x, int y);
bool frameSpriteFor(int w, int h);
bool bgIsLight(int b);
bool themeLightOK(int th);
bool themeOwnScene(int th);
int themeCategory(int th);
bool themeHidden(int th);
bool themeAnimatedId(int th);
bool themeAnimated();
unsigned long themeAnimPeriod();
int currentBg();
void composeBuf(uint16_t* p, int w, int rows, int yoff, int fullH, int b);
bool bgFrameOn();
void drawWifiGlyph(lgfx::LovyanGFX* g, int x, int y);
void drawBatteryGlyph(lgfx::LovyanGFX* g, int x, int y);
String clockStr();
void drawMuteGlyph(lgfx::LovyanGFX* g, int x, int y, uint16_t c);
uint16_t muteGlyphColor();
void drawStatusBar(bool showGlucose);
String ageTextTR();
uint16_t dimColor(uint16_t c, int pct);
uint16_t blendC(uint16_t a, uint16_t b, int pctB);
uint16_t TXT();
uint16_t SUBT();
uint16_t CARDC();
uint16_t FACEC();
uint16_t GC(int v);
uint16_t TRACKC(uint16_t c);
void drawUiRing();
void drawMiniHistory(int sx, int gy, int gw, int gh, int hours);
void drawUiCardTheme();
bool themeFlash();
void drawUiVertical();
void drawUiClock();
void drawUiWave();
void drawValueBlock(int cx, int cy, float scale, uint16_t tc, bool withUnit);
void drawDeltaRow(int cx, int cy, uint16_t tc, int arrow);
int kidY(int v);
String kidMsg(const char* subj);
void kidCard(uint16_t cardC, uint16_t txtC, bool darkText);
void kidMessage(const char* subj, uint16_t col, int y);
void drawUiMinimal();
void drawUiThermo();
void drawUiPanel();
void drawUiPulse();
void drawUiStars();
void drawUiBalloon();
void drawUiSpace();
void drawUiAquarium();
void drawUiSmiley();
void drawSpeakerIcon(int cx, int cy, uint16_t c, bool on);
int homeBtnW();
bool homeDikkatHit(int x, int y);
bool homeSnoozeHit(int x, int y);
void drawCompactAge(int cx, int y);
void drawHomeOverlays(int th);
bool homeDataStale();
void drawStaleLabel();
void drawHomeContent(int th);
void drawHome();
const char* appName(int i);
void drawAppGlyph(int id, int cx, int cy, uint16_t bg);
void drawAppsContent();
void drawAppsPage();
void openApp(int i);
void appsTap(int x, int y);
void goHome();
void goApps();
void drawFormHeaderBtn(const char* title, const char* btn, bool hot);
void drawFormHeader(const char* title, bool save);
void drawBackChevron(int cx, int cy);
int themeContentH();
int swatchCx(int k);
int swatchCyDark(int k);
int swatchCyLight();
void drawThemeTile(lgfx::LovyanGFX* g, int t, int x, int y, bool sel);
void drawBgSwatch(lgfx::LovyanGFX* g, int b, int cx, int cy, bool sel, bool ok);
void drawThemeHeader();
int thSelNow();
int thSavedNow();
void drawThemeBody();
void drawThemePage();
void themePickDone(bool apply);
void openThemePick(int slot);
void themeSave();
void themeTap(int x, int y);
void fHead(const String& t);
void fNumSnd(uint8_t id, const String& t, int* ip, bool* snd, const String& v);
void fTime(uint8_t id, const String& t, int* h, int* m);
String fmtLocalDT(uint32_t ts);
void fDT(uint8_t id, const String& t, uint32_t* ts);
String lifeLabel(int h);
void fLife(uint8_t id, const String& t, int* hours);
String levelName3(int i);
void formBuild(int id);
void fSnapAddI(int* p);
void fSnapAddB(bool* p);
void fSnapAddT(uint32_t* p);
void formSnapshot();
bool formIsDirty();
void formLive();
void formRevert();
void drawToggleG(lgfx::LovyanGFX* g, int tx, int ty, bool on);
void drawFormBody();
void drawFormDialog();
void drawForm();
void formOpen(int id);
void formClose();
bool formValidate();
void formSave();
void formBack();
void formAction(int id);
void doPowerOff();
void doDeviceReset();
void formDialogTap(int x, int y);
void formTap(int x, int y);
uint32_t civilToUnixLocal(int y, int m, int d, int hh, int mi);
void openDateTimePicker(const char* title, uint32_t* ts);
void dtpDisplay(char* out);
bool dtpTap(int k);
void computeManualAges();
void openLifePicker(const char* title, int* hours, int maxDays);
void lifeDisplay(char* out);
void lifeTap(int k);
const char* karbUnitName(int t, bool per);
String karbNum(float v);
void karbLoad();
bool karbSaveFile();
void buildKarbItems();
float karbAmountValue();
float karbResult();
void drawKarbCalc();
void karbCalcTap(int x, int y);
void drawKarbScreen();
void karbListTap(int id);
bool videoExists();
void videoStop();
bool videoStart();
void drawVideoChrome();
void drawVideoScreen();
void openVideo(int returnScreen, bool fromTeacher);
void closeVideo();
void videoTap(int x, int y);
void serviceVideo();
void formAbandon();
void doSnooze();
void startTeacherFlow();
void openSleepFormFromTest();
void homeEmptyTap();
int pagerList(int* out);
int pageScreen(int p);
bool pagerActive();
void showPage(int p);
void pagerStep(int step);
void goBack();
void handleSwipe(int dir);
bool autoRefreshOk();
bool listScreenActive();
int listKindNow();
void listTap(int kind, int x, int y);
bool handleListTouch(bool touchNow, bool touchWasPrev, int rawX, int rawY, unsigned long lastNav);
bool screenDirect();
void drawScreenBody();
void drawCurrentScreen();
void restartIntoPortal();
void doWifiSetup();
String savedWifiName();
bool wifiReasonIsAuth();
bool wifiReasonNoAp();
void libreLoginError();
unsigned long fetchIntervalNow();
void drawRetryPill();
void wifiBgReconnect();
String errorHintLong();
bool homeWifiPillOn();
bool homeWifiHit(int x, int y);
void openWifiErrorScreen();
void drawBootHoldPill(unsigned long held);
void checkLowBattery();
void redrawNow();
void forceLandscape();
String medUrlEnc(const String& s);
void medHeaders(HTTPClient& h);
int medLogin();
void medPush(int mg, uint32_t ts);
int medToMg(double g);
void medFillHistory(const String& user, uint32_t from, uint32_t to);
bool fetchMedtrumTry(bool again);
bool fetchMedtrum();
void wdtStart();
void wdtFeed();
bool crashReset();
void showQuickBootScreen();
void handleKarbPage();
void handleKarbSave();
void handleVideoPage();
void handleVideoUpload();
void handleVideoDone();
void handleVideoDel();
uint32_t parseDateTimeLocal(const String& s);
String dateTimeLocalValue(uint32_t ts);
void bootAnimTask(void*);
void bootStage(const char* msg, int cap);
void bootStop();
void bootFinish();
bool inFetchTask();
void netRelease();
void netAcquire();
int netGET(HTTPClient& h);
int netPOST(HTTPClient& h, const String& body);
String netBody(HTTPClient& h);
void fetchTaskFn(void* arg);
void bgFetchStart();
void requestFetch();
void bgYield();
bool fetchStuck();
void loop();
void drawSettingsList();
void loopBody();
String thresholdError(int c, int l, int h, int v);
void fullFactoryWipe();
bool sameOriginOk();
int statsSpanHours(int used, uint32_t tMin, uint32_t tMax);
bool statsEnough(int used, uint32_t tMin, uint32_t tMax);
int teacherSensorCat();
int teacherSgv();
bool jpgDims(const uint8_t* j, uint32_t n, int& w, int& h);
void drawPwSpaced(const String& pw, int cx, int y, int cell, const lgfx::IFont* f, uint16_t col, uint16_t bg);
void txtButton(const String& t, uint16_t act);
void txtButtonTap(int act);
void openPreviewMenu();
void startTeacherPreview(int k);
uint16_t ageBarColor(unsigned long ageSec, bool filled, uint16_t emptyCol);
bool internetProbe();
String homeNetPillText();
WiFiClientSecure& nsSecure();

bool inaRead16(uint8_t reg, int16_t& v) {
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)INA219_ADDR, 2) != 2) return false;
  v = (int16_t)((Wire.read() << 8) | Wire.read());
  return true;
}

void inaInit() {
  Wire.beginTransmission(INA219_ADDR);
  inaPresent = (Wire.endTransmission() == 0);
  if (inaPresent) {
    // 16V aralik, +-80mV sont (0.1 ohm ile +-800mA; sarj ~500mA sigar), 12bit 128 ornek ortalama, surekli
    uint16_t cfg = 0x0000 | (0 << 13) | (1 << 11) | (0x0F << 7) | (0x0F << 3) | 0x07;
    Wire.beginTransmission(INA219_ADDR);
    Wire.write(0x00);
    Wire.write(cfg >> 8);
    Wire.write(cfg & 0xFF);
    Wire.endTransmission();
  }
  Serial.printf("INA219 pil olcer: %s\n", inaPresent ? "OK" : "YOK");
}

// 3.4.20: derin uykudan once pil olceri de uyut (~1 mA tasarruf); acilista inaInit tekrar calistirir
void inaPowerDown() {
  if (!inaPresent) return;
  uint16_t cfg = (1 << 11) | (0x0F << 7) | (0x0F << 3);     // mod 000 = kapali
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(0x00); Wire.write(cfg >> 8); Wire.write(cfg & 0xFF);
  Wire.endTransmission();
}

// Li-ion dinlenme gerilimi -> yuzde
// 3.4.23: gercek desarj olcumunden (04.10, ~135 mA, 0,9 ohm duzeltmeli gerilim);
// %0 = 3,30 V'ta "Pil bitti" kapanmasi (yuk altinda ~3,42 V duzeltilmis)
int liionPercent(int mv) {
  static const int tbl[][2] = {
    // 3.4.25: 135 mA (03-04.10) ve ~80 mA WiFi uykulu (04-05.10) desarj olcumlerinin ortalamasi
    {4150, 100}, {4000, 97}, {3950, 91}, {3900, 77}, {3850, 56}, {3825, 52}, {3755, 42},
    {3700, 34}, {3650, 27}, {3610, 21}, {3545, 13}, {3480, 6}, {3450, 3}, {3420, 0}
  };
  const int n = sizeof(tbl) / sizeof(tbl[0]);
  if (mv >= tbl[0][0]) return 100;
  if (mv <= tbl[n - 1][0]) return 0;
  for (int i = 0; i < n - 1; i++) {
    if (mv <= tbl[i][0] && mv > tbl[i + 1][0]) {
      int v0 = tbl[i + 1][0], v1 = tbl[i][0], p0 = tbl[i + 1][1], p1 = tbl[i][1];
      return p0 + (long)(mv - v0) * (p1 - p0) / (v1 - v0);
    }
  }
  return 0;
}

void readBattery() {
  lastBatRead = millis();
  if (!inaPresent) { batteryPercent = -1; batteryCharging = false; return; }
  int16_t bus, shunt;
  if (!inaRead16(0x02, bus) || !inaRead16(0x01, shunt)) {
    batteryPercent = -1; batteryCharging = false; return;
  }
  int busMv = ((uint16_t)bus >> 3) * 4;           // LSB 4 mV
  int shuntUv10 = shunt;                           // LSB 10 uV
  batteryMa = shuntUv10 / 10;                      // 0.1 ohm sont: 10uV = 0.1 mA
  int vbat = busMv + shuntUv10 / 100;              // pil tarafi = VIN- + sont dususu
  if (vbat < 2800 || vbat > 4500) {                // pil takili degil / anlamsiz
    batteryPercent = -1; batteryCharging = false; batteryMv = vbat; return;
  }
  // Yuk altinda gerilim duser, sarjda yukselir: ic direnc ile duzelt.
  // 3.4.12: sarjda gerilim daha cok sisiyor (ic direnc + polarizasyon) -> 0.35 ohm
  batteryMv = vbat;
  batteryCharging = (batteryMa < -15);
  int restMv = batteryCharging ? vbat + (batteryMa * 35) / 100
                               : vbat + (batteryMa * 90) / 100;   // 3.4.22: olcum 3,92 V / 133 mA -> yol direnci ~0,9 ohm
  float voltPct = (float)liionPercent(restMv);

  // 3.4.12: yuzde, pile giren/cikan akimla sinirlanir (coulomb sayma).
  // Sarjda gerilim hemen yukseldigi icin yuzde eskiden dakikalar icinde
  // %27 -> %49 ziplardi; artik en fazla akimin izin verdigi kadar artar.
  static float pctF = -1;
  static float restF = -1;                                  // 3.4.25: pille calisirken suzulmus gerilim
  static float chgI = -1, chgCeil = 0, chgStartPct = 0;     // 3.4.26: sarj akimi (suzulmus), akim tavani
  static unsigned long lastPctMs = 0;
  unsigned long nowMs = millis();
  float dtH = (lastPctMs == 0) ? 0 : (nowMs - lastPctMs) / 3600000.0f;
  lastPctMs = nowMs;
  if (pctF < 0 || batteryPercent < 0) {
    pctF = voltPct;                                         // ilk okuma / pil yeni takildi
    // 3.4.27: pil bitip kapandiktan sonra sarjla acildiysa dinlenen gerilim yaniltir (06.10: %16 gosterdi, gercek ~%2)
    if (wokeFromLowBat) { if (pctF > 2) pctF = 2; wokeFromLowBat = false; }
  } else if (batteryCharging) {
    restF = -1;
    if (chgI < 0) { chgI = -batteryMa; chgCeil = 0; chgStartPct = pctF; }
    else chgI += (-batteryMa - chgI) * 0.1f;
    // 3.4.27 (06.10 sarj olcumu): sarjda gerilim yaniltiyor (19:43'te %47 gosterdi, gercek ~%23) ->
    // yuzde sadece pile giren akimla artar (1750 mAh), ust sinir akim tavani tablosu (olculen egriden).
    pctF += (-batteryMa) * dtH / BAT_CAPACITY_MAH * 100.0f;
    // Tavan bir sarj boyunca hic dusmez, takildigi andaki yuzdenin altina inmez.
    static const int ctab[][2] = { {410, 48}, {366, 50}, {316, 60}, {288, 68}, {219, 78}, {188, 85},
                                   {135, 90}, {100, 94}, {72, 97}, {30, 99}, {0, 100} };
    float c = 48;
    if (chgI < 410) {
      for (int i = 0; i < 10; i++) {
        if (chgI <= ctab[i][0] && chgI >= ctab[i + 1][0]) {
          c = ctab[i + 1][1] + (chgI - ctab[i + 1][0]) * (ctab[i][1] - ctab[i + 1][1]) / (float)(ctab[i][0] - ctab[i + 1][0]);
          break;
        }
      }
      if (chgI < 0) c = 100;
    }
    if (c > chgCeil) chgCeil = c;
    float lim = max(chgCeil, chgStartPct);
    if (pctF > lim) pctF = lim;
  } else {
    // 3.4.26: sarj entegresi akimi kesti (USB takili, akim ~0) -> %100
    if (chgI >= 0 && chgCeil >= 95 && batteryMa > -15 && batteryMa < 15) pctF = 100;
    chgI = -1;
    // 3.4.22: pille calisirken de akim sayilir - yuzde, cekilen akimin izin verdiginden hizli dusmez
    // (sarjdan cikinca %100 -> %79 ziplamasin). 3,70 V altinda (pil sonu) yine gerilim izlenir.
    // 3.4.25 (05.10 olcumu): WiFi uykusunda akim/gerilim dalgalaniyor -> gerilim ~2 dk suzulur; pille yuzde
    // ASLA artmaz (3->13, 15->19 ziplamalari kalkti); dusus akimin 3 katiyla + saatte %2 ile sinirli (14->9->6 olmasin).
    if (restF < 0) restF = restMv; else restF += (restMv - restF) * 0.05f;
    float vp = (float)liionPercent((int)(restF + 0.5f));
    float maxDown = (batteryMa > 0 ? batteryMa : 0) * dtH / BAT_CAPACITY_MAH * 100.0f * 3.0f + dtH * 2.0f;
    if (batteryMa > -15 && batteryMa < 15 && restMv >= 4120 && voltPct > pctF) pctF = voltPct;   // sarj bitti, USB'de bekliyor
    else if (vp < pctF) pctF -= min(pctF - vp, maxDown);
  }
  // 3.4.26: %100 sarj bitince (entegre akimi keser, 04.10 gozlemi). Yedek: >= 4,15 V ve akim < 30 mA.
  // (3.4.21'in < 150 mA kurali kalkti - ekran saatlerce erken %99/100 gosteriyordu.)
  static bool chgFull = false;
  if (!batteryCharging) chgFull = false;
  else if (vbat >= 4150 && batteryMa > -30) chgFull = true;
  if (chgFull) pctF = 100;
  int pct = (int)(pctF + 0.5f);
  if (batteryCharging && !chgFull && pct > 99) pct = 99;    // sarj bitmeden %100 gosterme
  batteryPercent = constrain(pct, 0, 100);
}

// Ana ekranin sag ust kosesine kucuk pil simgesi (INA219 yoksa hic cizmez)
void drawBatteryIndicator() {
  if (!inaPresent || batteryPercent < 0) return;
  int w = cv->width();
  int bx = w - 30, by = 3, bw = 24, bh = 11;
  cv->fillRect(bx - 34, 0, 34 + bw + 6, bh + 6, UI_BG);
  uint16_t c = batteryCharging ? TFT_CYAN
             : (batteryPercent <= 20 ? sevColor(0) : (batteryPercent <= 30 ? sevColor(3) : sevColor(2)));
  cv->drawRect(bx, by, bw, bh, TFT_LIGHTGREY);
  cv->fillRect(bx + bw, by + 3, 2, bh - 6, TFT_LIGHTGREY);
  int fillW = (bw - 4) * batteryPercent / 100;
  if (fillW > 0) cv->fillRect(bx + 2, by + 2, fillW, bh - 4, c);
  if (batteryCharging) {
    // simsek
    int cx = bx + bw / 2, cy = by + bh / 2;
    const int ox[4] = {-1, 1, 0, 0}, oy[4] = {0, 0, -1, 1};   // 3.4.13: hep sari, koyu kenarli
    for (int k = 0; k < 4; k++) {
      cv->fillTriangle(cx + 1 + ox[k], by + 1 + oy[k], cx - 3 + ox[k], cy + 1 + oy[k], cx + ox[k], cy + 1 + oy[k], TFT_BLACK);
      cv->fillTriangle(cx - 1 + ox[k], by + bh - 1 + oy[k], cx + 3 + ox[k], cy - 1 + oy[k], cx + ox[k], cy - 1 + oy[k], TFT_BLACK);
    }
    cv->fillTriangle(cx + 1, by + 1, cx - 3, cy + 1, cx, cy + 1, TFT_YELLOW);
    cv->fillTriangle(cx - 1, by + bh - 1, cx + 3, cy - 1, cx, cy - 1, TFT_YELLOW);
  }
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  cv->setTextDatum(MR_DATUM);
  dsN(cv, String(batteryPercent) + "%", bx - 3, by + bh / 2 + 1, 1);
  lastDrawnBatPct = batteryPercent;
  lastDrawnBatChg = batteryCharging;
}


void clearOldFotoFiles() {
  if (!SPIFFS.begin(true)) return;
  const char* olds[] = {
    "/foto1.rgb", "/foto2.rgb", "/foto3.rgb",
    "/foto.rgb.bak", "/logo.rgb"
  };
  for (int i = 0; i < 5; i++) {
    if (SPIFFS.exists(olds[i])) SPIFFS.remove(olds[i]);
  }
  // Eski TEK-foto surumunden (foto.rgb) gecis: dogru boyuttaysa yeni
  // cok-foto yapisina (pic0.rgb) tasi ki kullanicinin mevcut fotosu
  // firmware guncellemesiyle kaybolmasin. pic0.rgb zaten varsa (yeni
  // yapi zaten kullanimda) eski dosya sadece siliniyor.
  if (SPIFFS.exists("/foto.rgb")) {
    File f = SPIFFS.open("/foto.rgb", FILE_READ);
    size_t sz = f ? f.size() : 0;
    if (f) f.close();
    if (sz == (size_t)FOTO_BYTES && !SPIFFS.exists("/pic0.rgb")) {
      SPIFFS.rename("/foto.rgb", "/pic0.rgb");
    } else {
      SPIFFS.remove("/foto.rgb");
    }
  }
}

// Yatay rotasyon: 1 veya 3 (TFT+touch ayni)
int landRot = 1;
const char* CONTACT_EMAIL = "metematik.t1d@gmail.com";
// 3.5.0: imza (Bilgi ekrani)
#define SIGNATURE_TEXT() L("Oğlum Mete için yaptım. Burak", "Made for my son Mete. Burak")
const char* YOUTUBE_HANDLE = "@metematik.t1d";   // 3.6.1: YouTube kanali

// Grafik saat araligi (3 / 6 / 12 / 24)
int graphHoursIdx = 1;  // default 6 saat
const int GRAPH_HOURS[] = {3, 6, 12, 24};
const int GRAPH_HOURS_N = 4;

// En buyuk (4.) secenegin GERCEK anlami kaynaga gore degisir: Dexcom her
// zaman 288/24 saat doner; Nightscout 288 istegi basarisiz olup 200'e
// dustuyse bu ~16 saate denk gelir; Libre zaten sadece ilk 3 secenegi
// gosteriyor (bkz. ghMax), o yuzden burada Libre'ye dokunmuyoruz.
int graphHourLabelFor(int idx) {
  int h = GRAPH_HOURS[idx];
  if (idx == GRAPH_HOURS_N - 1 && dataSource == "nightscout" && nsUsedFallback200) {
    h = 16;
  }
  return h;
}
bool graphDetailOpen = false; // NS: Detay (IOB/COB/bolus)
float nsIob = -1.0f;   // aktif insulin (U) — sadece NS
float nsCob = -1.0f;   // aktif karbonhidrat (g) — sadece NS
unsigned long nsIobOkMs = 0, nsCobOkMs = 0;   // 3.4.20: son basarili IOB/COB zamani
#define BOLUS_MAX 5
struct BolusRec { uint32_t ts; float units; char note[12]; };
BolusRec boluses[BOLUS_MAX];
int bolusCount = 0;
bool graphMenuOpen = false;  // sagdaki acilir kutu
bool uiMenuOpen = false;     // 2. sayfa arayuz acilir kutu
bool uiChromeVisible = false;   // 2.5.0: acilir kutu yok (Tema uygulamasi)
unsigned long lastUiChromeMs = 0;
int lastLibreSgv = -1;       // Libre yeni olcum tespiti
uint32_t lastLocalAppendTs = 0; // localHist'e en son eklenen okumanin zamani
// 4.0: eskiden fonksiyon icinde static olan, kisiye ait durumlar (aile modunda kisi basina saklanir)
String ctxLastSrc = "";               // fetchData: son kaynak
uint32_t lrTs[10]; int16_t lrV[10]; int lrN = 0; bool lrKeyTs = true;   // Libre degisim halkasi
unsigned long lrLastBurst = 0; int lrLastKind = -1;                    // Libre alarm tekrar
unsigned long nsFullOkMs = 0;         // NS son tam cekme
unsigned long medLastHistTry = 0;     // Medtrum gecmis denemesi
unsigned long lastCritBurst = 0, lastStaleBurst = 0;                   // alarm tekrar zamanlari
String personName = "";               // 4.0: kisinin adi (aile modu)
int personCount = 1;                  // 1..PERSON_MAX (etkin: aile modu kapaliyken 1)
bool famEnabled = false;              // 4.0.1: aile modu acik/kapali
int famCount = 2;                     // 4.0.1: aile modu acikken kisi sayisi (2..PERSON_MAX)
int famMode = 0;                      // 0 sirayla, 1 bolunmus ekran
int liveCtx = 0;                      // globallerde su an hangi kisi var
int dispCtx = 0;                      // ekranda gosterilen kisi
volatile int fetchCtx = -1;           // verisi cekilen kisi (-1: yok)
void ctxSaveTo(int k);
void ctxLoadFrom(int k);
void ctxSwitch(int k);
const char* personNs(int k);
void localHistSaveAll();
bool fetchAllPersons();
String personLabel(int k);
void initPersons();
String personNameOf(int k);
extern unsigned long famLastRot;
void drawFamQrScreen();
extern int famQrFrom;

// Zamanlayici (yemek oncesi bekleme)
bool timerRunning = false;
unsigned long timerEndMs = 0;
int timerTotalSec = 0;
bool timerFinishedPending = false;

#define BTN_Y1  128
#define BTN_Y2  178
#define BTN_H   42

const char* colorName(int i) {
  switch (i) {
    case 0: return L("Kırmızı", "Red");   case 1: return L("Turuncu", "Orange"); case 2: return L("Sarı", "Yellow");
    case 3: return L("Beyaz", "White");   case 4: return L("Yeşil", "Green");    default: return L("Mavi", "Blue");
  }
}
const uint16_t colorVals[] = {TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_WHITE, TFT_GREEN, TFT_CYAN};
// Secili / secili degil (net ayrim)
#define SEL_BG   0x07FF
#define UNSEL_BG 0x18C3
#define SEL_FG   TFT_BLACK
#define UNSEL_FG TFT_WHITE


bool useMinuteBars() { return (dataSource != "libre"); }

const char* uiModeName(int m) {
  // Isimler Kullanici istegiyle daha klinik/sade + hafif eglenceli bir
  // tona cekildi: "Basit" -> "Klasik", "Fotoğraf" -> "Foto Çerçeve".
  // "Büyük" ve "Takvim" zaten net oldugu icin degistirilmedi.
  switch (m) {
    case 1: return L("Büyük", "Big");
    case 2: return L("Takvim", "Calendar");
    case 3: return L("Dikey", "Portrait");
    case 4: return L("Foto Çerçeve", "Photo Frame");
    default: return L("Klasik", "Classic");
  }
}

void drawUiDropdownChrome() {
  // Sag ust acilir kutu (Dikey arayuz kaldirildi: 4 secenek)
  int boxX = 210, boxW = 106;
  int cx = boxX + boxW / 2;
  cv->fillRoundRect(boxX, 2, boxW, 32, 6, 0x4B72);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(uiModeName(uiPageMode), cx, 16, &FreeSansTR7pt);
  if (uiMenuOpen) {
    const int dispModes[] = {0, 1, 2, 4};
    int listX = 170, listW = 146, rowH = 34;
    for (int di = 0; di < 4; di++) {
      int m = dispModes[di];
      int my = 34 + di * rowH;
      bool sel = (m == uiPageMode);
      cv->fillRoundRect(listX, my, listW, rowH - 2, 4, sel ? 0x0B5E : 0x2945);
      cv->setTextColor(TFT_WHITE, sel ? 0x0B5E : 0x2945);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(uiModeName(m), listX + listW / 2, my + rowH / 2 - 1, &FreeSansTR7pt);
    }
  }
}

String sourceLabel() {
  if (dataSource == "dexcom") return "Dexcom";
  if (dataSource == "medtrum") return "Medtrum";
  if (dataSource == "libre") return "LibreLinkUp";
  return "NightScout";
}

int screenCount() { return SCR_COUNT; }          // 2.5.0: sabit ekran numaralari
int statsScreenIndex() { return SCR_STATS; }
int timerScreenIndex() { return SCR_TIMER; }
int settingsScreenIndex() { return SCR_SETTINGS; }
bool isStatsScreen() { return currentScreen == statsScreenIndex(); }
bool isTimerScreen() { return currentScreen == timerScreenIndex(); }
bool isSettingsScreen() { return currentScreen == settingsScreenIndex(); }

uint16_t getAlarmColor() {
  if (alarmColorIdx < 0 || alarmColorIdx > 5) return sevColor(0);
  return colorVals[alarmColorIdx];
}

// Cift ok alarmlari: sadece taze veride (10 dk'dan eski trend gecersiz)
bool fastTrendFresh() {
  return dataValid && readingAgeSec <= 10UL * 60UL;
}
bool fastFallActive() {
  return alarmFastFallOn && fastTrendFresh() && (direction == "DoubleDown" || (dataSource == "libre" && delta <= -15));
}
bool fastRiseActive() {
  return alarmFastRiseOn && fastTrendFresh() && (direction == "DoubleUp" || (dataSource == "libre" && delta >= 15));
}

// Alarm kosulu (snooze disinda)
// Erteleme: sadece ertelendigi andaki uyari turunu susturur; tur degisirse (ornegin
// yuksekten kritik dusuge) hemen yine calar. Sure dolunca bayrak kapanir.
int getAlarmKind();
bool snoozedNow() {
  if (!snoozeActive) return false;
  if ((long)(millis() - alarmSnoozeUntil) >= 0) { snoozeActive = false; return false; }
  int k = getAlarmKind();
  if (k == 0 && sgv > alarmLow + 10 && sgv < alarmHigh - 10) { snoozeActive = false; return false; }   // 3.4.3: belirgin normale dondu
  if (k == snoozedKind) return true;
  if (snoozedKind == 1 && k == 2) return true;          // kritikten duzelirken dusuk de sussun
  if (snoozedKind == 4 && k == 3) return true;          // cok yuksekten inerken yuksek de sussun
  return false;
}
// Ertele dugmesi / mesajinda gosterilecek dakika
int snoozeShownMins() {
  int m = snoozeMinutes;
  if (getAlarmKind() == 1 && (m == 0 || m > 15)) m = 15;
  return m;
}
void startSnooze() {
  snoozedKind = getAlarmKind();
  unsigned long mins = (unsigned long)snoozeMinutes;
  if (snoozedKind == 1 && (mins == 0 || mins > 15)) mins = 15;   // kritik dusuk: en fazla 15 dk
  alarmSnoozeUntil = millis() + mins * 60UL * 1000UL;
  snoozeActive = true;
}
bool isAlarmCondition() {
  if (snoozedNow()) return false;
  // Veri yok
  if (!dataValid || (staleAlarmMin > 0 && readingAgeSec > (unsigned long)staleAlarmMin * 60UL)) {
    if (!dataValid) return true;
    if (readingAgeSec > (unsigned long)staleAlarmMin * 60UL) return true;
  }
  if (!dataValid) return false;
  if (sgv < alarmCritLow || sgv < alarmLow) return true;
  if (sgv >= alarmVeryHigh || sgv > alarmHigh) return true;
  if (fastFallActive() || fastRiseActive()) return true;
  return false;
}

// Ses + erteleme icin (gorsel kapali olsa da ses calabilir)
bool isAlarmActive() {
  return isAlarmCondition();
}


// Dikkat butonu: sadece kritik araliklar (hipo veya cok yuksek)
bool shouldShowDikkat() {
  if (!teacherModeOn || !dataValid) return false;
  if (homeDataStale()) return false;
  if (teacherStep > 0) return false;
  if (sgv <= 0) return false;
  // Kullanici esikleri: dusuk alti veya cok yuksek ustu
  if (sgv < alarmLow || sgv >= alarmVeryHigh) return true;
  return false;
}
bool shouldShowSnoozeButton() {
  // Cihaz sessizdeyse (genel ses, gece/okul modu sessiz) ya da o alarm
  // tipinin sesi kapaliysa ertelenecek bir ses yok - buton gosterilmez.
  if (!isAlarmActive()) return false;
  int k = getAlarmKind();
  if (effectiveSoundLevel() == 0) return false;          // sessiz: calacak ses yok (eskisi gibi)
  if (k > 0 && !alarmSoundEnabledForKind(k)) return false;
  if (k == 1) return true;                  // kritik dusuk: "Ertele kapali" olsa da 10 dk susturulabilir
  if (snoozeMinutes <= 0) return false;
  if (effectiveSoundLevel() == 0) return false;
  if (k > 0 && !alarmSoundEnabledForKind(k)) return false;
  return true;
}

int getAlarmKind() {
  // 1 kritik 2 dusuk 3 yuksek 4 cok yuksek 5 stale
  if (!dataValid) return 5;
  if (staleAlarmMin > 0 && readingAgeSec > (unsigned long)staleAlarmMin * 60UL)
    return (sgv > 20 && sgv < alarmCritLow && readingAgeSec <= 3600UL) ? 1 : 5;   // 3.4.3: son deger kritik dusukse (1 saate kadar) kritik alarm
  if (sgv < alarmCritLow) return 1;
  if (sgv < alarmLow) return 2;
  if (sgv >= alarmVeryHigh) return 4;
  if (sgv > alarmHigh) return 3;
  // Esik icindeyken cift ok: esik alarmlari her zaman once gelir
  if (fastFallActive()) return 6;
  if (fastRiseActive()) return 7;
  return 0;
}

// Seker esik disinda olsa da normale dogru belirgin bir hizla gidiyor mu?
// Dusukte hem YON (trend oku) hem MIKTAR (delta, mg/dL) birlikte bakiliyor.
// 3.4.22: Yuksek/Cok yuksekte delta <= -5 yeterli (ok yatay olabilir).
// Kritik Dusuk (kind==1) icin BILEREK her zaman false doner: o bolgede
// trend her an tersine donebilir, "duzeliyor gibi" gorunup gercekte
// donmeyebilir - bu yuzden Kritik Dusuk alarmi hicbir zaman bu yolla
// susturulmaz. Onaylanan tasarim (31 Agustos 2026).
bool isRecovering(int kind) {
  if (!recoverySoundSuppressOn) return false;
  if (!dataValid) return false;
  if (kind == 1) return false; // Kritik Dusuk: istisna, asla bastirma
  if (kind == 2) { // Dusuk: yukari mi gidiyor
    bool up = (direction == "SingleUp" || direction == "DoubleUp" || direction == "FortyFiveUp");
    return up && delta >= RECOVERY_DELTA_MGDL;
  }
  if (kind == 3 || kind == 4) { // Yuksek / Cok yuksek: asagi mi gidiyor
    bool down = (direction == "SingleDown" || direction == "DoubleDown" || direction == "FortyFiveDown");
    (void)down;
    return delta <= -RECOVERY_DELTA_MGDL;   // 3.4.22: Yuksekte ok yatay olsa da delta yeterli (NS yavas dususte ok gostermiyor)
  }
  return false; // kind 0 (alarm yok) veya 5 (veri yok) icin gecerli degil
}

// Esik basina ses ac/kapa kontrolu - kritik/dusuk/yuksek/cok yuksek/veri yok
// icin ayri ayri. Kapatilan bir esikte GORSEL alarm (varsa) yine calisir,
// sadece ses calmaz.
bool alarmSoundEnabledForKind(int kind) {
  switch (kind) {
    case 1: return alarmCritLowSound;
    case 2: return alarmLowSound;
    case 3: return alarmHighSound;
    case 4: return alarmVeryHighSound;
    case 5: return alarmStaleSound;
    default: return true;
  }
}

void stepHalfHour(int &hour, int &mins, int dir) {
  int total = hour * 60 + mins;
  total = (total + dir * 30 + 1440) % 1440;
  hour = total / 60;
  mins = total % 60;
}

bool isNightHours() {
  if (!nightModeOn) return false;
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) return false;
  time_t local = tv.tv_sec + 3 * 3600;
  struct tm *tm = gmtime(&local);
  int m = tm->tm_hour * 60 + tm->tm_min;
  int startM = nightStartHour * 60 + nightStartMin;
  int endM = nightEndHour * 60 + nightEndMin;
  if (startM == endM) return true;
  if (startM < endM)
    return (m >= startM && m < endM);
  // ornek 22:00 -> 07:00
  return (m >= startM || m < endM);
}

bool isSchoolHours() {
  if (!schoolModeOn) return false;
  if (isNightHours()) return false; // gece oncelikli
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) return false;
  time_t local = tv.tv_sec + 3 * 3600;
  struct tm *tm = gmtime(&local);
  int m = tm->tm_hour * 60 + tm->tm_min;
  if (tm->tm_wday == 0 || tm->tm_wday == 6) return false;   // 3.4.3: hafta sonu okul yok
  int startM = schoolStartHour * 60 + schoolStartMin;
  int endM = schoolEndHour * 60 + schoolEndMin;
  if (startM == endM) return true;
  if (startM < endM)
    return (m >= startM && m < endM);
  // geceyi asan aralik (nadir)
  return (m >= startM || m < endM);
}

void applyBrightness() {
  if (screenAsleep) { ledcWrite(BACKLIGHT_PIN, 0); return; }
  int level = brightnessLevel;
  if (isNightHours()) level = nightBrightLevel;
  else if (isSchoolHours()) level = schoolBrightLevel;
  if (lowBatDim && level > 0) level = 0;          // %20 alti pil: ekran kisik
  int pwm = BRIGHT_HIGH;
  if (level == 0) pwm = BRIGHT_LOW;
  else if (level == 1) pwm = BRIGHT_MED;
  ledcWrite(BACKLIGHT_PIN, pwm);
}

int effectiveSoundLevel() {
  if (isNightHours()) return nightSoundLevel;
  if (isSchoolHours()) return schoolSoundLevel;
  return soundLevel;
}
// Alarm sesi calmayacaksa kullaniciya gosterilecek kisa uyari (bos = sorun yok)
String soundWarnText() {
  if (effectiveSoundLevel() == 0) {
    if (isNightHours()) return L("Gece modu sessiz: alarm çalmaz", "Night mode silent: no alarm sound");
    if (isSchoolHours()) return L("Okul modu sessiz: alarm çalmaz", "School mode silent: no alarm sound");
    return L("Sessiz: alarmlar çalmaz", "Silent: alarms won't sound");
  }
  if (!alarmCritLowSound) return L("Kritik düşük sesi kapalı", "Critical low sound off");
  if (!alarmLowSound || !alarmHighSound || !alarmVeryHighSound || !alarmStaleSound)
    return L("Bazı alarm sesleri kapalı", "Some alarm sounds off");
  return "";
}

void cycleBrightness() {
  brightnessLevel = (brightnessLevel + 1) % 3;
  applyBrightness();
  prefs.begin("metematik", false);
  prefs.putInt("bright", brightnessLevel);
  prefs.end();
}

void applyRotation() {
  // Dikey arayuz (mod 5) veya onizleme dikey iken 0; aksi yatay landRot
  bool portrait = !showErrorScreen && !ageWarnActive && !formId && !textPageActive && !timePickerActive &&
                  ((currentScreen == 1 && uiPageMode == 3 && teacherStep == 0) ||
                   (currentScreen == SCR_THEME && themePreview && thSel == 7));
  int rot = portrait ? 0 : landRot;
  // CYD'deki donus numarasi (1 = yatay, 3 = ters yatay, 0 = dikey) korunuyor;
  // CrowPanel'de panel 3 adim kaydirilmis oldugu icin LovyanGFX'e (rot+3)%4.
  tft.setRotation((rot + 3) % 4);
  tftEffRot = (rot + 2) % 4;
}

// ---- FT6336U kapasitif dokunmatik ----
bool touchChipOk = false;

bool ftReadRegs(uint8_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)FT6336_ADDR, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// Ham dokunus (panelin dogal 240x320 cercevesi). true = parmak ekranda.
bool touchReadRaw(int& rx, int& ry) {
  uint8_t d[5];
  if (!ftReadRegs(0x02, d, 5)) return false;
  uint8_t n = d[0] & 0x0F;
  if (n == 0 || n > 2) return false;
  rx = ((d[1] & 0x0F) << 8) | d[2];
  ry = ((d[3] & 0x0F) << 8) | d[4];
  return true;
}

void touchInit() {
  // RST'yi kisa sure asagi cekip birak (FT6336 guvenli baslangic)
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);  delay(10);
  digitalWrite(TP_RST, HIGH); delay(120);
  pinMode(TP_INT, INPUT);
  Wire.beginTransmission(FT6336_ADDR);
  touchChipOk = (Wire.endTransmission() == 0);
  Serial.printf("Dokunmatik FT6336: %s\n", touchChipOk ? "OK" : "YOK");
}

// Ham koordinati o anki ekran donusune gore ekran koordinatina cevir.
// Donanim testinde olculdu: ham X/Y panelin dogal cercevesiyle birebir.
void mapTouchToScreen(int rx, int ry, int& x, int& y) {
  rx = constrain(rx, 0, 239);
  ry = constrain(ry, 0, 319);
  switch (tftEffRot) {
    case 0: x = rx;        y = ry;        break;   // dogal dikey
    case 1: x = ry;        y = 239 - rx;  break;   // yatay
    case 2: x = 239 - rx;  y = 319 - ry;  break;   // ters dikey (MeteMatik dikey modu)
    default: x = 319 - ry; y = rx;        break;   // yatay (varsayilan)
  }
  x = constrain(x, 0, cv->width() - 1);
  y = constrain(y, 0, cv->height() - 1);
}


String brightnessLabel() {
  if (brightnessLevel == 0) return L("Dusuk", "Low");
  if (brightnessLevel == 1) return L("Orta", "Med");
  return L("Yuksek", "High");
}

// mg/dL veya mmol/L gosterim (dahili her zaman mg/dL)
String formatGlucose(int mgdl) {
  if (unitIdx == 0) return String(mgdl);
  float m = mgdl / 18.0f;
  char b[12];
  dtostrf(m, 0, 1, b);
  return String(b);
}

String formatDelta(int dMg) {
  if (deltaUnknown) return "--";                     // 3.6.1: uzun kesinti
  const char* st = deltaInterp ? "*" : "";           // 3.6.1: 5 dakikaya cevrildi
  if (unitIdx == 0) {
    if (dMg > 0) return String("+") + String(dMg) + st;
    return String(dMg) + st;
  }
  float m = dMg / 18.0f;
  char b[12];
  dtostrf(m, 0, 1, b);
  if (m > 0.05f) return String("+") + String(b) + st;
  return String(b) + st;
}

String unitLabel() { return (unitIdx == 0) ? "mg/dL" : "mmol/L"; }
String langLabel() { return LANG_CODES[(langIdx >= 0 && langIdx < LANG_COUNT) ? langIdx : 0]; }

// 3.6.0: yerlesik (numarali) LovyanGFX fontlarinda yalniz ASCII harf var. Turkce ve
// Ingilizcede bu cizimler oldugu gibi kalir; diger dillerde metin ASCII disi harf
// iceriyorsa ayni yere benzer boyda Noto fontuyla cizilir (o -> o, e -> e kaybolmasin).
static bool dsNonAscii(const char* t) { for (; *t; t++) if ((uint8_t)*t >= 0x80) return true; return false; }
void dsN(lgfx::LovyanGFX* g, const char* t, int x, int y, int f) {
  if (langIdx >= 2 && dsNonAscii(t)) {
    const GFXfont* gf = (f <= 1) ? &FreeSansTR6pt : (f == 2 ? &FreeSansTR7pt : &FreeSansTR9pt);
    g->setFont((const lgfx::IFont*)gf);
    g->drawString(t, x, y);
    g->setTextFont(f);
    return;
  }
  g->drawString(t, x, y, f);
}
void dsN(lgfx::LovyanGFX* g, const String& t, int x, int y, int f) { dsN(g, t.c_str(), x, y, f); }

// 3.6.0: ay adlari dile gore (1..12)
const char* monthName(int m) {
  switch (m) {
    case 1: return L("Ocak", "January");     case 2: return L("Şubat", "February");
    case 3: return L("Mart", "March");       case 4: return L("Nisan", "April");
    case 5: return L("Mayıs", "May");        case 6: return L("Haziran", "June");
    case 7: return L("Temmuz", "July");      case 8: return L("Ağustos", "August");
    case 9: return L("Eylül", "September");  case 10: return L("Ekim", "October");
    case 11: return L("Kasım", "November");  default: return L("Aralık", "December");
  }
}
const char* monthShortName(int m) {
  switch (m) {
    case 1: return L("Oca", "Jan");  case 2: return L("Şub", "Feb");  case 3: return L("Mar", "Mar");
    case 4: return L("Nis", "Apr");  case 5: return L("May", "May");  case 6: return L("Haz", "Jun");
    case 7: return L("Tem", "Jul");  case 8: return L("Ağu", "Aug");  case 9: return L("Eyl", "Sep");
    case 10: return L("Eki", "Oct"); case 11: return L("Kas", "Nov"); default: return L("Ara", "Dec");
  }
}
// "Pil: %57" (TR) / "Battery: 57%" (EN) gibi: bicim dilden gelir
String fmtS(const char* fmt, const String& a) {
  char b[64];
  snprintf(b, sizeof(b), fmt, a.c_str());
  return String(b);
}
String batteryPctText(const char* fmt) {
  char b[48];
  snprintf(b, sizeof(b), fmt, batteryPercent);
  return String(b);
}

// TR/EN kisa metin. Not: numarali (built-in) TFT fontlarinda hala Turkce ozel
// karakter yok - o yuzden font 1 (en kucuk, ~8px) kullanan yerlerde "tr" hala
// ASCII yazilir. Font 2/4 kullanan yerlerde artik gercek Turkce karakterler
// (ı,ğ,ş,ö,ü,ç) kullanilabiliyor, cunku onlar drawFreeTitle() ile ozel
// FreeSansTR7pt/FreeSansTR9pt (GFXFF) fontuna ciziliyor - bkz. dosyanin
// basindaki Turkce font tanimlari.
// 3.6.0: Turkce ve Ingilizce kaynak koddaki metinler; diger diller metematik_lang.h
// tablosundan (lang/*.json -> lang/gen_lang.py). Ceviri yoksa Ingilizce gosterilir.
static uint32_t langFnv(const char* s, uint32_t h) {
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}
static const char* langFind(uint32_t key, int col) {
  int lo = 0, hi = LANG_TABLE_N - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t k = LANG_KEYS[mid];
    if (k == key) return LANG_TXT[mid][col];
    if (k < key) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}
const char* L(const char* tr, const char* en) {
  if (langIdx == 0) return tr;
  if (langIdx == 1 || langIdx >= LANG_COUNT) return en;
  const char* t = langFind(langFnv(en, langFnv("\x1f", langFnv(tr, 2166136261u))), langIdx - 1);
  return t ? t : en;
}
// Web sayfasi metinleri: kaynakta yalniz Turkce; Ingilizce dahil diger diller tablodan.
const char* WT(const char* tr) {
  if (langIdx == 0 || langIdx >= LANG_COUNT) return tr;
  uint32_t key = langFnv("\x1e", langFnv(tr, 2166136261u));
  const char* t = langFind(key, langIdx - 1);
  if (!t && langIdx > 1) t = langFind(key, 0);
  return t ? t : tr;
}

String soundLabel() {
  if (soundLevel < 0 || soundLevel > 3) return L("Orta", "Med");
  switch (soundLevel) {
    case 0: return L("Sessiz", "Mute");
    case 1: return L("Düşük", "Low");
    case 2: return L("Orta", "Med");
    default: return L("Yüksek", "High");
  }
}

// Buzzer icin frekans esleme: melodi/bip tonlari 2 oktav (x4) yukari tasinir,
// boylece buzzer'in en guclu oldugu 2-4.4 kHz bandina duser ve melodinin
// kendisi (notalar arasi oranlar) aynen korunur.
int buzzFreq(int f) {
  if (f <= 0) return 0;
  f *= 4;
  if (f < 1800) f = 1800;
  if (f > 4400) f = 4400;
  return f;
}
void spkTone(int f)    { lastToneMs = millis(); tone(SPEAKER_PIN, buzzFreq(f)); }
void spkToneRaw(int f) { lastToneMs = millis(); tone(SPEAKER_PIN, f); }   // alarm tonlari: dogrudan (zaten buzzer bandinda)

void stopSpeaker() {
  noTone(SPEAKER_PIN);
  digitalWrite(SPEAKER_PIN, LOW);
}

extern volatile bool fetchBusy;
void serviceAlarmSound() {
  int snd = effectiveSoundLevel();
  unsigned long now = millis();

  // Yeni olcumde burst baslat (3.4.20: veri cekme bitmeden degil - yarim veriyle yanlis "veri yok" bipi olmasin)
  if (requestAlarmBurst && !fetchBusy) {
    requestAlarmBurst = false;
    int kind = getAlarmKind();
    if (kind > 0 && isAlarmCondition()) {
      // Gorsel: ses kapali olsa da kisa yanip sonme
      // kritik / dusuk / yuksek / cok yuksek: 3 sn
      unsigned long dur = 3000UL;
      if (visualAlarmOn) {
        visualBurstEnd = now + dur;
        alarmBlinkState = true;
        lastBlink = now;
      }
      if (kind > 0 && snd > 0 && alarmSoundEnabledForKind(kind) && !isRecovering(kind)) {
        alarmBurstKind = kind;
        alarmBurstEnd = now + dur;
        beepPhase = 0;
        lastBeepPhase = 0;
        fastBeepSeq = 0;
      }
    }
  }

  // Kritik dusuk: Ertele'ye basilana kadar 5 dakikada bir 5 sn (3.4.20; yeni olcum beklenmez).
  // Sessiz modda / kritik dusuk sesi kapaliyken ses yok (sadece gorsel) - monitor olarak kullananlar icin.
  if (isAlarmCondition() && getAlarmKind() == 1) {   // 4.0: lastCritBurst kisi basina (global)
    if (lastCritBurst == 0 || now - lastCritBurst > 300000UL) {
      bool critSnd = (snd > 0 && alarmCritLowSound);      // sessiz / kritik sesi kapaliysa sadece gorsel
      lastCritBurst = now;
      if (visualAlarmOn) { visualBurstEnd = now + 5000UL; alarmBlinkState = true; lastBlink = now; }
      if (critSnd) {
        alarmBurstKind = 1;
        alarmBurstEnd = now + 5000UL;
        beepPhase = 0; lastBeepPhase = 0; fastBeepSeq = 0;
      }
    }
  } else lastCritBurst = 0;

  // Veri yok: 5 dakikada bir, 2 sn
  if (isAlarmCondition() && getAlarmKind() == 5) {
    if (now - lastStaleBurst > 300000UL && (long)(now - alarmBurstEnd) >= 0 && (long)(now - visualBurstEnd) >= 0) {
      lastStaleBurst = now;
      if (visualAlarmOn) {
        visualBurstEnd = now + 2000UL;
        alarmBlinkState = true;
      }
      if (snd > 0 && alarmSoundEnabledForKind(5)) {
        alarmBurstKind = 5;
        alarmBurstEnd = now + 2000UL;
        beepPhase = 0;
        lastBeepPhase = 0;
      }
    }
  }

  if (!isAlarmCondition() || snd == 0 || (long)(now - alarmBurstEnd) >= 0 || alarmBurstKind == 0) {
    if (beepPhase != 0) { stopSpeaker(); beepPhase = 0; }
    if (!isAlarmCondition()) alarmBurstKind = 0;
    return;
  }

  // Ses seviyesi: genel volume (kisa/seyrek <-> uzun/sik)
  int onMs, gapMs;
  if (snd == 1) { onMs = 60; gapMs = 280; }
  else if (snd == 2) { onMs = 100; gapMs = 180; }
  else { onMs = 140; gapMs = 120; }

  // 4 tip net ayirt edilir
  // CrowPanel buzzer'i ~4 kHz'de en yuksek sesi veriyor; tonlar bu banda
  // tasindi. Tipler hem incelik hem ritimle ayirt edilir (CYD'deki siralama
  // korunarak: kritik en tiz/hizli, veri yok en kalin/yavas).
  int freq = 3000;
  if (alarmBurstKind == 1) {      // kritik: en tiz, cok hizli
    freq = 4000;
    onMs = 50; gapMs = 70;
  } else if (alarmBurstKind == 2) { // dusuk: tiz
    freq = 3500;
    onMs = max(70, onMs);
    gapMs = max(150, gapMs);
  } else if (alarmBurstKind == 3) { // yuksek: daha kalin
    freq = 2700;
    onMs = max(100, onMs);
    gapMs = max(160, gapMs);
  } else if (alarmBurstKind == 4) { // cok yuksek: iki ton
    freq = ((now / 180) % 2 == 0) ? 2700 : 4000;
    onMs = 120; gapMs = 100;
  } else if (alarmBurstKind == 6 || alarmBurstKind == 7) {
    // cift ok: 3'lu hizli bip + uzun ara. Dusus: tizden kalina (4000-3500-3000),
    // yukselis: kalindan tize (3000-3500-4000) - diger alarmlardan ayirt edilir.
    static const int fallF[3] = {4000, 3500, 3000};
    static const int riseF[3] = {3000, 3500, 4000};
    int idx = fastBeepSeq % 3;
    freq = (alarmBurstKind == 6) ? fallF[idx] : riseF[idx];
    onMs = 80;
    gapMs = (fastBeepSeq > 0 && fastBeepSeq % 3 == 0) ? 450 : 60;
  } else { // stale: yavas, en kalin
    freq = 2300;
    onMs = 150; gapMs = 350;
  }

  if (beepPhase == 0) {
    if (lastBeepPhase == 0 || now - lastBeepPhase >= (unsigned long)gapMs) {
      spkToneRaw(freq);
      fastBeepSeq++;
      beepPhase = 1;
      lastBeepPhase = now;
    }
  } else {
    if (now - lastBeepPhase >= (unsigned long)onMs) {
      stopSpeaker();
      beepPhase = 0;
      lastBeepPhase = now;
    }
  }
}

void playTestBeep() {
  if (soundLevel == 0) return;
  int freq = 800;
  int onMs = (soundLevel == 1) ? 80 : (soundLevel == 2) ? 150 : 250;
  spkTone(freq);
  delay(onMs);
  stopSpeaker();
}

// Sensor/Set "15 dakika kaldi" uyarisi icin cok kisa tek bip - kullanicinin
// istegiyle SADECE 15 dakika esiginde calar (12sa/3sa/1sa sessiz/gorsel).
// Genel ses seviyesine (effectiveSoundLevel — gece/okul modunu da kapsar)
// uyuyor, Sessiz'de hic calmaz.
void playAgeWarnBeep() {
  if (effectiveSoundLevel() == 0) return;
  spkTone(900);
  delay(90);
  stopSpeaker();
}

uint32_t localDayStamp() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) return 0;
  time_t local = tv.tv_sec + 3 * 3600;
  struct tm *tm = gmtime(&local);
  return (uint32_t)(tm->tm_year + 1900) * 10000UL + (tm->tm_mon + 1) * 100UL + tm->tm_mday;
}

String todayDateStr() {
  uint32_t d = localDayStamp();
  if (d == 0) return "----";
  char buf[16];
  sprintf(buf, "%02lu.%02lu.%04lu", d % 100, (d / 100) % 100, d / 10000);
  return String(buf);
}

uint32_t dayStampFromMs(uint64_t ms) {
  if (ms < 1000000000000ULL) return 0;
  time_t sec = (time_t)(ms / 1000ULL) + 3 * 3600;
  struct tm *tm = gmtime(&sec);
  return (uint32_t)(tm->tm_year + 1900) * 10000UL + (tm->tm_mon + 1) * 100UL + tm->tm_mday;
}

// --- Kaynak-bagimsiz cihaz-tarafli 24 saatlik gunluk ---
// Sadece gercekten YENI bir okuma geldiginde cagrilir (Nightscout/Libre
// icin - Dexcom Share zaten API'den tek seferde 24s/288 aliyor, ona
// gerek yok). Son 24 saatten eski kayitlari otomatik atar; LOCAL_HIST_MAX
// kapasiteyi asarsa en eskiyi dusurur.
// 3.4.15: cihazin kendi kaydi (localHist) SPIFFS'te saklanir - pille kullanimda kapatip acinca
// (Libre'nin 12 saatlik siniri disindaki) grafik/istatistik kaybolmasin. 15 dk'da bir + kapanirken yazilir.
#define LH_PATH lhPath()
const char* lhPath() { return liveCtx == 1 ? "/lh1.bin" : (liveCtx == 2 ? "/lh2.bin" : "/lh.bin"); }   // 4.0: kisi basina
unsigned long lhLastSave = 0;
bool lhDirty = false;
uint32_t lhKeyHash() {
  String k = dataSource + "|";
  if (dataSource == "libre") k += libreEmail;
  else if (dataSource == "dexcom") k += dexUser;
  else if (dataSource == "medtrum") k += medEmail;
  else k += nightscoutUrl;
  uint32_t h = 2166136261u;
  for (unsigned i = 0; i < k.length(); i++) { h ^= (uint8_t)k[i]; h *= 16777619u; }
  return h;
}
void localHistSave(bool force) {
  if (!lhDirty) return;
  if (!force && lhLastSave && millis() - lhLastSave < 15UL * 60000UL) return;
  if (!SPIFFS.begin(true)) return;
  File f = SPIFFS.open(LH_PATH, FILE_WRITE);
  if (!f) return;
  uint32_t hdr[3] = {0x484C4D4DUL, lhKeyHash(), (uint32_t)localHistCount};
  f.write((const uint8_t*)hdr, sizeof(hdr));
  for (int i = 0; i < localHistCount; i++) {
    int16_t v = (int16_t)localHist[i];
    f.write((const uint8_t*)&v, 2);
    f.write((const uint8_t*)&localHistTs[i], 4);
  }
  f.close();
  lhLastSave = millis(); lhDirty = false;
}
void localHistLoad() {
  if (!SPIFFS.begin(true) || !SPIFFS.exists(LH_PATH)) return;
  File f = SPIFFS.open(LH_PATH, FILE_READ);
  if (!f) return;
  uint32_t hdr[3] = {0, 0, 0};
  bool ok = f.read((uint8_t*)hdr, sizeof(hdr)) == sizeof(hdr) && hdr[0] == 0x484C4D4DUL &&
            hdr[1] == lhKeyHash() && hdr[2] <= LOCAL_HIST_MAX;
  int n = 0; uint32_t prevTs = 0;
  if (ok) {
    for (uint32_t i = 0; i < hdr[2]; i++) {
      int16_t v; uint32_t t;
      if (f.read((uint8_t*)&v, 2) != 2 || f.read((uint8_t*)&t, 4) != 4) break;
      if (v < 40 || v > 500 || t < 1600000000UL || t < prevTs) continue;
      localHist[n] = v; localHistTs[n] = t; prevTs = t; n++;
    }
  }
  f.close();
  if (!ok) { SPIFFS.remove(LH_PATH); return; }        // baska kaynak/hesap: eski kayit kullanilmaz
  localHistCount = n;
  if (n > 0) lastLocalAppendTs = localHistTs[n - 1];
}

void appendLocalHistory(int sgvVal, uint32_t tsSec) {
  if (sgvVal < 40 || sgvVal > 500 || tsSec == 0) return;
  lhDirty = true;
  if (localHistCount < LOCAL_HIST_MAX) {
    localHist[localHistCount] = sgvVal;
    localHistTs[localHistCount] = tsSec;
    localHistCount++;
  } else {
    for (int i = 1; i < LOCAL_HIST_MAX; i++) {
      localHist[i - 1] = localHist[i];
      localHistTs[i - 1] = localHistTs[i];
    }
    localHist[LOCAL_HIST_MAX - 1] = sgvVal;
    localHistTs[LOCAL_HIST_MAX - 1] = tsSec;
  }
  uint32_t cutoff = (tsSec > 24UL * 3600UL) ? tsSec - 24UL * 3600UL : 0;
  int dropN = 0;
  while (dropN < localHistCount && localHistTs[dropN] < cutoff) dropN++;
  if (dropN > 0) {
    for (int i = dropN; i < localHistCount; i++) {
      localHist[i - dropN] = localHist[i];
      localHistTs[i - dropN] = localHistTs[i];
    }
    localHistCount -= dropN;
  }
  localHistSave(false);
}

// En fazla ~5 dakikada bir localHist'e ekler - kaynagin canli okuma
// sikligi API'nin grafik cozunurluguyle ayni olmayabilir, bu sabit
// araligi korumaya calisir.
void trackLocalReading(int sgvVal, uint32_t tsSec, uint32_t &lastAppendTs) {
  if (tsSec == 0) return;
  if (lastAppendTs && tsSec <= lastAppendTs) return;      // 3.4.3: geriye giden zaman eklenmez
  if (lastAppendTs == 0 || tsSec - lastAppendTs >= 240) {
    appendLocalHistory(sgvVal, tsSec);
    lastAppendTs = tsSec;
  }
}

// API'den gelen history[]/historyTsSec[] (eskiden yeniye sirali) ile
// cihazin kendi biriktirdigi localHist/localHistTs'i birlestirir. Iki
// dizi de zaman sirali oldugu icin klasik "iki isaretci" birlestirmesi
// yapilir: cakisan (30 sn ici) zaman damgalarinda API degeri tercih
// edilir (localdeki tekrar atlanir), boylece API'nin zaten getirdigi
// veri ASLA kaybolmaz - localHist sadece API'nin KAPSAMADIGI bosluklari
// (ornegin Libre'nin kisa graph penceresinin disini) doldurur.
void mergeLocalHistory() {
  if (localHistCount == 0) return;
  static int mergedH[HIST_MAX + LOCAL_HIST_MAX];
  static uint32_t mergedT[HIST_MAX + LOCAL_HIST_MAX];
  int mc = 0, li = 0, hi = 0;
  uint32_t lastTs = 0;
  while (mc < HIST_MAX + LOCAL_HIST_MAX && (li < localHistCount || hi < historyCount)) {
    bool haveL = (li < localHistCount);
    bool haveH = (hi < historyCount);
    uint32_t tl = haveL ? localHistTs[li] : 0xFFFFFFFFUL;
    uint32_t th = haveH ? historyTsSec[hi] : 0xFFFFFFFFUL;
    uint32_t ts; int val;
    if (th <= tl) {
      ts = th; val = history[hi]; hi++;
      if (haveL && tl <= th + 30) li++; // ayni ani temsil eden local kaydi atla
    } else {
      ts = tl; val = localHist[li]; li++;
    }
    if (mc > 0 && ts <= lastTs + 30) continue; // cok yakin zaman damgasi
    mergedH[mc] = val; mergedT[mc] = ts; mc++;
    lastTs = ts;
  }
  int skip = (mc > HIST_MAX) ? mc - HIST_MAX : 0;     // 3.4.1: en eskiler atilir
  for (int i = skip; i < mc; i++) {
    history[i - skip] = mergedH[i];
    historyTsSec[i - skip] = mergedT[i];
    historyDay[i - skip] = (mergedT[i] > 0) ? dayStampFromMs((uint64_t)mergedT[i] * 1000ULL) : localDayStamp();
  }
  historyCount = mc - skip;
}

// Son 24 saat (bakilan andan geriye) — Dexcom/NS/Libre ortak
void calcLast24hStats(int &minV, int &maxV, int &avgV, int &pct70180, int &pct70140, int &used,
                      uint32_t &tMin, uint32_t &tMax) {
  minV = 999; maxV = 0; avgV = 0;
  pct70180 = 0; pct70140 = 0; used = 0;
  tMin = 0; tMax = 0;
  uint32_t nowSec = (uint32_t)time(nullptr);
  uint32_t cut = (nowSec > 24UL * 3600UL) ? nowSec - 24UL * 3600UL : 0;
  long sum = 0;
  int in70180 = 0, in70140 = 0;
  for (int i = 0; i < historyCount; i++) {
    uint32_t ts = historyTsSec[i];
    if (ts != 0 && ts < cut) continue;
    int v = history[i];
    if (v < 20 || v > 600) continue;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
    sum += v; used++;
    if (v >= 70 && v <= 180) in70180++;
    if (v >= 70 && v <= 140) in70140++;
    if (ts > 0) {
      if (tMin == 0 || ts < tMin) tMin = ts;
      if (ts > tMax) tMax = ts;
    }
  }
  if (used == 0) { minV = 0; return; }
  avgV = (int)(sum / used);
  pct70180 = (in70180 * 100) / used;
  pct70140 = (in70140 * 100) / used;
}

// 3.4.1: istatistigin kapsadigi saat (zaman damgasi varsa gercek sure)
int estimatedHours(int used);
int statsSpanHours(int used, uint32_t tMin, uint32_t tMax) {
  if (tMin > 0 && tMax > tMin) return (int)((tMax - tMin + 1800UL) / 3600UL);
  return used > 0 ? estimatedHours(used) : 0;
}
#define STATS_MIN_HOURS 16
bool statsEnough(int used, uint32_t tMin, uint32_t tMax) {
  return used > 0 && statsSpanHours(used, tMin, tMax) >= STATS_MIN_HOURS;
}

// Elimizdeki olcum sayisindan (used) yaklasik kac saatlik veri oldugunu
// tahmin eder. Kaynaklar tipik olarak ~5 dk araliklarla veri dondugu icin
// 288 olcum ~= 24 saat kabul edilir (Nightscout 200'e dustugunde ~16.6 saat
// -> 16, Libre genelde daha kisa). 270+ olcum tam 24 saat sayilir, asagi
// yuvarlanir (ornegin 200 olcum -> 16 saat, ayni kod tabanindaki 288->200
// dusme yorumundaki "16.6 saat" ifadesiyle tutarli).
int estimatedHours(int used) {
  if (used <= 0) return 24;
  if (used >= 270) return 24;
  int h = (int)((used * 24L) / 288L);
  if (h < 1) h = 1;
  if (h > 24) h = 24;
  return h;
}

// Gercek zaman damgasi olmadigi (tMin/tMax=0) durumlarda Istatistik
// sayfasinin ust basligi icin uzun bicim: "Son 24 saat" / "Yaklasik son N saat".
String estimatedHoursLabel(int used) {
  int h = estimatedHours(used);
  if (used >= 270 || used <= 0) return L("Son 24 saat", "Last 24 hours");
  char buf[40];
  snprintf(buf, sizeof(buf), L("Yaklaşık son %d saat", "Approx. last %d hours"), h);
  return String(buf);
}

// Istatistik sayfasinin alt satirinda ham olcum sayisi ("288 olcum") yerine
// kullanilan kisa bicim: "~24 saat" gibi.
String approxHoursShort(int used) {
  char buf[24];
  snprintf(buf, sizeof(buf), L("~%d saat", "~%dh"), estimatedHours(used));
  return String(buf);
}

String ageText() {
  if (!dataValid || entrySeenAtMillis == 0) return L("Veri yok", "No data");
  unsigned long sec = readingAgeSec;
  char b[40];
  if (sec < 60) { snprintf(b, sizeof(b), L("%lu sn once", "%lus ago"), sec); return String(b); }
  unsigned long min = sec / 60;
  if (min < 60) { snprintf(b, sizeof(b), L("%lu dk once", "%lum ago"), min); return String(b); }
  snprintf(b, sizeof(b), L("%lu sa %lu dk once", "%luh %lum ago"), min / 60, min % 60);
  return String(b);
}

void detectDataSource() {
  nightscoutUrl.trim(); dexUser.trim(); dexPass.trim();
  libreEmail.trim(); librePass.trim();
  bool hasLibre = (libreEmail.length() > 3 && librePass.length() > 0);
  bool hasDex   = (dexUser.length() > 0 && dexPass.length() > 0);
  medEmail.trim(); medPass.trim();
  bool hasMed   = (medEmail.length() > 3 && medPass.length() > 0);
  if (hasMed && !hasLibre && !hasDex) { bool nsOk = (nightscoutUrl.length() > 12 && nightscoutUrl.indexOf('.') > 0);
    if (!nsOk) { dataSource = "medtrum"; return; } }
  bool hasNs    = (nightscoutUrl.length() > 12 && nightscoutUrl != "https://" &&
                   nightscoutUrl != "http://" && nightscoutUrl.indexOf('.') > 0);
  if (hasLibre && !hasDex && !hasNs) dataSource = "libre";
  else if (hasDex && !hasLibre && !hasNs) dataSource = "dexcom";
  else if (hasNs && !hasLibre && !hasDex) dataSource = "nightscout";
  else if (hasLibre) dataSource = "libre";
  else if (hasDex) dataSource = "dexcom";
  else if (hasMed) dataSource = "medtrum";
  else dataSource = "nightscout";
}

// Siddet rengi: 0=kritik/tehlike 1=dusuk/dikkat 2=normal 3=yuksek/dikkat.
// Renk korlugu modu kapaliyken klasik kirmizi/sari/yesil/turuncu, aciksa
// kirmizi-yesil ayrimina dayanmayan mavi/turkuaz/amber/vermilyon paleti
// (Okabe-Ito) kullanilir — esik MANTIGI hicbir zaman degismez, sadece renk.
uint16_t sevColor(int tier) {
  if (colorBlindMode) {
    switch (tier) {
      case 0: return cv->color565(0xD5, 0x5E, 0x00); // vermilyon - kritik/tehlike
      case 1: return cv->color565(0x56, 0xB4, 0xE9); // gok mavisi - dusuk/dikkat
      case 3: return cv->color565(0xE6, 0x9F, 0x00); // amber - yuksek/dikkat
      default: return cv->color565(0x00, 0x9E, 0x73); // mavimsi yesil - normal
    }
  }
  switch (tier) {
    case 0: return TFT_RED;
    case 1: return TFT_YELLOW;
    case 3: return TFT_ORANGE;
    default: return TFT_GREEN;
  }
}

uint16_t getGlucoseColor(int v) {
  if (homeStaleGrey) return 0x8410;                      // 3.3.0: eski veri gri
  // Alarm esiklerine gore: kritik esik ve alti tehlike, dusuk esik alti dikkat, normal,
  // yuksek esik ustu dikkat, cok yuksek esik ve ustu tehlike. (kullanici talebiyle klasik/net
  // renklere geri donuldu — modern/yumusak ton denemesi geri alindi, esik mantigi hep ayniydi.
  // Renk korlugu modu acikken sadece renkler degisir, esikler ayni kalir.)
  if (v < alarmCritLow) return sevColor(0);
  if (v < alarmLow) return sevColor(1);
  if (v <= alarmHigh) return sevColor(2);
  if (v < alarmVeryHigh) return sevColor(3);
  return sevColor(0);
}

String sha256Hex(const String& s) {
  unsigned char hash[32];
  mbedtls_sha256((const unsigned char*)s.c_str(), s.length(), hash, 0);
  char buf[65];
  for (int i = 0; i < 32; i++) sprintf(buf + i * 2, "%02x", hash[i]);
  buf[64] = 0;
  return String(buf);
}

void drawTrendArrow(int cx, int cy, String dir, uint16_t col, int size) {
  dir.toUpperCase();
  // Not: tek/cift yon oklarinin ucgen sekli eskiden cy'ye gore dikeyde
  // simetrik degildi (yukari oklar cy'nin biraz USTUNDE, asagi oklar
  // biraz ALTINDA agirlik merkezine sahipti) - bu da ana ekranda delta
  // yazisiyla ayni satirda gorunmelerini engelliyordu. Asagidaki y
  // degerleri, sekil boyutu degismeden (ayni "size"), sinir kutusu cy'ye
  // gore dikeyde tam simetrik olacak sekilde kaydirildi.
  //
  // Yeni: tekli yukari/asagi ok artik sadece dolu bir ucgen degil, gercek
  // bir ok gibi "ok basi" (ucgen) + "govde" (ince sap) seklinde ciziliyor -
  // kullanicinin "yukari ok bir ucgen gibi gorunuyor" geri bildirimi uzerine.
  // Ciftli ok (hizli yukselis/dusus) da artik ust uste iki ucgen degil,
  // yan yana iki gercek (govdeli) ok olarak ciziliyor. Toplam genislik
  // yine disaridan verilen "size" ile hemen hemen ayni kaliyor ki cagiran
  // ekranlardaki yerlesim/hizalama hesaplari (orn. drawUiSade/drawScreen0
  // grup ortalama mantigi, drawScreen2'deki delta tasma kontrolu) bozulmasin.
  // Capraz ok (FORTYFIVEUP/FORTYFIVEDOWN) govdesi eskiden tek pikselli ince
  // bir cizgiydi; sonra bunu kalinlastirmak icin ust uste kaydirilmis
  // paralel cizgiler denendi ama capraz Bresenham cizgileri birbirine tam
  // oturmadigi icin govde "delikli" gorunuyordu, ustelik ok basi (ucgen)
  // govdenin ucuyla tam hizali degildi ("govdeden icerde kalmis" hissi
  // veriyordu) - kullanici geri bildirimi. Simdi govde de bas da (dikey
  // oklarla ayni mantikla) fillTriangle ile ciziliyor - bosluksuz dolu,
  // ve govdenin genis ucu ok basinin tabanina TAM oturuyor (ikisi de ayni
  // "headBase" noktasindan cikiyor).
  auto drawVArrow = [&](int vcx, int vcy, int vsize, bool up) {
    int halfW = vsize / 2;
    int top = vcy - 2 * vsize / 3;
    int bottom = vcy + 2 * vsize / 3;
    int stemW = max(3, vsize / 4);
    if (up) {
      int headBaseY = top + vsize / 2;
      cv->fillTriangle(vcx, top, vcx - halfW, headBaseY, vcx + halfW, headBaseY, col);
      cv->fillRect(vcx - stemW / 2, headBaseY, stemW, bottom - headBaseY, col);
    } else {
      int headBaseY = bottom - vsize / 2;
      cv->fillTriangle(vcx, bottom, vcx - halfW, headBaseY, vcx + halfW, headBaseY, col);
      cv->fillRect(vcx - stemW / 2, top, stemW, headBaseY - top, col);
    }
  };
  // upRight=true: sol-alt -> sag-ust (yavas yukselis). upRight=false:
  // sol-ust -> sag-alt (yavas dusus). Her ikisi de "uc" (dcx+dsize/2,...)
  // ile "kuyruk" (dcx-dsize/2,...) arasinda, tam ortadaki (dcx,dcy)
  // noktasinda bulusan govde+bas ciziyor.
  auto drawDiagArrow = [&](int dcx, int dcy, int dsize, bool upRight) {
    int headK = max(2, dsize / 3);  // ok basi taban yari-genisligi (kosegen bileseni)
    int stemK = max(1, dsize / 8);  // govde yari-kalinligi (kosegen bileseni)
    int ySign = upRight ? 1 : -1;   // dik yon isareti: (1,1) ya da (1,-1)
    int tipX = dcx + dsize/2, tipY = dcy - ySign*dsize/2;
    int tailX = dcx - dsize/2, tailY = dcy + ySign*dsize/2;
    // Ok basi: uc + govdenin bittigi (dcx,dcy) noktasindaki genis taban
    cv->fillTriangle(tipX, tipY, dcx + headK, dcy + ySign*headK, dcx - headK, dcy - ySign*headK, col);
    // Govde: kuyruktan (dcx,dcy)'ye kadar, dar bir dortgen (2 ucgenle dolu)
    int t1x = tailX + stemK, t1y = tailY + ySign*stemK;
    int t2x = tailX - stemK, t2y = tailY - ySign*stemK;
    int b1x = dcx + stemK,   b1y = dcy + ySign*stemK;
    int b2x = dcx - stemK,   b2y = dcy - ySign*stemK;
    cv->fillTriangle(t1x, t1y, t2x, t2y, b1x, b1y, col);
    cv->fillTriangle(t2x, t2y, b1x, b1y, b2x, b2y, col);
  };

  if (dir.indexOf("DOUBLEUP") >= 0) {
    int subSize = size / 2;
    int gap = max(2, size / 8);
    drawVArrow(cx - subSize / 2 - gap / 2, cy, subSize, true);
    drawVArrow(cx + subSize / 2 + gap / 2, cy, subSize, true);
  } else if (dir.indexOf("SINGLEUP") >= 0 || dir == "UP") {
    drawVArrow(cx, cy, size, true);
  } else if (dir.indexOf("FORTYFIVEUP") >= 0) {
    drawDiagArrow(cx, cy, size, true);
  } else if (dir.indexOf("FLAT") >= 0 || dir.length() < 2) {
    // Yatay (duz) ok eskiden govdesi cok genis tutulmustu (sol kenari
    // cx-size'a kadar, sag kenari cx+size+2'ye kadar uzaniyordu - dikey/
    // capraz oklardan ~2 kat genis bir kutu). Bu yuzden ana ekran, sade
    // duzen ve Grafik ekranindaki delta+ok yerlesim hesaplari (hepsi
    // dikey/capraz oklarin ~size genisligindeki kutusunu varsayiyor) bu
    // durumda deltayla cakismaya cok yaklasiyordu (kullanicinin bildirdigi
    // "yatay ok deltaya cok yakin" sorunu). Simdi dikey oklarla ayni
    // oranda (toplam uzunluk 4*size/3, cx'e gore simetrik) gercek bir
    // ok: govde + sag ucta ucgen bas.
    int rightTip = cx + 2 * size / 3;
    int leftTail = cx - 2 * size / 3;
    int stemW = max(3, size / 4);
    int headBaseX = rightTip - size / 2;
    cv->fillRect(leftTail, cy - stemW / 2, headBaseX - leftTail, stemW, col);
    cv->fillTriangle(rightTip, cy, headBaseX, cy - size / 2, headBaseX, cy + size / 2, col);
  } else if (dir.indexOf("FORTYFIVEDOWN") >= 0) {
    drawDiagArrow(cx, cy, size, false);
  } else if (dir.indexOf("SINGLEDOWN") >= 0 || dir == "DOWN") {
    drawVArrow(cx, cy, size, false);
  } else if (dir.indexOf("DOUBLEDOWN") >= 0) {
    int subSize = size / 2;
    int gap = max(2, size / 8);
    drawVArrow(cx - subSize / 2 - gap / 2, cy, subSize, false);
    drawVArrow(cx + subSize / 2 + gap / 2, cy, subSize, false);
  } else {
    cv->setTextColor(col, UI_BG);
    cv->setTextDatum(MC_DATUM);
    dsN(cv, "?", cx, cy, 4);
  }
}

uint16_t ageBarColor(unsigned long ageSec, bool filled, uint16_t emptyCol) {   // 3.4.7
  unsigned long oldSec = (staleAlarmMin > 0 ? (unsigned long)staleAlarmMin : 20UL) * 60UL;
  if (ageSec >= oldSec) return sevColor(0);
  if (ageSec >= 6UL * 60UL) return sevColor(1);
  return filled ? sevColor(2) : emptyCol;
}

void drawMinuteBars(int y, unsigned long ageSec, bool withAgeText = true) {
  int bars = (int)(ageSec / 60UL);
  if (bars > 5) bars = 5;
  if (bars < 0) bars = 0;
  int barW = 36, gap = 8, barH = 22;
  int startX = (320 - (5 * barW + 4 * gap)) / 2;
  int clearH = withAgeText ? (barH + 22) : (barH + 6);
  cv->fillRect(0, y - 2, 320, clearH, UI_BG);
  for (int i = 0; i < 5; i++) {
    // 5 dakikadan uzun suredir yeni veri gelmediyse tum cubuklar kirmizi
    // (renk dostu modda vermilyon) - normalde her dakika bir cubuk dolar.
    uint16_t col = ageBarColor(ageSec, i < bars, (uint16_t)0x2104);
    cv->fillRoundRect(startX + i * (barW + gap), y, barW, barH, 4, col);
  }
  if (withAgeText) {
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    cv->setTextDatum(MC_DATUM);
    String at = ageText();
    int ty = y + barH + 10;
    if (langIdx == 0) {                                     // 4.0.4: Turkcede "önce" (yerlesik font o harfi cizemiyordu)
      at.replace("once", "önce");
      cv->setFont((const lgfx::IFont*)&FreeSansTR6pt);
      cv->drawString(at.c_str(), 160, ty - 4);
      cv->setTextFont(1);
    } else {
      if (langIdx >= 2 && dsNonAscii(at.c_str())) ty -= 4;   // 3.6.1: Noto 6pt daha uzun - ekranin altindan kesilmesin (Rusca)
      dsN(cv, at, 160, ty, 1);
    }
  }
}

// Libre icin altta "X dk once" goster
void drawAgeFooter(int y) {
  cv->fillRect(0, y - 2, 320, 28, UI_BG);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  cv->setTextDatum(MC_DATUM);
  // Sure: son seker degeri degisiminden bu yana
  String t = ageText();
  if (t == L("Veri yok", "No data")) {
    drawFreeTitle(L("Son ölçüm: -", "Last reading: -"), 160, y + 10, &FreeSansTR7pt);
  } else {
    // ageText() paylasilan bir fonksiyon (font 1 kullanan yerler de var, onlar
    // ASCII kalsin diye ageText() icinde duzeltmiyoruz) - burada sadece bu
    // (font 2, Turkce font'a gecen) ekran icin goruntu duzeltmesi yapiyoruz.
    String td = t;
    if (langIdx == 0) td.replace("once", "önce");
    drawFreeTitle(td.c_str(), 160, y + 10, &FreeSansTR7pt);
  }
}

void updateAge() {
  readingAgeSec = (entrySeenAtMillis == 0) ? 0 : (millis() - entrySeenAtMillis) / 1000UL;
}

// Olcumun gercek yasi (ms): saat (NTP) gecerli ve olcum zamani biliniyorsa; yoksa 0
bool ageNtpAnchored = false;
unsigned long readingAgeFromTs(uint64_t dateMs) {
  time_t nowS = time(nullptr);
  if (dateMs == 0 || nowS < 1600000000) return 0;
  int64_t age = (int64_t)nowS * 1000LL - (int64_t)dateMs;
  if (age < 0) age = 0;                                  // saat farki: gelecekte gorunen olcum
  if (age > 24LL * 3600LL * 1000LL) age = 24LL * 3600LL * 1000LL;
  return (unsigned long)age;
}

bool markNewReading(uint64_t dateMs, const String& dateStr) {
  bool isNew = (dateStr.length() > 5 && dateStr != lastDateString) ||
               (dateMs > 0 && dateMs > lastEntryDateMs);
  if (isNew) {
    lastDateString = dateStr;
    if (dateMs > 0) lastEntryDateMs = dateMs;
    entrySeenAtMillis = millis() - readingAgeFromTs(dateMs);
    if (entrySeenAtMillis == 0) entrySeenAtMillis = 1;
    ageNtpAnchored = (time(nullptr) >= 1600000000);
    updateAge();
    requestAlarmBurst = true; // yeni olcum -> alarm tekrar kontrol
  } else if (!ageNtpAnchored && dateMs > 0 && entrySeenAtMillis != 0 && time(nullptr) >= 1600000000) {
    // acilista saat henuz yoktu: ayni olcum icin yasi saat gelince bir kez duzelt
    entrySeenAtMillis = millis() - readingAgeFromTs(dateMs);
    if (entrySeenAtMillis == 0) entrySeenAtMillis = 1;
    ageNtpAnchored = true;
    updateAge();
  } else if (entrySeenAtMillis == 0) {
    entrySeenAtMillis = millis() - readingAgeFromTs(dateMs);
    if (entrySeenAtMillis == 0) entrySeenAtMillis = 1;
    ageNtpAnchored = (time(nullptr) >= 1600000000);
    lastDateString = dateStr;
    if (dateMs > 0) lastEntryDateMs = dateMs;
  }
  return isNew;
}

void loadSettings() {
  prefs.begin("metematik", true);
  nightscoutUrl = prefs.getString("nsurl", "https://");
  apiSecret = prefs.getString("apisecret", "");
  // Sabit kimlik — prefs yok sayilir
  webUser = WEB_USER;
  dataSource = prefs.getString("source", "");
  dexUser = prefs.getString("dexuser", "");
  dexPass = prefs.getString("dexpass", "");
  dexServer = prefs.getString("dexsrv", "ous");
  libreEmail = prefs.getString("libremail", "");
  librePass = prefs.getString("librepass", "");
  libreRegion = prefs.getString("librereg", "EU");
  medEmail = prefs.getString("medu", "");
  medPass = prefs.getString("medp", "");
  {
    uint32_t pm = prefs.getUInt("pgmask", 0);
    for (int p = 1; p < P_APPS; p++) pgOn[p] = (pm >> p) & 1;
    pgTh2Theme = prefs.getInt("pgth2", 5);
    pgTh3Theme = prefs.getInt("pgth3", 8);
  }
  brightnessLevel = prefs.getInt("bright", 2);
  if (brightnessLevel < 0 || brightnessLevel > 2) brightnessLevel = 2;
  langIdx = prefs.getInt("lang", 0);
  if (langIdx < 0 || langIdx >= LANG_COUNT) langIdx = 0;
  unitIdx = prefs.getInt("unit", 0);
  if (unitIdx < 0 || unitIdx > 1) unitIdx = 0;
  colorBlindMode = prefs.getBool("cblind", false);
  landRot = prefs.getInt("landrot", 1);
  if (landRot != 1 && landRot != 3) landRot = 1;
  uiPageMode = prefs.getInt("uipage", 0);
  if (uiPageMode == 5) uiPageMode = 3;
  else if (uiPageMode == 6) uiPageMode = 4;
  if (uiPageMode == 3) uiPageMode = 0; // Dikey arayuz kaldirildi, Basit'e don
  if (uiPageMode < 0 || uiPageMode > 4) uiPageMode = 0;
  uiPreviewActive = false;
  uiPreviewMode = 0;
  alarmLow = prefs.getInt("alow", 70);
  alarmHigh = prefs.getInt("ahigh", 180);
  alarmVeryHigh = prefs.getInt("avhigh", 250);
  alarmCritLow = prefs.getInt("acrit", 55);
  alarmLowSound = prefs.getBool("alsnd", true);
  alarmHighSound = prefs.getBool("ahsnd", true);
  alarmVeryHighSound = prefs.getBool("avhsnd", true);
  alarmCritLowSound = prefs.getBool("acsnd", true);
  alarmStaleSound = prefs.getBool("astsnd", true);
  alarmFastFallOn = prefs.getBool("afastdn", true);
  alarmFastRiseOn = prefs.getBool("afastup", false);
  recoverySoundSuppressOn = prefs.getBool("rsupp", true);
  sensorTrackOn = prefs.getBool("strack", true);
  cannulaTrackOn = prefs.getBool("ctrack", true);
  sensorLifeHours = prefs.getInt("slifeh", 10 * 24 + 12);
  if (sensorLifeHours < 1) sensorLifeHours = 1;
  if (sensorLifeHours > 30 * 24 + 23) sensorLifeHours = 30 * 24 + 23;
  cannulaLifeHours = prefs.getInt("clifeh", 3 * 24 + 8);
  if (cannulaLifeHours < 1) cannulaLifeHours = 1;
  if (cannulaLifeHours > 14 * 24 + 23) cannulaLifeHours = 14 * 24 + 23;
  staleAlarmMin = prefs.getInt("stale", 15);
  {                                                    // 3.4.14: bildirilmis sure esikleri
    uint8_t m0 = prefs.getUChar("awm0", 0), m1 = prefs.getUChar("awm1", 0);
    for (int i = 0; i < 4; i++) { sensorWarnFired[i] = (m0 >> i) & 1; cannulaWarnFired[i] = (m1 >> i) & 1; }
    lastSensorAgeForWarn = prefs.getFloat("awa0", -1);
    lastCannulaAgeForWarn = prefs.getFloat("awa1", -1);
  }
  if (staleAlarmMin < 5) staleAlarmMin = 5;
  if (staleAlarmMin > 60) staleAlarmMin = 60;
  nightModeOn = prefs.getBool("nighton", false);
  nightStartHour = prefs.getInt("nighth", 22);
  if (nightStartHour < 0 || nightStartHour > 23) nightStartHour = 22;
  nightEndHour = prefs.getInt("nighte", 7);
  if (nightEndHour < 0 || nightEndHour > 23) nightEndHour = 7;
  nightStartMin = prefs.getInt("nightsm", prefs.getBool("nighthh", false) ? 30 : 0);
  nightEndMin = prefs.getInt("nightem", prefs.getBool("nighteh", false) ? 30 : 0);
  nightBrightLevel = prefs.getInt("nightb", 0);
  if (nightBrightLevel < 0 || nightBrightLevel > 2) nightBrightLevel = 0;
  nightSoundLevel = prefs.getInt("nights", 1);
  if (nightSoundLevel < 0 || nightSoundLevel > 3) nightSoundLevel = 1;
  schoolModeOn = prefs.getBool("schoolon", false);
  teacherModeOn = prefs.getBool("teachon", false);         // 4.0.4: varsayilan kapali (Burak)
  schoolStartHour = prefs.getInt("schoolsh", 8);
  if (schoolStartHour < 0 || schoolStartHour > 23) schoolStartHour = 8;
  schoolEndHour = prefs.getInt("schooleh", 15);
  if (schoolEndHour < 0 || schoolEndHour > 23) schoolEndHour = 15;
  schoolStartMin = prefs.getInt("schoolsm", prefs.getBool("schoolshh", false) ? 30 : 0);
  schoolEndMin = prefs.getInt("schoolem", prefs.getBool("schoolehh", false) ? 30 : 0);
  schoolBrightLevel = prefs.getInt("schoolb", 1);
  if (schoolBrightLevel < 0 || schoolBrightLevel > 2) schoolBrightLevel = 1;
  schoolSoundLevel = prefs.getInt("schools", 1);
  if (schoolSoundLevel < 0 || schoolSoundLevel > 3) schoolSoundLevel = 1;
  alarmColorIdx = prefs.getInt("acolor", 0);
  visualAlarmOn = prefs.getBool("visalm", true);
  snoozeMinutes = prefs.getInt("snoozem", 30);
  sleepModeOn = prefs.getBool("sleepon", false);
  micSnapOn = prefs.getBool("micon", true);
  snapSens = prefs.getInt("snapsn", 1);
  if (snapSens < 0 || snapSens > 2) snapSens = 1;
  if (snoozeMinutes != 0 && snoozeMinutes != 15 && snoozeMinutes != 30 && snoozeMinutes != 60)
    snoozeMinutes = 30;
  graphHoursIdx = prefs.getInt("ghours", 1);
  soundLevel = prefs.getInt("sound", 2);
  if (soundLevel < 0 || soundLevel > 3) soundLevel = 2;
  if (graphHoursIdx < 0 || graphHoursIdx >= GRAPH_HOURS_N) graphHoursIdx = 1;
  if (alarmColorIdx < 0 || alarmColorIdx > 5) alarmColorIdx = 0;
  if (alarmLow < 40) alarmLow = 40;
  if (alarmHigh > 400) alarmHigh = 400;
  if (alarmVeryHigh > 400) alarmVeryHigh = 400;
  if (alarmVeryHigh < 200) alarmVeryHigh = 250;
  if (alarmLow >= alarmHigh) { alarmLow = 70; alarmHigh = 180; }
  if (alarmHigh > alarmVeryHigh) alarmHigh = alarmVeryHigh - 10;
  fotoRotateMin = prefs.getInt("fotorot", 5);
  if (fotoRotateMin < 1) fotoRotateMin = 1;
  if (fotoRotateMin > 180) fotoRotateMin = 180;
  wifiCount = prefs.getInt("wcount", 0);
  if (wifiCount > MAX_WIFI) wifiCount = MAX_WIFI;
  for (int i = 0; i < wifiCount; i++) {
    wifiSsid[i] = prefs.getString(("ws" + String(i)).c_str(), "");
    wifiPass[i] = prefs.getString(("wp" + String(i)).c_str(), "");
  }
  homeTheme = prefs.getInt("hometh", 0);
  if (homeTheme < 0 || homeTheme >= THEME_N || themeHidden(homeTheme)) homeTheme = 0;
  if (pgTh2Theme < 0 || pgTh2Theme >= THEME_N || themeHidden(pgTh2Theme)) pgTh2Theme = 5;
  if (pgTh3Theme < 0 || pgTh3Theme >= THEME_N || themeHidden(pgTh3Theme)) pgTh3Theme = 8;
  homeBg = prefs.getInt("homebg", 0);
  if (homeBg < 0 || homeBg >= BG_N) homeBg = 0;
  homeBgDark = prefs.getInt("homebgd", bgIsLight(homeBg) ? 0 : homeBg);
  if (homeBgDark < 0 || homeBgDark >= BG_N || bgIsLight(homeBgDark)) homeBgDark = 0;
  sensInsTs = prefs.getUInt("sensins", 0);
  setInsTs = prefs.getUInt("setins", 0);
  personName = prefs.getString("pname", "");          // 4.0
  {                                                    // 4.0.1: aile modu ac/kapa + kisi sayisi
    bool hasPc = prefs.isKey("pcount");                // 4.0.4: yeni / silinmis cihaz tek kisiyle baslar
    int pc = prefs.getInt("pcount", 2);
    famEnabled = prefs.getBool("famon", hasPc && pc > 1);
    famCount = constrain(pc, 2, PERSON_MAX);
    personCount = famEnabled ? famCount : 1;
  }
  famMode = prefs.getInt("fammode", 0);
  if (famMode < 0 || famMode > 1) famMode = 0;
  prefs.end();
  uiPageMode = themeUiMode(homeTheme);
  // Kaydedilmis gecerli bir kaynak varsa (kullanici Ayarlar/Web'den acikca
  // sectiyse) buna dokunma - eskiden burada sartsiz detectDataSource()
  // cagriliyordu ve birden fazla kaynagin kimlik bilgisi (eski Libre +
  // yeni Nightscout gibi) doluyken kullanicinin sectigi kaynagi sessizce
  // degistirebiliyordu. Sadece ilk kurulumda/gecersiz deger varsa tahmin et.
  if (dataSource != "nightscout" && dataSource != "dexcom" && dataSource != "libre" && dataSource != "medtrum") {
    detectDataSource();
  }
  if (currentScreen >= screenCount()) currentScreen = homeScreenIdx();
  if (currentScreen <= 1) currentScreen = homeScreenIdx();
}

// Dexcom kullanici telefonla girildiyse +90 bicimine getir (e-posta / kullanici adina dokunmaz)
String normDexUser(String s) {
  s.trim();
  String d;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == ' ' || c == '-' || c == '(' || c == ')') continue;
    if (!(isdigit((unsigned char)c) || (c == '+' && d.length() == 0))) return s;   // telefon degil
    d += c;
  }
  if (d.length() == 0) return s;
  if (d[0] == '+') return d;
  if (d.length() == 11 && d.startsWith("05")) return "+90" + d.substring(1);
  if (d.length() == 10 && d[0] == '5') return "+90" + d;
  if (d.length() == 12 && d.startsWith("90")) return "+" + d;
  if (d.length() == 14 && d.startsWith("0090")) return "+" + d.substring(2);
  return d;
}

int sensorLifeDefault(const String& src) {
  if (src == "libre" || src == "medtrum") return 14 * 24;
  return 10 * 24 + 12;                       // Dexcom G7 (10 gun + 12 sa ek sure), Nightscout
}

// 4.0: kisiye ait ayarlar (kaynak, giris, esikler, sensor/set) o kisinin NVS alaninda.
// 1. kisi eski alanda ("metematik", eski anahtarlar) - guncellemede hicbir ayar kaybolmaz.
void savePersonPrefs() {
  Preferences pp;
  pp.begin(personNs(liveCtx), false);
  // Kaynak degistiyse elle girilen sensor / set takma zamanlari silinir (eski kaynagin bilgisi kalmasin)
  {
    String oldSrc = pp.getString("source", "");
    if (oldSrc.length() && oldSrc != dataSource) { sensInsTs = 0; setInsTs = 0; }
    // 3.4.17: sensor omru kaynaga gore (Libre/Medtrum 14 gun, Dexcom 10 gun 12 sa). Elle girilmis
    // (varsayilandan farkli) bir deger varsa dokunulmaz; sonradan elle degistirilebilir.
    if (oldSrc != dataSource && dataSource.length() &&
        (sensorLifeHours == sensorLifeDefault(oldSrc) || sensorLifeHours == 10 * 24 + 12))
      sensorLifeHours = sensorLifeDefault(dataSource);
  }
  dexUser = normDexUser(dexUser);
  pp.putString("nsurl", nightscoutUrl);
  pp.putString("apisecret", apiSecret);
  pp.putString("source", dataSource);
  pp.putString("dexuser", dexUser);
  pp.putString("dexpass", dexPass);
  pp.putString("dexsrv", dexServer);
  pp.putString("libremail", libreEmail);
  pp.putString("librepass", librePass);
  pp.putString("librereg", libreRegion);
  pp.putString("medu", medEmail);
  pp.putString("medp", medPass);
  pp.putInt("alow", alarmLow);
  pp.putInt("ahigh", alarmHigh);
  pp.putInt("avhigh", alarmVeryHigh);
  pp.putInt("acrit", alarmCritLow);
  pp.putBool("strack", sensorTrackOn);
  pp.putBool("ctrack", cannulaTrackOn);
  pp.putInt("slifeh", sensorLifeHours);
  pp.putInt("clifeh", cannulaLifeHours);
  pp.putUInt("sensins", sensInsTs);
  pp.putUInt("setins", setInsTs);
  pp.putString("pname", personName);
  pp.end();
}

// Kisinin ayarlarini globallere oku (1. kisi icin loadSettings ayni anahtarlari okur)
void loadPersonPrefs(int k) {
  Preferences pp;
  pp.begin(personNs(k), true);
  nightscoutUrl = pp.getString("nsurl", "https://");
  apiSecret = pp.getString("apisecret", "");
  dataSource = pp.getString("source", "");
  dexUser = pp.getString("dexuser", "");
  dexPass = pp.getString("dexpass", "");
  dexServer = pp.getString("dexsrv", "ous");
  libreEmail = pp.getString("libremail", "");
  librePass = pp.getString("librepass", "");
  libreRegion = pp.getString("librereg", "EU");
  medEmail = pp.getString("medu", "");
  medPass = pp.getString("medp", "");
  alarmLow = pp.getInt("alow", 70);
  alarmHigh = pp.getInt("ahigh", 180);
  alarmVeryHigh = pp.getInt("avhigh", 250);
  alarmCritLow = pp.getInt("acrit", 55);
  sensorTrackOn = pp.getBool("strack", true);
  cannulaTrackOn = pp.getBool("ctrack", true);
  sensorLifeHours = pp.getInt("slifeh", 10 * 24 + 12);
  cannulaLifeHours = pp.getInt("clifeh", 3 * 24 + 8);
  sensInsTs = pp.getUInt("sensins", 0);
  setInsTs = pp.getUInt("setins", 0);
  personName = pp.getString("pname", "");
  {
    uint8_t m0 = pp.getUChar("awm0", 0), m1 = pp.getUChar("awm1", 0);
    for (int i = 0; i < 4; i++) { sensorWarnFired[i] = (m0 >> i) & 1; cannulaWarnFired[i] = (m1 >> i) & 1; }
    lastSensorAgeForWarn = pp.getFloat("awa0", -1);
    lastCannulaAgeForWarn = pp.getFloat("awa1", -1);
  }
  pp.end();
  if (alarmLow < 40) alarmLow = 40;
  if (alarmHigh > 400) alarmHigh = 400;
  if (alarmVeryHigh > 400) alarmVeryHigh = 400;
  if (alarmVeryHigh < 200) alarmVeryHigh = 250;
  if (alarmLow >= alarmHigh) { alarmLow = 70; alarmHigh = 180; }
  if (alarmHigh > alarmVeryHigh) alarmHigh = alarmVeryHigh - 10;
  if (sensorLifeHours < 1 || sensorLifeHours > 30 * 24 + 23) sensorLifeHours = 10 * 24 + 12;
  if (cannulaLifeHours < 1 || cannulaLifeHours > 14 * 24 + 23) cannulaLifeHours = 3 * 24 + 8;
  if (dataSource != "nightscout" && dataSource != "dexcom" && dataSource != "libre" && dataSource != "medtrum") detectDataSource();
}

void saveSettings() {
  prefs.begin("metematik", false);
  {
    uint32_t pm = 0;
    for (int p = 1; p < P_APPS; p++) if (pgOn[p]) pm |= (1UL << p);
    prefs.putUInt("pgmask", pm);
    prefs.putInt("pgth2", pgTh2Theme);
    prefs.putInt("pgth3", pgTh3Theme);
  }
  prefs.putInt("bright", brightnessLevel);
  prefs.putInt("lang", langIdx);
  prefs.putInt("unit", unitIdx);
  prefs.putBool("cblind", colorBlindMode);
  prefs.putInt("landrot", landRot);
  prefs.putInt("uipage", uiPageMode);
  prefs.putBool("alsnd", alarmLowSound);
  prefs.putBool("ahsnd", alarmHighSound);
  prefs.putBool("avhsnd", alarmVeryHighSound);
  prefs.putBool("acsnd", alarmCritLowSound);
  prefs.putBool("astsnd", alarmStaleSound);
  prefs.putBool("afastdn", alarmFastFallOn);
  prefs.putBool("afastup", alarmFastRiseOn);
  prefs.putBool("rsupp", recoverySoundSuppressOn);
  prefs.putInt("stale", staleAlarmMin);
  prefs.putBool("nighton", nightModeOn);
  prefs.putInt("nighth", nightStartHour);
  prefs.putInt("nighte", nightEndHour);
  prefs.putInt("nightsm", nightStartMin);
  prefs.putInt("nightem", nightEndMin);
  prefs.putInt("nightb", nightBrightLevel);
  prefs.putInt("nights", nightSoundLevel);
  prefs.putBool("schoolon", schoolModeOn);
  prefs.putBool("teachon", teacherModeOn);
  prefs.putInt("schoolsh", schoolStartHour);
  prefs.putInt("schooleh", schoolEndHour);
  prefs.putInt("schoolsm", schoolStartMin);
  prefs.putInt("schoolem", schoolEndMin);
  prefs.putInt("schoolb", schoolBrightLevel);
  prefs.putInt("schools", schoolSoundLevel);
  prefs.putInt("acolor", alarmColorIdx);
  prefs.putBool("visalm", visualAlarmOn);
  prefs.putInt("snoozem", snoozeMinutes);
  prefs.putBool("sleepon", sleepModeOn);
  prefs.putBool("micon", micSnapOn);
  prefs.putInt("snapsn", snapSens);
  prefs.putInt("sound", soundLevel);
  prefs.putInt("ghours", graphHoursIdx);
  prefs.putInt("fotorot", fotoRotateMin);
  prefs.putInt("hometh", homeTheme);
  prefs.putInt("homebg", homeBg);
  prefs.putInt("homebgd", homeBgDark);
  prefs.putInt("pcount", famCount);      // 4.0.1
  prefs.putBool("famon", famEnabled);
  prefs.putInt("fammode", famMode);
  prefs.putInt("wcount", wifiCount);
  for (int i = 0; i < wifiCount; i++) {
    prefs.putString(("ws" + String(i)).c_str(), wifiSsid[i]);
    prefs.putString(("wp" + String(i)).c_str(), wifiPass[i]);
  }
  prefs.end();
  savePersonPrefs();   // 4.0: ekrandaki (canli) kisinin ayarlari kendi alanina
}

void addWifiCredential(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return;
  for (int i = 0; i < wifiCount; i++) {
    if (wifiSsid[i] == ssid) {
      wifiPass[i] = pass;
      String s = wifiSsid[i], p = wifiPass[i];
      for (int j = i; j > 0; j--) {
        wifiSsid[j] = wifiSsid[j - 1];
        wifiPass[j] = wifiPass[j - 1];
      }
      wifiSsid[0] = s; wifiPass[0] = p;
      saveSettings();
      return;
    }
  }
  if (wifiCount < MAX_WIFI) wifiCount++;
  for (int i = wifiCount - 1; i > 0; i--) {
    wifiSsid[i] = wifiSsid[i - 1];
    wifiPass[i] = wifiPass[i - 1];
  }
  wifiSsid[0] = ssid; wifiPass[0] = pass;
  saveSettings();
}

void wifiEventCb(WiFiEvent_t e, WiFiEventInfo_t info) {
  if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    if (r != 8) wifiDiscReason = r;               // 8 = cihaz kendisi kesti (yeniden deneme)
  }
  else if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) wifiDiscReason = 0;
}
void ensureWifiEvents() {
  static bool reg = false;
  if (!reg) { WiFi.onEvent(wifiEventCb); reg = true; }
}
void buildWifiMulti() {
  ensureWifiEvents();
  wifiMulti = WiFiMulti();
  for (int i = 0; i < wifiCount; i++) {
    if (wifiSsid[i].length()) wifiMulti.addAP(wifiSsid[i].c_str(), wifiPass[i].c_str());
  }
}

// ===================== WEB SERVER (ayar + OTA) =====================

// Cihaza ozel web sifresi + hatirlama cerezi (ilk acilista uretilir)
void ensureWebCredentials() {
  static const char* alpha = "abcdefghjkmnpqrstuvwxyz23456789"; // karisan 0/O/1/l/i yok
  prefs.begin("metematik", false);
  webPass = prefs.getString("webpw", "");
  if (webPass.length() < 6) {
    webPass = "";
    for (int i = 0; i < 8; i++) webPass += alpha[esp_random() % 31];
    prefs.putString("webpw", webPass);
  }
  // Etiketteki (fabrika) sifre: kullanici sifresini degistirse de saklanir,
  // Cihaz Sifirla bu sifreye geri dondurur.
  if (prefs.getString("webpw0", "").length() < 6) prefs.putString("webpw0", webPass);
  webToken = prefs.getString("webtok", "");
  if (webToken.length() < 32) {
    char b[33];
    for (int i = 0; i < 16; i++) sprintf(b + i * 2, "%02x", (uint8_t)(esp_random() & 0xFF));
    b[32] = 0;
    webToken = String(b);
    prefs.putString("webtok", webToken);
  }
  prefs.end();
}

// Cihaz Sifirla: acik tarayici oturumlarini gecersiz kil (sifre degismez)
void resetWebToken() {
  prefs.begin("metematik", false);
  prefs.remove("webtok");
  prefs.end();
  ensureWebCredentials();
}

// ---- 2.4.0 WEB GIRISI: kullanici adi yok, sadece cihaz sifresi ----
// Giris sayfasi (/login) sifreyi alir, 30 gun gecerli cerez verir. Tum web
// sayfalari ve /update bu cerezle korunur. 5 hatali denemede 30 sn bekleme.
int loginFails = 0;
unsigned long loginLockUntil = 0;

bool webCookieOk() {
  if (webToken.length() < 32 || !server.hasHeader("Cookie")) return false;
  return server.header("Cookie").indexOf("mmt=" + webToken) >= 0;
}

void sendLoginCookie() {
  server.sendHeader("Set-Cookie", "mmt=" + webToken + "; Max-Age=2592000; Path=/; HttpOnly; SameSite=Lax");
}

bool checkWebAuth() {
  if (webCookieOk()) return true;
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain; charset=utf-8", "Giris gerekli");
  return false;
}

String webFactoryPass() {
  prefs.begin("metematik", true);
  String f = prefs.getString("webpw0", "");
  prefs.end();
  return f.length() >= 6 ? f : webPass;
}

// Cihaz Sifirla: kullanicinin koydugu sifreyi etiketteki sifreye dondur
void restoreFactoryWebPass() {
  String f = webFactoryPass();
  webPass = f;
  prefs.begin("metematik", false);
  prefs.putString("webpw", f);
  prefs.end();
}

static const char WEB_MINI_CSS[] PROGMEM =
  "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<link rel='icon' href='/logo.png'><style>"
  "*{box-sizing:border-box}body{font-family:system-ui,sans-serif;background:#0f1115;color:#e8e8e8;margin:0;padding:16px}"
  ".card{background:#1a1d24;border-radius:12px;padding:18px;max-width:420px;margin:24px auto;box-shadow:0 2px 8px #0006}"
  ".brand{display:flex;align-items:center;gap:14px;margin-bottom:12px}.brand img{width:56px;height:57px}"
  ".brand b{font-size:1.4rem;display:block}.brand span{color:#9a9a9a;font-size:.9rem}"
  "input{width:100%;padding:12px;border-radius:8px;border:1px solid #333;background:#111;color:#eee;font-size:1.1rem;margin:6px 0}"
  ".btn{display:inline-block;margin-top:10px;padding:12px 20px;border:none;border-radius:8px;background:#1a73e8;color:#fff;font-weight:600;font-size:1rem;cursor:pointer;text-decoration:none}"
  ".err{color:#f66}.ok{color:#6f6}.note{color:#999;font-size:.85rem}a{color:#8af}progress{width:100%;height:18px;margin-top:12px}"
  "</style>";

String webBrandHtml() {
  return String("<div class='brand'><img src='/logo.png' alt=''><div><b>MeteMatik</b><span>") +
         deviceName() + "</span></div></div>";
}

// Cihazdaki kurallar: kritik <= dusuk-5, dusuk <= yuksek-10, yuksek <= cok yuksek-10
String thresholdError(int c, int l, int h, int v) {
  if (c < 40 || c > 80) return WT("Kritik dusuk 40-80 arasinda olmali.");
  if (l < 40 || l > 200) return WT("Dusuk 40-200 arasinda olmali.");
  if (h < 100 || h > 400) return WT("Yuksek 100-400 arasinda olmali.");
  if (v < 150 || v > 400) return WT("Cok yuksek 150-400 arasinda olmali.");
  if (c > l - 5) return WT("Kritik dusuk, dusuk esikten en az 5 asagida olmali.");
  if (l > h - 10) return WT("Dusuk esik, yuksek esikten en az 10 asagida olmali.");
  if (h > v - 10) return WT("Yuksek esik, cok yuksek esikten en az 10 asagida olmali.");
  return "";
}

void handleLogin() {
  String msg;
  if (server.method() == HTTP_POST) {
    if ((long)(millis() - loginLockUntil) < 0) {
      msg = WT("<p class='err'>Cok fazla hatali deneme. 30 saniye bekleyin.</p>");
    } else if (webPass.length() >= 6 && server.arg("p") == webPass) {
      loginFails = 0;
      sendLoginCookie();
      server.sendHeader("Location", "/");
      server.send(302, "text/plain; charset=utf-8", "");
      return;
    } else {
      // 3.4.0: bekleme (delay) yok - alarmlar gecikmesin; 5 hatada 30 sn kilit yeterli
      if (++loginFails >= 5) { loginFails = 0; loginLockUntil = millis() + 30000; }
      msg = WT("<p class='err'>Sifre hatali.</p>");
    }
  }
  String h = String(WT("<!DOCTYPE html><html><head><title>MeteMatik giris</title>")) + FPSTR(WEB_MINI_CSS) +
             "</head><body><div class='card'>" + webBrandHtml() +
             WT("<form method='POST' action='/login'>"
             "<input name='p' type='password' placeholder='Sifre' autocomplete='current-password' autofocus>"
             "<button class='btn' type='submit'>Giris</button></form>") + msg +
             WT("<p class='note'>Sifre cihazin Bilgi ekraninda yazar.</p></div></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

// Ayar istekleri baska bir siteden gelmesin (tarayici Origin/Referer basligi)
bool sameOriginQuiet() {
  String host = server.hostHeader();
  String o = server.hasHeader("Origin") ? server.header("Origin") : (server.hasHeader("Referer") ? server.header("Referer") : String(""));
  if (o.length() == 0) return true;
  int p = o.indexOf("://");
  if (p >= 0) o = o.substring(p + 3);
  int s2 = o.indexOf('/');
  if (s2 >= 0) o = o.substring(0, s2);
  return o.equalsIgnoreCase(host);
}

bool sameOriginOk() {
  String host = server.hostHeader();
  String o = server.hasHeader("Origin") ? server.header("Origin") : (server.hasHeader("Referer") ? server.header("Referer") : String(""));
  if (o.length() == 0) return true;                    // basliksiz istek (curl vb.): sifre zaten gerekli
  int p = o.indexOf("://");
  if (p >= 0) o = o.substring(p + 3);
  int s = o.indexOf('/');
  if (s >= 0) o = o.substring(0, s);
  if (o.equalsIgnoreCase(host)) return true;
  server.send(403, "text/plain; charset=utf-8", WT("Baska siteden istek reddedildi"));
  return false;
}

void handlePassChange() {
  if (!checkWebAuth()) return;
  if (!sameOriginOk()) return;
  String cur = server.arg("cur"), np = server.arg("np"), np2 = server.arg("np2");
  String err;
  if (cur != webPass) err = WT("Mevcut sifre hatali.");
  else if (np != np2) err = WT("Yeni sifreler ayni degil.");
  else if (np.length() < 6 || np.length() > 32) err = WT("Yeni sifre 6-32 karakter olmali.");
  else {
    for (unsigned int i = 0; i < np.length(); i++) {
      char c = np[i];
      if (c < 33 || c > 126) { err = WT("Sadece Ingilizce harf, rakam ve isaret kullanin (bosluk ve Turkce harf olmaz)."); break; }
    }
  }
  String body;
  if (err.length()) body = "<p class='err'>" + err + "</p>";
  else {
    webPass = np;
    prefs.begin("metematik", false);
    prefs.putString("webpw", webPass);
    prefs.end();
    resetWebToken();                                // diger tarayicilarin oturumu kapanir
    sendLoginCookie();                              // bu tarayici acik kalsin
    body = WT("<p class='ok'>Sifre degistirildi. Diger cihazlardaki oturumlar kapatildi.</p>");
  }
  String h = String(WT("<!DOCTYPE html><html><head><title>Sifre</title>")) + FPSTR(WEB_MINI_CSS) +
             "</head><body><div class='card'>" + webBrandHtml() + body +
             WT("<p><a href='/'>&larr; Ayarlara don</a></p></div></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

// ---- Yazilim guncelleme (/update) - ElegantOTA yerine, cerezle korumali ----
bool otaAuthOk = false;
bool otaStarted = false;

void handleUpdatePage() {
  if (!checkWebAuth()) return;
  String h = String(WT("<!DOCTYPE html><html><head><title>Guncelleme</title>")) + FPSTR(WEB_MINI_CSS) +
             "</head><body><div class='card'>" + webBrandHtml() +
             WT("<h3>Yazılım güncelleme</h3><p class='note'>Şu anki sürüm: ") + FIRMWARE_VERSION "</p>"
             "<div style='background:#222;border-radius:8px;padding:10px 12px;font-size:.9rem;color:#ccc;line-height:1.5'>" +
             WT("1. Güncelleme dosyasını (<b>.bin</b>) bilgisayarınıza veya telefonunuza indirin.<br>") +
             WT("2. <b>Dosyayı Seçin</b> ile o dosyayı seçin.<br>") +
             WT("3. <b>Yükle</b>'ye basın ve bitmesini bekleyin.</div>") +
             "<input type='file' id='f'>" +
             WT("<button class='btn' id='b'>Yükle</button>") +
             "<progress id='pr' max='100' value='0'></progress><p id='st'></p>" +
             WT("<p class='note'>Yükleme bitene kadar sayfayı kapatmayın. Cihaz sonra kendiliğinden yeniden başlar; ayarlar, fotoğraflar ve video silinmez.</p>") +
             WT("<p><a href='/'>&larr; Ayarlar</a></p></div>") +
             "<script>"
             "var st=document.getElementById('st'),pr=document.getElementById('pr'),fi=document.getElementById('f');"
             "function bad(f){var n=f.name.toLowerCase();" +
             WT("if(n.endsWith('.ino'))return 'Bu kod dosyası (.ino). Cihaza .bin dosyası yüklenir.';") +
             WT("if(!n.endsWith('.bin'))return 'Sadece .bin dosyası yüklenir.';") +
             WT("if(n.indexOf('merged')>=0||n.indexOf('bootloader')>=0||n.indexOf('partitions')>=0)return 'Yanlış dosya: güncelleme dosyasını (.bin) seçin.';") +
             WT("if(f.size>4000000)return 'Dosya çok büyük: güncelleme dosyasını (.bin) seçin.';return '';}") +
             WT("fi.onchange=function(){var f=fi.files[0];if(!f)return;var e=bad(f);st.style.color=e?'#f66':'#8f8';st.textContent=e||('Seçildi: '+f.name+' ('+Math.round(f.size/1024)+' KB)');};") +
             "document.getElementById('b').onclick=function(){" +
             WT("var f=fi.files[0];if(!f){st.textContent='Önce .bin dosyasını seçin';return;}") +
             "var e=bad(f);if(e){st.style.color='#f66';st.textContent=e;return;}"
             "f.slice(0,1).arrayBuffer().then(function(ab){" +
             WT("if(new Uint8Array(ab)[0]!=0xE9){st.style.color='#f66';st.textContent='Bu dosya geçerli bir ESP32 yazılımı değil.';return;}") +
             "st.style.color='';var fd=new FormData();fd.append('update',f,f.name);var x=new XMLHttpRequest();"
             "x.upload.onprogress=function(e){if(e.lengthComputable)pr.value=e.loaded*100/e.total;};"
             "x.onload=function(){st.textContent=x.responseText;};" +
             WT("x.onerror=function(){st.textContent='Bağlantı koptu';};") +
             WT("x.open('POST','/update');x.send(fd);st.textContent='Yükleniyor...';});};") +
             "</script></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaAuthOk = webCookieOk() && sameOriginQuiet();
    otaStarted = false;
    if (!otaAuthOk) { server.client().stop(); return; }
    cv->fillScreen(UI_BG);
    cv->setTextColor(sevColor(1), UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("Güncelleniyor...", "Updating..."), 160, 100, &FreeSansTR9pt);
    otaStarted = Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaAuthOk) { wdtFeed(); serviceAlarmSound(); }
    if (otaAuthOk && otaStarted && Update.write(up.buf, up.currentSize) != up.currentSize) Update.abort();
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaAuthOk && otaStarted) Update.end(true);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (otaStarted) Update.abort();
    if (otaAuthOk) redrawNow();                // 3.4.20: "Guncelleniyor..." ekranda kalmasin
  }
}

void handleUpdateDone() {
  if (!otaAuthOk || !webCookieOk()) { otaAuthOk = false; server.send(403, "text/plain; charset=utf-8", "Giris gerekli"); return; }
  otaAuthOk = false;
  bool ok = otaStarted && Update.isFinished() && !Update.hasError();
  cv->fillScreen(UI_BG);
  cv->setTextColor(ok ? sevColor(2) : sevColor(0), UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(ok ? L("Başarılı!", "Success!") : L("Güncelleme hatası", "Update error"), 160, 110, &FreeSansTR9pt);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain; charset=utf-8",
              ok ? String(WT("Basarili. Cihaz yeniden basliyor...")) : String(WT("Hata: ")) + Update.errorString());
  otaStarted = false;
  if (ok) { delay(1000); ESP.restart(); }
  else { delay(1500); drawCurrentScreen(); }
}

// =====================================================================
//  2.5.0 - WEB: Karb-Not listesi, glukagon videosu, sensor takma zamani
// =====================================================================
void handleKarbPage() {
  if (!checkWebAuth()) return;
  String js;
  {
    DynamicJsonDocument doc(12288);
    JsonArray a = doc.to<JsonArray>();
    for (int i = 0; i < karbCount; i++) {
      JsonObject o = a.createNestedObject();
      o["n"] = karbItems[i].name; o["t"] = karbItems[i].type; o["c"] = karbItems[i].carb; o["u"] = karbItems[i].uses;
    }
    serializeJson(doc, js);
  }
  String h = String("<!DOCTYPE html><html><head><title>Karb-Not</title>") + FPSTR(WEB_MINI_CSS) +
    "<style>.card{max-width:640px}table{width:100%;border-collapse:collapse}td{padding:3px}"
    "td input,td select{margin:0;padding:8px;font-size:.95rem}.del{background:#a33;padding:8px 10px;margin:0}"
    ".srch{background:#333;padding:8px 10px;margin:0}</style></head><body><div class='card'>" + webBrandHtml() +
    WT("<h3>Karbonhidrat Notlarım</h3>") +
    WT("<p class='note'>Her yiyecek için karbonhidrat miktarını seçtiğiniz birime göre girin: 100 gramında / 1 adetinde / 1 porsiyonunda / 1 kasesinde / 1 diliminde / 1 bardağında / 1 paketinde kaç gram karbonhidrat var. ") +
    WT("Cihazda Karb-Not uygulamasından yiyeceği seçip miktarı yazınca karbonhidratı hesaplar; en sık seçilen 5 yiyecek listenin başında durur. Değerleri ürün etiketinden veya güvenilir kaynaktan kontrol edin.</p>") +
    "<table id='t'></table>" +
    WT("<button class='btn btn2' onclick='addRow()'>+ Yiyecek ekle</button> ") +
    WT("<button class='btn' onclick='save()'>Kaydet</button><p id='st'></p>") +
    WT("<p><a href='/'>&larr; Ayarlar</a></p></div>") +
    "<script>var D=" + js + ";" +
    WT("var U=[[0,'100 gramında'],[1,'1 adetinde'],[3,'1 porsiyonunda'],[4,'1 kasesinde'],[5,'1 diliminde'],[6,'1 çay bardağında'],[7,'1 su bardağında'],[8,'1 paketinde']];") +
    "function esc(s){return String(s).replace(/&/g,'&amp;').replace(/\"/g,'&quot;').replace(/</g,'&lt;');}" +
    WT("function draw(){var h='<tr><td>Yiyecek</td><td>Birim</td><td>Karb (g)</td><td></td><td></td></tr>';") +
    "D.forEach(function(o,i){h+='<tr><td><input maxlength=24 value=\"'+esc(o.n)+'\" onchange=\"D['+i+'].n=this.value\"></td>'"
    "+'<td><select onchange=\"D['+i+'].t=+this.value\">'+U.map(function(u){var t=(o.t==2?1:o.t);return '<option value='+u[0]+(t==u[0]?' selected':'')+'>'+u[1]+'</option>';}).join('')+'</select></td>'"
    "+'<td><input type=number step=0.1 min=0 max=1000 value=\"'+o.c+'\" onchange=\"D['+i+'].c=+this.value\" style=\"width:90px\"></td>'" +
    WT("+'<td><button class=\"btn srch\" onclick=\"ara('+i+')\">Ara</button></td>'") +
    WT("+'<td><button class=\"btn del\" onclick=\"D.splice('+i+',1);draw()\">Sil</button></td></tr>';});") +
    "document.getElementById('t').innerHTML=h;}" +
    WT("function addRow(){if(D.length>=60){alert('En fazla 60 yiyecek');return;}D.push({n:'',t:0,c:0,u:0});draw();}") +
    WT("function ara(i){var n=D[i].n||'';window.open('https://www.google.com/search?q='+encodeURIComponent(n+' 100 gram karbonhidrat'),'_blank');}") +
    "function save(){var st=document.getElementById('st');var body='d='+encodeURIComponent(JSON.stringify(D.filter(function(o){return (o.n||'').trim().length;})));"
    "fetch('/karb/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})" +
    WT(".then(function(r){return r.text();}).then(function(t){st.textContent=t;}).catch(function(){st.textContent='Bağlantı hatası';});}") +
    "draw();</script></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void handleKarbSave() {
  if (!checkWebAuth()) return;
  if (!sameOriginOk()) return;
  DynamicJsonDocument doc(16384);
  if (!server.hasArg("d") || deserializeJson(doc, server.arg("d"))) { server.send(400, "text/plain; charset=utf-8", WT("Hata: liste okunamadı")); return; }
  static KarbItem oldItems[KARB_MAX];
  int oldN = karbCount;
  for (int i = 0; i < oldN; i++) oldItems[i] = karbItems[i];
  int n = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (n >= KARB_MAX) break;
    String nm = o["n"] | "";
    nm.replace("<", ""); nm.replace(">", ""); nm.trim();
    if (!nm.length()) continue;
    if (nm.length() > 24) nm = nm.substring(0, 24);
    karbItems[n].name = nm;
    karbItems[n].type = constrain((int)(o["t"] | 0), 0, 8); if (karbItems[n].type == 2) karbItems[n].type = 1;
    karbItems[n].carb = constrain((float)(o["c"] | 0.0f), 0.0f, 1000.0f);
    karbItems[n].uses = (uint16_t)constrain((int)(o["u"] | 0), 0, 60000);
    for (int i = 0; i < oldN; i++)                       // cihazda sonradan yapilan secimleri kaybetme
      if (oldItems[i].name == nm && oldItems[i].uses > karbItems[n].uses) karbItems[n].uses = oldItems[i].uses;
    n++;
  }
  karbCount = n;
  bool ok = karbSaveFile();
  if (currentScreen == SCR_KARB && !formId) { karbView = 0; drawCurrentScreen(); }
  server.send(200, "text/plain; charset=utf-8", ok ? String(WT("Kaydedildi: ") + String(n) + WT(" yiyecek")) : String(WT("Hata: dosya yazılamadı")));
}

// ---- glukagon videosu ----
File vidUp;
bool vidUpOk = false;
size_t vidUpBytes = 0;
String vidUpErr;

void handleVideoPage() {
  if (!checkWebAuth()) return;
  bool has = videoExists();
  size_t freeB = SPIFFS.totalBytes() - SPIFFS.usedBytes();
  size_t cur = 0;
  if (has) { File f = SPIFFS.open(VIDEO_PATH, FILE_READ); if (f) { cur = f.size(); f.close(); } }
  String h = String(WT("<!DOCTYPE html><html><head><title>Glukagon videosu</title>")) + FPSTR(WEB_MINI_CSS) +
    "</head><body><div class='card'>" + webBrandHtml() +
    WT("<h3>Glukagon videosu</h3>") +
    WT("<p class='note'>Telefonunuzdaki videoyu seçin. Tarayıcı videoyu cihazın oynatabileceği küçük karelere (288x216, saniyede 6 kare, sessiz) dönüştürüp yükler. ") +
    WT("Yaklaşık 60-70 saniyeye kadar sığar; daha uzunsa baştan bu kadarı alınır. Cihazda ses yoktur (sadece zil). Dönüştürme telefonda 1-2 dakika sürebilir, sayfayı kapatmayın.</p>") +
    WT("<p>Durum: <b>") + (has ? String(String(WT("Video var (")) + String(cur / 1024) + " KB)") : String(WT("Video yok"))) + WT("</b> &nbsp; Boş alan: ") + String(freeB / 1024) + " KB</p>"
    "<input type='file' id='f' accept='video/*'>" +
    WT("<button class='btn' id='b'>Dönüştür ve yükle</button>") +
    "<progress id='pr' max='100' value='0'></progress><p id='st'></p>"
    "<canvas id='c' width='288' height='216' style='width:100%;max-width:288px;background:#000;border-radius:8px'></canvas>"
    "<video id='v' muted playsinline preload='auto' style='display:none'></video>"
    + (has ? String(WT("<form method='POST' action='/video/del' onsubmit=\"return confirm('Video silinsin mi?')\"><button class='btn btn2'>Videoyu sil</button></form>")) : String("")) +
    WT("<p><a href='/'>&larr; Ayarlar</a></p></div>") +
    "<script>"
    "var W=288,H=216,FPS=6,Q=0.55,MAX=" + String(VIDEO_MAX_BYTES) + ";"
    "var st=document.getElementById('st'),pr=document.getElementById('pr'),c=document.getElementById('c'),v=document.getElementById('v');"
    "function seek(t){return new Promise(function(r){var done=false;v.onseeked=function(){if(!done){done=true;r();}};setTimeout(function(){if(!done){done=true;r();}},3000);v.currentTime=t;});}"
    "function blob(){return new Promise(function(r){c.toBlob(r,'image/jpeg',Q);});}"
    "document.getElementById('b').onclick=async function(){" +
    WT("var file=document.getElementById('f').files[0];if(!file){st.textContent='Önce video seçin';return;}") +
    "this.disabled=true;v.src=URL.createObjectURL(file);" +
    WT("try{await new Promise(function(r,j){v.onloadeddata=r;v.onerror=function(){j('Video açılamadı');};});}catch(e){st.textContent=e;this.disabled=false;return;}") +
    "try{await v.play();v.pause();}catch(e){}"
    "var dur=v.duration,ctx=c.getContext('2d');var sc=Math.min(W/v.videoWidth,H/v.videoHeight),dw=v.videoWidth*sc,dh=v.videoHeight*sc;"
    "var parts=[],total=12,n=0,cut=false;"
    "for(var t=0;t<dur;t+=1/FPS){await seek(t);ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);ctx.drawImage(v,(W-dw)/2,(H-dh)/2,dw,dh);"
    "var bl=await blob();var ab=new Uint8Array(await bl.arrayBuffer());"
    "if(total+4+ab.length>MAX){cut=true;break;}"
    "var hd=new Uint8Array(4);new DataView(hd.buffer).setUint32(0,ab.length,true);parts.push(hd,ab);total+=4+ab.length;n++;" +
    WT("pr.value=t*100/dur;st.textContent='Dönüştürülüyor: '+n+' kare, '+Math.round(total/1024)+' KB';}") +
    "var head=new Uint8Array(12);head.set([77,77,86,49]);var dv=new DataView(head.buffer);dv.setUint16(4,W,true);dv.setUint16(6,H,true);head[8]=FPS;dv.setUint16(10,n,true);"
    "var all=new Blob([head].concat(parts));var fd=new FormData();fd.append('v',all,'glk.mmv');" +
    WT("var x=new XMLHttpRequest();x.upload.onprogress=function(e){if(e.lengthComputable){pr.value=e.loaded*100/e.total;st.textContent='Yükleniyor... '+Math.round(e.loaded/1024)+' / '+Math.round(e.total/1024)+' KB';}};") +
    WT("x.onload=function(){st.textContent=x.responseText+(cut?' (video uzun: ilk '+Math.round(n/FPS)+' sn alındı)':'');};") +
    WT("x.onerror=function(){st.textContent='Bağlantı koptu';};") +
    "x.open('POST','/video?size='+all.size);x.send(fd);};"
    "</script></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void handleVideoUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    vidUpOk = webCookieOk(); vidUpBytes = 0; vidUpErr = "";
    if (!vidUpOk) { vidUpErr = WT("Giriş gerekli"); server.client().stop(); return; }
    if (SPIFFS.exists("/glk.tmp")) SPIFFS.remove("/glk.tmp");
    size_t want = server.hasArg("size") ? (size_t)server.arg("size").toInt() : 0;
    size_t oldSz = 0;
    if (SPIFFS.exists(VIDEO_PATH)) { File of = SPIFFS.open(VIDEO_PATH, FILE_READ); if (of) { oldSz = of.size(); of.close(); } }
    size_t freeNoOld = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    size_t freeB = freeNoOld + oldSz;
    if (want == 0 || want > VIDEO_MAX_BYTES) { vidUpOk = false; vidUpErr = WT("Video çok büyük"); return; }
    if (want + 96 * 1024 > freeB) { vidUpOk = false; vidUpErr = WT("Yer yok (boş ") + String(freeB / 1024) + WT(" KB). Birkaç foto silin."); return; }
    // 3.4.3: yer varsa eski video yeni yukleme bitene kadar korunur
    videoStop();
    if (currentScreen == SCR_VIDEO) goApps();
    if (want + 96 * 1024 > freeNoOld && SPIFFS.exists(VIDEO_PATH)) SPIFFS.remove(VIDEO_PATH);
    vidUp = SPIFFS.open("/glk.tmp", FILE_WRITE);
    if (!vidUp) { vidUpOk = false; vidUpErr = WT("Dosya açılamadı"); }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (vidUpOk) { wdtFeed(); serviceAlarmSound(); }
    if (vidUpOk && vidUp) {
      if (vidUp.write(up.buf, up.currentSize) != up.currentSize) { vidUpOk = false; vidUpErr = WT("Yazma hatası (yer dolu olabilir)"); }
      vidUpBytes += up.currentSize;
      if (vidUpBytes > VIDEO_MAX_BYTES + 1024) { vidUpOk = false; vidUpErr = "Video çok büyük"; }
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (vidUp) vidUp.close();
    if (vidUpOk) {
      if (SPIFFS.exists(VIDEO_PATH)) SPIFFS.remove(VIDEO_PATH);
      if (!SPIFFS.rename("/glk.tmp", VIDEO_PATH)) { vidUpOk = false; vidUpErr = WT("Kaydedilemedi"); }
    }
    else if (SPIFFS.exists("/glk.tmp")) SPIFFS.remove("/glk.tmp");
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (vidUp) vidUp.close();
    if (SPIFFS.exists("/glk.tmp")) SPIFFS.remove("/glk.tmp");
    vidUpOk = false; vidUpErr = WT("Yükleme yarıda kesildi");
  }
}

void handleVideoDone() {
  if (!webCookieOk()) { server.send(403, "text/plain; charset=utf-8", "Giris gerekli"); return; }
  server.send(200, "text/plain; charset=utf-8",
              vidUpOk ? String(WT("Yüklendi (") + String(vidUpBytes / 1024) + WT(" KB). Cihazda: Uygulamalar > Glukagon")) : String(WT("Hata: ") + vidUpErr));
  if (currentScreen == SCR_APPS) drawCurrentScreen();
}

void handleVideoDel() {
  if (!checkWebAuth()) return;
  videoStop();
  if (SPIFFS.exists(VIDEO_PATH)) SPIFFS.remove(VIDEO_PATH);
  server.sendHeader("Location", "/video");
  server.send(302, "text/plain; charset=utf-8", "");
}

// "YYYY-MM-DDTHH:MM" (yerel saat) -> unix sn; bos -> 0
uint32_t parseDateTimeLocal(const String& s) {
  if (s.length() < 16) return 0;
  int y = s.substring(0, 4).toInt(), mo = s.substring(5, 7).toInt(), d = s.substring(8, 10).toInt();
  int hh = s.substring(11, 13).toInt(), mi = s.substring(14, 16).toInt();
  if (y < 2020 || mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mi > 59) return 0;
  return civilToUnixLocal(y, mo, d, hh, mi);
}

String dateTimeLocalValue(uint32_t ts) {
  if (ts < 1600000000UL) return "";
  time_t t = (time_t)ts + 3 * 3600;
  struct tm* tm = gmtime(&t);
  char b[20]; sprintf(b, "%04d-%02d-%02dT%02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
  return String(b);
}


String getChipIdStr() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[20];
  // ust 16 + alt 32 = 12 hex (MAC tabanli benzersiz)
  sprintf(buf, "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(buf);
}

// Her cihaza ozel ad: "metematik-" + chip ID'nin son 4 hanesi (ornek metematik-3F2A).
// Kurulum agi (AP), ag adi (hostname) ve mDNS adresi bununla olusur - ayni
// ortamda birden fazla MeteMatik birbirine karismaz. Ayni ad cihaz etiketine yazilir.
String deviceName() {
  String id = getChipIdStr();
  return String("metematik-") + id.substring(id.length() - 4);
}

// Web formundan gelen "HH:MM" degerini saat + yarim-saat bayragina cevirir.
void parseHM(const String& s, int &hour, int &mins, int defHour) {
  int c = s.indexOf(':');
  if (c < 1) { hour = defHour; mins = 0; return; }
  int h = s.substring(0, c).toInt();
  int m = s.substring(c + 1).toInt();
  if (h < 0 || h > 23) h = defHour;
  if (m < 0 || m > 59) m = 0;
  hour = h;
  mins = m;
}

String htmlEscape(const String& s) {
  String o = s;
  o.replace("&", "&amp;"); o.replace("<", "&lt;"); o.replace(">", "&gt;"); o.replace("\"", "&quot;"); o.replace("'", "&#39;");
  return o;
}

// 4.0: web sayfasinda hangi kisinin ayarlari (?p=0..2)
int webPersonArg() {
  int k = server.hasArg("p") ? server.arg("p").toInt() : 0;
  if (k < 0 || k >= personCount) k = 0;
  return k;
}
// Ust bant: aile modu aciksa hangi kisinin ayarlari duzenleniyor (sekmeler)
String famWebTabs(int pk) {
  if (personCount < 2) return "";
  String h = "<div class='card'><label>" + String(WT("Ayarları düzenlenen kişi")) + "</label><p>";
  for (int k = 0; k < personCount; k++)
    h += "<a class='btn" + String(k == pk ? "" : " btn2") + "' style='margin:4px 6px 0 0' href='/?p=" + String(k) + "'>" + htmlEscape(personLabel(k)) + "</a>";
  h += "</p><p class='badge'>" + String(WT("Aşağıdaki kaynak, eşik ve sensör ayarları bu kişi için: ")) + "<b>" + htmlEscape(personLabel(pk)) + "</b></p></div>";
  return h;
}
// Aile modu karti (sayfanin altinda, Guvenlik kartinin ustune tasinir)
String famWebCard(int pk) {
  String h = "<div class='card' id='famcard'><h1>" + String(WT("Aile modu")) + "</h1>";
  h += "<p style='color:#aaa;font-size:.9rem'>" + String(WT("Aynı cihazda en fazla 3 kişinin şekerini izleyin. Her kişinin kendi kaynağı ve eşikleri olur.")) + "</p>";
  h += "<form method='POST' action='/save'><input type='hidden' name='famform' value='1'>";
  h += "<label>" + String(WT("Durum")) + "</label><select name='famon' onchange=\"document.getElementById('box-fam').classList.toggle('on',this.value=='1')\">";
  h += "<option value='0'" + String(famEnabled ? "" : " selected") + ">" + WT("Kapalı (1 kişi)") + "</option>";
  h += "<option value='1'" + String(famEnabled ? " selected" : "") + ">" + WT("Açık") + "</option></select>";
  h += "<div class='modebox" + String(famEnabled ? " on" : "") + "' id='box-fam'>";
  h += "<div class='row'><div><label>" + String(WT("Kişi sayısı")) + "</label><select name='pcount'>";
  for (int i = 2; i <= PERSON_MAX; i++) h += "<option value='" + String(i) + "'" + (famCount == i ? " selected" : "") + ">" + String(i) + "</option>";
  h += "</select></div><div><label>" + String(WT("Ana ekran")) + "</label><select name='fammode'>";
  h += "<option value='0'" + String(famMode == 0 ? " selected" : "") + ">" + WT("Sırayla (15 sn)") + "</option>";
  h += "<option value='1'" + String(famMode == 1 ? " selected" : "") + ">" + WT("Bölünmüş (hepsi birlikte)") + "</option>";
  h += "</select></div></div>";
  for (int k = 0; k < PERSON_MAX; k++) {
    h += "<label>" + String(k + 1) + ". " + String(WT("kişinin adı")) + "</label><input name='pname" + String(k) + "' maxlength='16' value='" + htmlEscape(personNameOf(k)) + "'>";
  }
  h += "<p style='color:#aaa;font-size:.85rem'>" + String(WT("Kaydettikten sonra sayfanın üstünden kişiyi seçip kaynağını ve eşiklerini girin.")) + "</p>";
  h += "</div><input type='hidden' name='p' value='" + String(pk) + "'>";
  h += "<button class='btn' type='submit'>" + String(WT("Kaydet")) + "</button></form></div>";
  // Guvenlik kartinin ustune tasi; her forma secili kisiyi ekle
  h += "<script>document.addEventListener('DOMContentLoaded',function(){var fc=document.getElementById('famcard'),pf=document.querySelector('form[action=\"/pass\"]');"
       "if(fc&&pf){var sc=pf.closest('.card');sc.parentNode.insertBefore(fc,sc);}"
       "document.querySelectorAll('form[action=\"/save\"]').forEach(function(f){"
       "if(!f.querySelector('input[name=p]')){var i=document.createElement('input');i.type='hidden';i.name='p';i.value='" + String(pk) + "';f.appendChild(i);}});});</script>";
  return h;
}

void handleRoot() {
  if (!checkWebAuth()) return;
  int pk = webPersonArg();             // 4.0
  ctxSwitch(pk);
  String html = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeteMatik</title>
<link rel="icon" href="/logo.png">
<style>
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;background:#0f1115;color:#e8e8e8;margin:0;padding:16px;overflow-x:hidden}
.card{background:#1a1d24;border-radius:12px;padding:16px;margin:0 auto 16px;max-width:480px;box-shadow:0 2px 8px #0006;overflow:hidden}
.brand{display:flex;align-items:center;gap:14px;max-width:480px;margin:0 auto 14px}
.brand img{width:64px;height:65px}
.brand b{font-size:1.5rem;color:#fff;display:block}
.brand span{color:#9a9a9a;font-size:.9rem}
h1{font-size:1.4rem;margin:0 0 8px;color:#4fc3f7}
h2{font-size:1.05rem;margin:16px 0 8px;padding-top:8px;border-top:1px solid #333}
label{display:block;font-size:.85rem;color:#aaa;margin:8px 0 4px}
input,select{width:100%;max-width:100%;min-width:0;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #333;background:#111;color:#eee;font-size:1rem}
input[type=time]{-webkit-appearance:none;appearance:none;padding:10px 8px}
.btn{display:inline-block;margin-top:12px;padding:12px 20px;border:none;border-radius:8px;background:#1a73e8;color:#fff;font-weight:600;cursor:pointer;text-decoration:none}
.btn2{background:#333}
.row{display:flex;gap:8px;flex-wrap:wrap}
.row>*{flex:1 1 120px;min-width:0}
.badge{display:inline-block;padding:4px 10px;border-radius:999px;background:#222;color:#8f8;font-size:.85rem}
.srcbox{display:none}
.srcbox.on{display:block}
.modebox{display:none}
.modebox.on{display:block}
.nsonly{display:none}
.nsonly.on{display:block}
.manonly{display:none}
.manonly.on{display:block}
@media (max-width:400px){body{padding:10px}.card{padding:12px}}
</style></head><body>
<div class="brand"><img src="/logo.png" alt="MeteMatik"><div><b>MeteMatik</b><span>)HTML";
  html += deviceName() + " &middot; v" FIRMWARE_VERSION;
  html += R"HTML(</span></div></div>
)HTML";
  html += famWebTabs(pk);              // 4.0.1: kisi sekmeleri (aile modu aciksa)
  html += famWebCard(pk);              // 4.0: aile modu karti (JS ile Guvenlik kartinin ustune tasinir)
  html += R"HTML(<div class="card">
<p>IP: )HTML";
  html += WiFi.localIP().toString();
  html += WT(" &nbsp;|&nbsp; Kaynak: <b>") + sourceLabel() + "</b></p>";
  html += "<p>WiFi: " + htmlEscape(WiFi.SSID()) + "</p>";
  html += WT(R"HTML(
<p><a class="btn" href="/update">Firmware Guncelle (OTA)</a>
<a class="btn btn2" href="/foto">Foto yukle</a></p>
<p><a class="btn btn2" href="/karb">Karb-Not listesi</a></p>
</div>
<div class="card">
<h1>Kaynak Ayarlari</h1>
<p style="color:#aaa;font-size:.9rem">Once kullandiginiz kaynagi secin, sonra sadece o alanlari doldurun.</p>
<form method="POST" action="/save">
<label>Aktif kaynak</label>
<select name="srcpick" id="srcpick" onchange="showSrc(this.value)">
<option value="nightscout")HTML");
  if (dataSource == "nightscout") html += " selected";
  html += R"HTML(>Nightscout</option>
<option value="dexcom")HTML";
  if (dataSource == "dexcom") html += " selected";
  html += R"HTML(>Dexcom</option>
<option value="libre")HTML";
  if (dataSource == "libre") html += " selected";
  html += R"HTML(>LibreLinkUp</option>
<option value="medtrum")HTML";
  if (dataSource == "medtrum") html += " selected";
  html += R"HTML(>Medtrum (EasyFollow)</option>
</select>

<div class="srcbox)HTML";
  html += (dataSource == "nightscout") ? " on" : "";
  html += R"HTML(" id="box-nightscout">
<h2 style="color:#4a90d9">Nightscout</h2>
<label>URL</label><input name="nsurl" value=")HTML";
  html += htmlEscape(nightscoutUrl);
  html += WT(R"HTML(">
<label>API Secret (opsiyonel; yildizlar = kayitli)</label><input name="apisecret" type="password" autocomplete="off" value=")HTML");
  html += pwMask(apiSecret);
  html += R"HTML(">
</div>

<div class="srcbox)HTML";
  html += (dataSource == "dexcom") ? " on" : "";
  html += WT(R"HTML(" id="box-dexcom">
<h2 style="color:#e67e22">Dexcom</h2>
<p style="color:#aaa;font-size:.85rem">Dexcom G6 / G7 / ONE uygulamasina giris yaptiginiz <b>kendi hesabiniz</b> (Follow uygulamasinin hesabi degil). Uygulamada Paylasim (Share) acik ve en az bir takipci ekli olmali (takipcinin uygulamayi kullanmasi gerekmez).</p>
<label>Kullanici adi, e-posta veya telefon</label>
<p style="color:#aaa;font-size:.8rem;margin:2px 0 6px">Dexcom hesabiniz telefon numarasiyla kayitliysa numarayi ulke koduyla, bosluksuz yazin: +905XXXXXXXXX</p><input name="dexuser" value=")HTML");
  html += htmlEscape(dexUser);
  html += WT(R"HTML(">
<label>Sifre (yildizlar = kayitli sifre; silmek icin bosaltin)</label><input name="dexpass" type="password" value=")HTML");
  html += pwMask(dexPass);
  html += WT(R"HTML(" autocomplete="off">
<label>Sunucu</label><select name="dexsrv"><option value="ous")HTML");
  if (dexServer != "us") html += " selected";
  html += WT(R"HTML(>ous (EU vb.)</option><option value="us")HTML");
  if (dexServer == "us") html += " selected";
  html += R"HTML(>us</option></select>
</div>

<div class="srcbox)HTML";
  html += (dataSource == "libre") ? " on" : "";
  html += WT(R"HTML(" id="box-libre">
<h2 style="color:#27ae60">LibreLinkUp</h2>
<p style="color:#aaa;font-size:.85rem">LibreLinkUp <b>takipci hesabinin</b> e-posta ve sifresi (LibreLink / Libre uygulamasindan bu hesaba davet gonderilip LibreLinkUp uygulamasinda kabul edilmis olmali).</p>
<label>E-posta</label><input name="libremail" value=")HTML");
  html += htmlEscape(libreEmail);
  html += WT(R"HTML(">
<label>Sifre (yildizlar = kayitli sifre; silmek icin bosaltin)</label><input name="librepass" type="password" value=")HTML");
  html += pwMask(librePass);
  html += WT(R"HTML(" autocomplete="off">
<label>Bolge</label><input name="librereg" value=")HTML");
  html += htmlEscape(libreRegion);
  html += R"HTML(">
</div>

<div class="srcbox)HTML";
  html += (dataSource == "medtrum") ? " on" : "";
  html += WT(R"HTML(" id="box-medtrum">
<h2 style="color:#9b59b6">Medtrum</h2>
<p style="color:#aaa;font-size:.85rem">EasyView / Medtrum uygulamasindan (Takip / Paylas) davet ettiginiz <b>EasyFollow hesabinin</b> e-posta ve sifresi. Sunucu: Avrupa (easyview.medtrum.eu).</p>
<label>EasyFollow e-posta</label><input name="medemail" value=")HTML");
  html += htmlEscape(medEmail);
  html += WT(R"HTML(">
<label>Sifre (yildizlar = kayitli sifre; silmek icin bosaltin)</label><input name="medpass" type="password" value=")HTML");
  html += pwMask(medPass);
  html += WT(R"HTML(" autocomplete="off">
</div>

<button class="btn" type="submit">Kaydet</button>
</form>
</div>
<script>
function showSrc(v){
  document.querySelectorAll('.srcbox').forEach(function(el){
    el.classList.toggle('on', el.id === 'box-' + v);
  });
  document.querySelectorAll('.nsonly').forEach(function(el){
    el.classList.toggle('on', v === 'nightscout');
  });
  document.querySelectorAll('.manonly').forEach(function(el){
    el.classList.toggle('on', v !== 'nightscout');
  });
}
function toggleBox(id, v){
  document.getElementById(id).classList.toggle('on', v === '1');
}
</script>)HTML");
  html += WT(R"HTML(
<div class="card">
<h1>Sensor / Set Suresi</h1>
<p style="color:#aaa;font-size:.9rem">Kalan sure = omur - gecen sure. Nightscout'ta gecen sure otomatik gelir. Dexcom / Libre kullaniyorsaniz takma zamanini asagiya (veya cihazda Uygulamalar &gt; Sensor'e) girin. Sure dolmaya 12 saat / 3 saat / 1 saat / 15 dakika kala cihazda kisa bir uyari cikar. Pompa kullanmiyorsaniz Set/Pompa takibini kapatabilirsiniz.</p>
<form method="POST" action="/save">
<div class="manonly)HTML");
  html += (dataSource != "nightscout") ? " on" : "";
  html += WT(R"HTML(">
<label>Sensor takilma zamani</label><input name="sensins" type="datetime-local" value=")HTML");
  html += dateTimeLocalValue(sensInsTs);
  html += WT(R"HTML(">
<label>Set / pompa degisim zamani</label><input name="setins" type="datetime-local" value=")HTML");
  html += dateTimeLocalValue(setInsTs);
  html += WT(R"HTML(">
</div>
<label>Sensor takibi</label><select name="strackon" onchange="toggleBox('box-strack',this.value)"><option value="1")HTML");
  if (sensorTrackOn) html += " selected";
  html += WT(R"HTML(>Acik</option><option value="0")HTML");
  if (!sensorTrackOn) html += " selected";
  html += WT(R"HTML(>Kapali</option></select>
<div class="modebox)HTML");
  html += sensorTrackOn ? " on" : "";
  html += WT(R"HTML(" id="box-strack">
<div class="row">
<div><label>Sensor omru - gun</label><input name="slifed" type="number" min="0" max="30" value=")HTML");
  html += String(sensorLifeHours / 24);
  html += WT(R"HTML("></div>
<div><label>saat (0-23)</label><input name="slifeh" type="number" min="0" max="23" value=")HTML");
  html += String(sensorLifeHours % 24);
  html += WT(R"HTML("></div>
</div>
</div>
<label>Set / Pompa takibi</label><select name="ctrackon" onchange="toggleBox('box-ctrack',this.value)"><option value="1")HTML");
  if (cannulaTrackOn) html += " selected";
  html += WT(R"HTML(>Acik</option><option value="0")HTML");
  if (!cannulaTrackOn) html += " selected";
  html += WT(R"HTML(>Kapali</option></select>
<div class="modebox)HTML");
  html += cannulaTrackOn ? " on" : "";
  html += WT(R"HTML(" id="box-ctrack">
<div class="row">
<div><label>Set/Pompa omru - gun</label><input name="clifed" type="number" min="0" max="14" value=")HTML");
  html += String(cannulaLifeHours / 24);
  html += WT(R"HTML("></div>
<div><label>saat (0-23)</label><input name="clifeh" type="number" min="0" max="23" value=")HTML");
  html += String(cannulaLifeHours % 24);
  html += WT(R"HTML("></div>
</div>
</div>
<button class="btn" type="submit">Sensor/Set kaydet</button>
</form>
</div>
<div class="card">
<h1>Alarm esikleri</h1>
<script>
function chkTh(f){var c=+f.acrit.value,l=+f.alow.value,h=+f.ahigh.value,v=+f.avhigh.value,m='';
if(c>l-5)m='Kritik dusuk, dusuk esikten en az 5 asagida olmali.';
else if(l>h-10)m='Dusuk esik, yuksek esikten en az 10 asagida olmali.';
else if(h>v-10)m='Yuksek esik, cok yuksek esikten en az 10 asagida olmali.';
if(m){alert(m);return false;}return true;}
</script>
<form method="POST" action="/save" onsubmit="return chkTh(this)">
<label>Kritik dusuk</label><input name="acrit" type="number" min="40" max="80" value=")HTML");
  html += String(alarmCritLow);
  html += WT(R"HTML(">
<label>Dusuk</label><input name="alow" type="number" min="40" max="200" value=")HTML");
  html += String(alarmLow);
  html += WT(R"HTML(">
<label>Yuksek</label><input name="ahigh" type="number" min="100" max="400" value=")HTML");
  html += String(alarmHigh);
  html += WT(R"HTML(">
<label>Cok yuksek</label><input name="avhigh" type="number" min="150" max="400" value=")HTML");
  html += String(alarmVeryHigh);
  html += WT(R"HTML(">
<label>Veri yok (dk)</label><input name="stale" type="number" min="5" max="60" value=")HTML");
  html += String(staleAlarmMin);
  html += WT(R"HTML(">
<label>Hizli dusus alarmi (cift asagi ok)</label><select name="afdn"><option value="1")HTML");
  if (alarmFastFallOn) html += " selected";
  html += WT(R"HTML(>Acik</option><option value="0")HTML");
  if (!alarmFastFallOn) html += " selected";
  html += WT(R"HTML(>Kapali</option></select>
<label>Hizli yukselis alarmi (cift yukari ok)</label><select name="afup"><option value="1")HTML");
  if (alarmFastRiseOn) html += " selected";
  html += WT(R"HTML(>Acik</option><option value="0")HTML");
  if (!alarmFastRiseOn) html += " selected";
  html += WT(R"HTML(>Kapali</option></select>
<label>Erteleme dk (0=yok)</label><input name="snooze" type="number" min="0" max="60" value=")HTML");
  html += String(snoozeMinutes);
  html += WT(R"HTML(">
<label>Gorsel alarm</label><select name="visalm"><option value="1")HTML");
  if (visualAlarmOn) html += " selected";
  html += WT(R"HTML(>Acik</option><option value="0")HTML");
  if (!visualAlarmOn) html += " selected";
  html += WT(R"HTML(>Kapali</option></select>
<button class="btn" type="submit">Alarm kaydet</button>
</form>
</div>
<div class="card">
<h1>Cihaz</h1>
<form method="POST" action="/save">
<label>Parlaklik</label><select name="bright">)HTML");
  html += String("<option value=\"0\"") + (brightnessLevel==0?" selected":"") + WT(">Dusuk</option>");
  html += String("<option value=\"1\"") + (brightnessLevel==1?" selected":"") + WT(">Orta</option>");
  html += String("<option value=\"2\"") + (brightnessLevel==2?" selected":"") + WT(">Yuksek</option>");
  html += WT(R"HTML(</select>
<label>Alarm sesi</label><select name="sound">)HTML");
  html += String("<option value=\"0\"") + (soundLevel==0?" selected":"") + WT(">Sessiz</option>");
  html += String("<option value=\"1\"") + (soundLevel==1?" selected":"") + WT(">Dusuk</option>");
  html += String("<option value=\"2\"") + (soundLevel==2?" selected":"") + WT(">Orta</option>");
  html += String("<option value=\"3\"") + (soundLevel==3?" selected":"") + WT(">Yuksek</option>");
  html += WT(R"HTML(</select>
<button class="btn" type="submit">Cihaz kaydet</button>
</form>
</div>
<div class="card">
<h1>Gece modu</h1>
<form method="POST" action="/save">
<label>Durum</label><select name="nighton" onchange="toggleBox('box-night',this.value)"><option value="0")HTML");
  if (!nightModeOn) html += " selected";
  html += WT(R"HTML(>Kapali</option><option value="1")HTML");
  if (nightModeOn) html += " selected";
  html += WT(R"HTML(>Acik</option></select>
<div class="modebox)HTML");
  html += nightModeOn ? " on" : "";
  html += WT(R"HTML(" id="box-night">
<label>Baslangic</label><input name="nighth" type="time" step="60" value=")HTML");
  { char tb[8]; sprintf(tb, "%02d:%02d", nightStartHour, nightStartMin); html += String(tb); }
  html += WT(R"HTML(">
<label>Bitis</label><input name="nighte" type="time" step="60" value=")HTML");
  { char tb[8]; sprintf(tb, "%02d:%02d", nightEndHour, nightEndMin); html += String(tb); }
  html += WT(R"HTML(">
<label>Gece parlaklik</label><select name="nightb">)HTML");
  html += String("<option value=\"0\"") + (nightBrightLevel==0?" selected":"") + WT(">Dusuk</option>");
  html += String("<option value=\"1\"") + (nightBrightLevel==1?" selected":"") + WT(">Orta</option>");
  html += String("<option value=\"2\"") + (nightBrightLevel==2?" selected":"") + WT(">Yuksek</option>");
  html += WT(R"HTML(</select>
<label>Gece ses</label><select name="nights">)HTML");
  html += String("<option value=\"0\"") + (nightSoundLevel==0?" selected":"") + WT(">Sessiz</option>");
  html += String("<option value=\"1\"") + (nightSoundLevel==1?" selected":"") + WT(">Dusuk</option>");
  html += String("<option value=\"2\"") + (nightSoundLevel==2?" selected":"") + WT(">Orta</option>");
  html += String("<option value=\"3\"") + (nightSoundLevel==3?" selected":"") + WT(">Yuksek</option>");
  html += WT(R"HTML(</select>
</div>
<button class="btn" type="submit">Gece kaydet</button>
</form>
</div>
<div class="card">
<h1>Okul modu</h1>
<form method="POST" action="/save">
<label>Durum</label><select name="schoolon" onchange="toggleBox('box-school',this.value)"><option value="0")HTML");
  if (!schoolModeOn) html += " selected";
  html += WT(R"HTML(>Kapali</option><option value="1")HTML");
  if (schoolModeOn) html += " selected";
  html += WT(R"HTML(>Acik</option></select>
<div class="modebox)HTML");
  html += schoolModeOn ? " on" : "";
  html += WT(R"HTML(" id="box-school">
<label>Baslangic</label><input name="schoolsh" type="time" step="60" value=")HTML");
  { char tb[8]; sprintf(tb, "%02d:%02d", schoolStartHour, schoolStartMin); html += String(tb); }
  html += WT(R"HTML(">
<label>Bitis</label><input name="schooleh" type="time" step="60" value=")HTML");
  { char tb[8]; sprintf(tb, "%02d:%02d", schoolEndHour, schoolEndMin); html += String(tb); }
  html += WT(R"HTML(">
<label>Okul parlaklik</label><select name="schoolb">)HTML");
  html += String("<option value=\"0\"") + (schoolBrightLevel==0?" selected":"") + WT(">Dusuk</option>");
  html += String("<option value=\"1\"") + (schoolBrightLevel==1?" selected":"") + WT(">Orta</option>");
  html += String("<option value=\"2\"") + (schoolBrightLevel==2?" selected":"") + WT(">Yuksek</option>");
  html += WT(R"HTML(</select>
<label>Okul ses</label><select name="schools">)HTML");
  html += String("<option value=\"0\"") + (schoolSoundLevel==0?" selected":"") + WT(">Sessiz</option>");
  html += String("<option value=\"1\"") + (schoolSoundLevel==1?" selected":"") + WT(">Dusuk</option>");
  html += String("<option value=\"2\"") + (schoolSoundLevel==2?" selected":"") + WT(">Orta</option>");
  html += String("<option value=\"3\"") + (schoolSoundLevel==3?" selected":"") + WT(">Yuksek</option>");
  html += WT(R"HTML(</select>
</div>
<button class="btn" type="submit">Okul kaydet</button>
</form>
</div>
<div class="card">
<h1>Guvenlik</h1>
<p style="color:#aaa;font-size:.9rem">Bu tarayici 30 gun boyunca hatirlanir. Sifre cihazin Bilgi ekraninda da yazar; unutursan oradan bakabilirsin. Cihaz Sifirla, sifreyi ilk (fabrika) sifresine dondurur; yeni sifre yine Bilgi ekraninda yazar.</p>
<form method="POST" action="/pass">
<label>Mevcut sifre</label><input name="cur" type="password" autocomplete="current-password">
<label>Yeni sifre (6-32 karakter, Turkce harf ve bosluk yok)</label><input name="np" type="password" autocomplete="new-password">
<label>Yeni sifre (tekrar)</label><input name="np2" type="password" autocomplete="new-password">
<button class="btn" type="submit">Sifreyi degistir</button>
</form>
<p><a class="btn btn2" href="/logout">Cikis yap</a></p>
</div>
</body></html>)HTML");
  ctxSwitch(dispCtx);                  // 4.0
  server.send(200, "text/html; charset=utf-8", html);
}

void clearSourceData() {
  dataValid = false; sgv = 0; delta = 0; deltaInterp = false; deltaUnknown = false; direction = "Flat"; historyCount = 0;
  entrySeenAtMillis = 0; lastEntryDateMs = 0; lastDateString = ""; readingAgeSec = 0;
  localHistCount = 0; lastLocalAppendTs = 0; lastLibreSgv = -1; libreRingReset = true;
  snoozeActive = false; alarmSnoozeUntil = 0; libBurstReset = true;      // 3.4.20
  lhDirty = false; if (SPIFFS.begin(true)) SPIFFS.remove(LH_PATH);      // 3.4.15
  nsIob = -1; nsCob = -1; bolusCount = 0; sensorAgeHours = -1; cannulaAgeHours = -1; lastNsExtraFetch = 0;
  medHistOk = false;
}
// web / kurulum alanlari: bas-son bosluk ve sayfayi bozabilecek karakterler atilir
void cleanField(String& s) {
  s.trim();
  s.replace("'", ""); s.replace("\"", ""); s.replace("<", ""); s.replace(">", "");
}

void handleSave() {
  if (!checkWebAuth()) return;
  if (!sameOriginOk()) return;
  int pk = webPersonArg();             // 4.0: hangi kisinin ayarlari
  ctxSwitch(pk);
  if (server.hasArg("famform")) {      // 4.0: aile modu karti
    famEnabled = (server.arg("famon") == "1");
    if (server.hasArg("pcount")) famCount = constrain(server.arg("pcount").toInt(), 2, PERSON_MAX);
    personCount = famEnabled ? famCount : 1;
    if (server.hasArg("fammode")) famMode = constrain(server.arg("fammode").toInt(), 0, 1);
    saveSettings();                    // cihaz ayarlari + canli kisi
    for (int k = 0; k < PERSON_MAX; k++) {   // kisilerin adlari
      String an = "pname" + String(k);
      if (!server.hasArg(an)) continue;
      String nm = server.arg(an); cleanField(nm);
      if (nm.length() > 16) nm = nm.substring(0, 16);
      ctxSwitch(k);
      if (nm != personName) { personName = nm; savePersonPrefs(); }
    }
    if (pk >= personCount) pk = 0;
    if (dispCtx >= personCount) dispCtx = 0;
    ctxSwitch(dispCtx);
    famLastRot = millis();
    if (isHomeScreen() && autoRefreshOk()) drawCurrentScreen();
    requestFetch();
    server.sendHeader("Location", "/?p=" + String(pk));
    server.send(303);
    return;
  }
  // 3.4.1: kaynak / hesap ayarlari degisti mi (kayittan sonra karsilastirilir)
  String o_src = dataSource, o_ns = nightscoutUrl, o_sec = apiSecret, o_du = dexUser, o_dp = dexPass, o_ds = dexServer;
  String o_le = libreEmail, o_lp = librePass, o_lr = libreRegion, o_me = medEmail, o_mp = medPass;
  // 3.4.0: esikler birbirine gore denetlenir; uyumsuzsa hicbir sey kaydedilmez
  if (server.hasArg("acrit") || server.hasArg("alow") || server.hasArg("ahigh") || server.hasArg("avhigh")) {
    int c = server.hasArg("acrit") ? server.arg("acrit").toInt() : alarmCritLow;
    int l = server.hasArg("alow") ? server.arg("alow").toInt() : alarmLow;
    int h = server.hasArg("ahigh") ? server.arg("ahigh").toInt() : alarmHigh;
    int v = server.hasArg("avhigh") ? server.arg("avhigh").toInt() : alarmVeryHigh;
    String err = thresholdError(c, l, h, v);
    if (err.length()) {
      String pg = String(WT("<!DOCTYPE html><html><head><title>Esikler</title>")) + FPSTR(WEB_MINI_CSS) +
                  "</head><body><div class='card'>" + webBrandHtml() + "<p class='err'>" + err +
                  WT("</p><p>Hicbir ayar degistirilmedi.</p><p><a href='javascript:history.back()'>&larr; Geri don</a></p></div></body></html>");
      ctxSwitch(dispCtx);
      server.send(400, "text/html; charset=utf-8", pg);
      return;
    }
  }
  if (server.hasArg("nsurl")) nightscoutUrl = server.arg("nsurl");
  if (server.hasArg("apisecret")) applyPw(server.arg("apisecret"), apiSecret);
  if (server.hasArg("dexuser")) dexUser = server.arg("dexuser");
  if (server.hasArg("dexpass")) applyPw(server.arg("dexpass"), dexPass);
  if (server.hasArg("dexsrv")) {
    dexServer = server.arg("dexsrv");
    dexServer.toLowerCase();
    if (dexServer != "us") dexServer = "ous";
  }
  if (server.hasArg("libremail")) libreEmail = server.arg("libremail");
  if (server.hasArg("medemail")) medEmail = server.arg("medemail");
  if (server.hasArg("medpass")) applyPw(server.arg("medpass"), medPass);
  if (server.hasArg("librepass")) applyPw(server.arg("librepass"), librePass);
  if (server.hasArg("librereg")) {
    libreRegion = server.arg("librereg");
    libreRegion.toUpperCase();
  }
  cleanField(nightscoutUrl); cleanField(dexUser); cleanField(libreEmail); cleanField(medEmail);
  if (server.hasArg("acrit")) alarmCritLow = constrain(server.arg("acrit").toInt(), 40, 80);
  if (server.hasArg("alow")) alarmLow = constrain(server.arg("alow").toInt(), 40, 200);
  if (server.hasArg("ahigh")) alarmHigh = constrain(server.arg("ahigh").toInt(), 100, 400);
  if (server.hasArg("avhigh")) alarmVeryHigh = constrain(server.arg("avhigh").toInt(), 150, 400);
  if (server.hasArg("stale")) staleAlarmMin = constrain(server.arg("stale").toInt(), 5, 60);
  if (server.hasArg("strackon")) sensorTrackOn = (server.arg("strackon") == "1");
  if (server.hasArg("ctrackon")) cannulaTrackOn = (server.arg("ctrackon") == "1");
  if (server.hasArg("slifed") || server.hasArg("slifeh")) {
    int d = server.hasArg("slifed") ? constrain(server.arg("slifed").toInt(), 0, 30) : (sensorLifeHours / 24);
    int h = server.hasArg("slifeh") ? constrain(server.arg("slifeh").toInt(), 0, 23) : (sensorLifeHours % 24);
    sensorLifeHours = d * 24 + h;
    if (sensorLifeHours < 1) sensorLifeHours = 1;
  }
  if (server.hasArg("clifed") || server.hasArg("clifeh")) {
    int d = server.hasArg("clifed") ? constrain(server.arg("clifed").toInt(), 0, 14) : (cannulaLifeHours / 24);
    int h = server.hasArg("clifeh") ? constrain(server.arg("clifeh").toInt(), 0, 23) : (cannulaLifeHours % 24);
    cannulaLifeHours = d * 24 + h;
    if (cannulaLifeHours < 1) cannulaLifeHours = 1;
  }
  if (server.hasArg("snooze")) {
    int s = server.arg("snooze").toInt();
    if (s == 0 || s == 15 || s == 30 || s == 60) snoozeMinutes = s;
  }
  if (server.hasArg("visalm")) visualAlarmOn = (server.arg("visalm") == "1");
  if (server.hasArg("afdn")) alarmFastFallOn = (server.arg("afdn") == "1");
  if (server.hasArg("afup")) alarmFastRiseOn = (server.arg("afup") == "1");
  if (server.hasArg("bright")) {
    brightnessLevel = constrain(server.arg("bright").toInt(), 0, 2);
    applyBrightness();
  }
  if (server.hasArg("sound")) soundLevel = constrain(server.arg("sound").toInt(), 0, 3);
  if (server.hasArg("nighton")) nightModeOn = (server.arg("nighton") == "1");
  if (server.hasArg("nighth")) parseHM(server.arg("nighth"), nightStartHour, nightStartMin, nightStartHour);
  if (server.hasArg("nighte")) parseHM(server.arg("nighte"), nightEndHour, nightEndMin, nightEndHour);
  if (server.hasArg("nightb")) nightBrightLevel = constrain(server.arg("nightb").toInt(), 0, 2);
  if (server.hasArg("nights")) nightSoundLevel = constrain(server.arg("nights").toInt(), 0, 3);
  if (server.hasArg("schoolon")) schoolModeOn = (server.arg("schoolon") == "1");
  if (server.hasArg("schoolsh")) parseHM(server.arg("schoolsh"), schoolStartHour, schoolStartMin, schoolStartHour);
  if (server.hasArg("schooleh")) parseHM(server.arg("schooleh"), schoolEndHour, schoolEndMin, schoolEndHour);
  if (server.hasArg("schoolb")) schoolBrightLevel = constrain(server.arg("schoolb").toInt(), 0, 2);
  if (server.hasArg("schools")) schoolSoundLevel = constrain(server.arg("schools").toInt(), 0, 3);
  if (server.hasArg("sensins")) sensInsTs = parseDateTimeLocal(server.arg("sensins"));
  if (server.hasArg("setins")) setInsTs = parseDateTimeLocal(server.arg("setins"));
  if (server.hasArg("sensins") || server.hasArg("setins")) { computeManualAges(); checkAgeWarnings(); }
  if (alarmLow >= alarmHigh) { alarmLow = 70; alarmHigh = 180; }
  if (alarmHigh >= alarmVeryHigh) alarmHigh = alarmVeryHigh - 10;
  if (alarmCritLow >= alarmLow) alarmCritLow = alarmLow - 5;
  if (alarmCritLow < 40) alarmCritLow = 40;
  // web kullanici/sifre webden degistirilemez (cihaza ozel sifre)
  webUser = WEB_USER;
  // Kaynak Ayarlari karti disindaki bir form (Alarm/Cihaz/Sensor-Set/Gece/Okul vb.)
  // kaydedildiginde bu alanlarin hicbiri POST edilmez — boyle bir durumda dataSource'a
  // hic dokunma, yoksa aktif kaynak sessizce degisir/iptal olurdu.
  bool srcFormSubmitted = server.hasArg("srcpick") || server.hasArg("nsurl") || server.hasArg("apisecret") ||
                          server.hasArg("dexuser") || server.hasArg("dexpass") || server.hasArg("dexsrv") ||
                          server.hasArg("libremail") || server.hasArg("librepass") || server.hasArg("librereg") ||
                          server.hasArg("medemail") || server.hasArg("medpass");
  if (srcFormSubmitted) {
    if (server.hasArg("srcpick")) {
      // Kullanici acilir kutudan acikca sectiyse otomatik tahmine gerek yok.
      String sp = server.arg("srcpick");
      if (sp == "nightscout" || sp == "dexcom" || sp == "libre" || sp == "medtrum") dataSource = sp;
      else detectDataSource();
    } else {
      detectDataSource();
    }
  }
  saveSettings();
  if (dataSource != o_src || nightscoutUrl != o_ns || apiSecret != o_sec || dexUser != o_du || dexPass != o_dp ||
      dexServer != o_ds || libreEmail != o_le || librePass != o_lp || libreRegion != o_lr || medEmail != o_me || medPass != o_mp) {
    // 3.4.1: eski kaynagin/hesabin degeri ekranda kalmasin; yeni ayarla ilk deneme hata verirse hata ekrani
    clearSourceData();
    consecutiveFetchFails = 0; clearFetchError();
    showErrorScreen = false; errorDismissed = false;
    srcChangedPending = true;
    credGen++;                            // suren cekmenin sonucu / oturumu gecersiz
    secKey = ""; caKey = "";              // acik baglantilar kapatilsin
    dexAccountId = ""; dexSessionId = ""; libreToken = ""; libreAccountId = ""; librePatientId = ""; medCookie = "";
    if (pk == dispCtx && isHomeScreen() && autoRefreshOk()) drawCurrentScreen();
  }
  ctxSwitch(dispCtx);                  // 4.0
  server.sendHeader("Location", "/?p=" + String(pk));
  server.send(303);
  requestFetch();
}


File fotoUploadFile;
static size_t fotoUploadWritten = 0;
static int fotoUploadSlot = -1;
static bool fotoUploadFull = false;
// Coklu foto (gercek SPIFFS kapasitesine gore FOTO_MAX=5): her yukleme sirali
// bir sonraki bos slota ("pic<fotoCount>.rgb") yaziliyor, mevcutlar
// silinmiyor. FOTO_MAX doluysa dosya hic acilmiyor, WRITE adimi sessizce
// atlaniyor ve handleFotoDone() acikca "FULL" doner.
//
// Onceki surumde burada her yuklemede loadFotoFromFS() (ki o da kendi
// icinde SPIFFS.begin()+clearOldFotoFiles() cagiriyor) ARDINDAN ayrica
// SPIFFS.end()+begin() "fragment temizligi" yapiliyordu - tek dosyalik
// eski tasarimdan kalma bir aliskanlikti. Web sayfasi artik birden fazla
// dosyayi ARKA ARKAYA (bekleme olmadan) yukleyince bu, her yukleme
// basina 3-4 SPIFFS.begin()/end() dongusu demek oluyordu; kullanicinin
// bildirdigi "sadece ilk foto yukleniyor, digerleri hata veriyor"
// belirtisiyle uyumlu bir SPIFFS montaj kararsizligina yol acmis
// olabilir. Duzeltme: START'ta TEK bir begin() yeterli, fotoCount zaten
// RAM'de guncel tutuluyor (asagida basarili yuklemede +1 artiyor) -
// diskten yeniden taramaya (loadFotoFromFS) gerek yok. Bu tarama hala
// acilista ve donusum zamanlayicisinda (loop()) calisiyor, o yuzden
// yarim kalmis/bozuk bir durum en gec birkac dakikada kendini duzeltir.
void handleFotoUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (fotoUploadFile) fotoUploadFile.close();          // 3.4.3: yarim kalmis yukleme dosyasi kapansin
    fotoUploadWritten = 0;
    fotoUploadFull = false;
    fotoUploadSlot = -1;
    if (!webCookieOk() || !sameOriginQuiet()) { server.client().stop(); return; }   // giris yoksa / baska site: hicbir sey yazma
    if (!SPIFFS.begin(true)) return;
    if (fotoCount >= FOTO_MAX) {
      fotoUploadFull = true;
      return;
    }
    fotoUploadSlot = fotoCount;
    fotoUploadFile = SPIFFS.open(fotoPath(fotoUploadSlot), FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (fotoUploadFile) { wdtFeed(); serviceAlarmSound(); }
    if (fotoUploadFile) {
      if (fotoUploadWritten + upload.currentSize > (size_t)FOTO_BYTES) {   // fazlasi yazilmaz
        fotoUploadFile.close();
        fotoUploadWritten = (size_t)FOTO_BYTES + 1;
      } else {
        size_t w = fotoUploadFile.write(upload.buf, upload.currentSize);
        fotoUploadWritten += w;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fotoUploadFile) {
      fotoUploadFile.close();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (fotoUploadFile) fotoUploadFile.close();
    if (fotoUploadSlot >= 0 && SPIFFS.exists(fotoPath(fotoUploadSlot))) SPIFFS.remove(fotoPath(fotoUploadSlot));
    fotoUploadSlot = -1; fotoUploadWritten = 0;
  }
}

void handleFotoDone() {
  if (!checkWebAuth()) return;
  if (fotoUploadFull) {
    server.send(200, "text/plain; charset=utf-8", "FULL max=" + String(FOTO_MAX));
    return;
  }
  if (fotoUploadWritten != (size_t)FOTO_BYTES) {
    if (fotoUploadSlot >= 0 && SPIFFS.exists(fotoPath(fotoUploadSlot))) SPIFFS.remove(fotoPath(fotoUploadSlot));
    size_t freeB = 0;
    if (SPIFFS.begin(true)) freeB = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    String msg = "FAIL size=" + String(fotoUploadWritten) + " need=" + String(FOTO_BYTES) + " free=" + String(freeB);
    server.send(200, "text/plain; charset=utf-8", msg);
    return;
  }
  // Basarili: fotoCount RAM'de dogrudan artirilir (biz zaten sirali/
  // bosluksuz yaziyoruz, diskten yeniden taramaya gerek yok - bu da
  // basina bir SPIFFS.begin() dongusu daha eklemekten kacinir).
  fotoCount = fotoUploadSlot + 1;
  fotoLoaded = true;
  server.send(200, "text/plain; charset=utf-8", "OK count=" + String(fotoCount));
}

void handleFotoPage() {
  if (!checkWebAuth()) return;
  String html = WT(R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foto yukle</title>
<link rel="icon" href="/logo.png">
<style>
body{font-family:system-ui;background:#0f1115;color:#eee;padding:16px}
.card{background:#1a1d24;border-radius:12px;padding:16px;max-width:480px;margin:auto}
.btn{display:inline-block;margin-top:12px;padding:12px 18px;background:#1a73e8;color:#fff;border:none;border-radius:8px}
canvas{max-width:100%;background:#000;margin-top:8px}
label{display:block;font-size:.85rem;color:#aaa;margin:14px 0 4px}
input[type=number]{width:100%;max-width:110px;padding:8px;border-radius:8px;border:1px solid #333;background:#111;color:#eee;font-size:1rem}
#st{white-space:pre-wrap;font-family:monospace;font-size:12px;color:#ddd}
</style></head><body><div class="card">
<h1>Foto yukle (256x192)</h1>
<p>En fazla )HTML");
  html += String(FOTO_MAX);
  html += WT(R"HTML( fotograf saklanabilir. Su an: <b id="cnt">)HTML");
  html += String(fotoCount) + "/" + String(FOTO_MAX);
  html += WT(R"HTML(</b> foto.</p>
<p style="color:#888;font-size:13px">Birden fazla dosya secebilirsiniz, sirayla otomatik yuklenir. )HTML");
  html += String(FOTO_MAX);
  html += WT(R"HTML( doluysa once "Fotograflari Sil"e basip yer acin.</p>
<p style="color:#888;font-size:13px">SPIFFS bos alan (su an): )HTML");
  {
    size_t freeB = 0, totalB = 0;
    if (SPIFFS.begin(true)) { freeB = SPIFFS.totalBytes() - SPIFFS.usedBytes(); totalB = SPIFFS.totalBytes(); }
    html += String(freeB / 1024) + "KB / " + String(totalB / 1024) + "KB";
  }
  html += WT(R"HTML(</p>
<input type="file" id="f" accept="image/*" multiple>
<canvas id="c" width="256" height="192" style="width:100%;max-width:320px"></canvas>
<button class="btn" id="clr" style="background:#666">Fotograflari Sil</button>
<p id="st"></p>

<label>Fotograf arayuzunde kac dakikada bir bir sonrakine gecilsin</label>
<input type="number" id="rot" min="1" max="180" value=")HTML");
  html += String(fotoRotateMin);
  html += WT(R"HTML(">
<button class="btn" id="rotsave" style="margin-top:10px">Bu ayari kaydet</button>
<p id="rotst" style="color:#888;font-size:13px">Sadece birden fazla foto yuklendiyse gecerli olur.</p>

<p><a href="/" style="color:#8af">← Geri</a></p>
</div>
<script>
const W=256,H=192,FMAX=)HTML");
  html += String(FOTO_MAX);
  // 3.6.0: betikteki mesajlar dile gore (M nesnesi)
  html += String(";\nvar M={up:'") + WT("Yukleniyor") + "',err:'" + WT("HATA") +
          "',full:'" + WT("%1 foto doldu, %2 tanesi yuklendi. Kalanlar icin once sil.") +
          "',ok:'" + WT("%1 foto yuklendi - Arayuz > Foto Cerceve sec") +
          "',part:'" + WT("%1 yuklendi, %2 hata verdi") +
          "',del:'" + WT("Siliniyor...") + "',sav:'" + WT("Kaydediliyor...") + "'}";
  html += R"HTML(;
function rgb565(r,g,b){return ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)}
function drawToCanvas(file){
  return new Promise((resolve)=>{
    const img=new Image();
    img.onload=()=>{
      const c=document.getElementById('c');
      const ctx=c.getContext('2d');
      // Cover: ekrani doldur (kirp)
      const s=Math.max(W/img.width,H/img.height);
      const dw=img.width*s, dh=img.height*s;
      ctx.drawImage(img,(W-dw)/2,(H-dh)/2,dw,dh);
      resolve();
    };
    img.src=URL.createObjectURL(file);
  });
}
function canvasToBuf(){
  const c=document.getElementById('c');
  const ctx=c.getContext('2d');
  const id=ctx.getImageData(0,0,W,H).data;
  const buf=new Uint8Array(W*H*2);
  for(let i=0,j=0;i<id.length;i+=4,j+=2){
    const v=rgb565(id[i],id[i+1],id[i+2]);
    // little-endian (ESP32 uint16)
    buf[j]=v&0xFF; buf[j+1]=v>>8;
  }
  return buf;
}
async function uploadOne(buf){
  const fd=new FormData();
  fd.append('foto', new Blob([buf],{type:'application/octet-stream'}), 'foto.rgb');
  const r=await fetch('/foto',{method:'POST',body:fd});
  const t=await r.text();
  return {ok:(t.indexOf('OK')===0), full:(t.indexOf('FULL')===0), text:t};
}
document.getElementById('f').onchange=async(e)=>{
  const files=Array.from(e.target.files); if(!files.length) return;
  const st=document.getElementById('st');
  let done=0, fail=0, stoppedFull=false, log=[];
  for(let i=0;i<files.length;i++){
    const file=files[i];
    st.textContent=log.join('\n')+(log.length?'\n':'')+M.up+' '+(i+1)+'/'+files.length+'...';
    await drawToCanvas(file);
    const buf=canvasToBuf();
    const res=await uploadOne(buf);
    if(res.ok){ done++; log.push((i+1)+'/'+files.length+': OK ('+file.name+')'); }
    else if(res.full){ stoppedFull=true; log.push((i+1)+'/'+files.length+': '+res.text); break; }
    else{ fail++; log.push((i+1)+'/'+files.length+': '+M.err+' — '+res.text+' ('+file.name+')'); }
    st.textContent=log.join('\n');
  }
  let summary = stoppedFull
    ? M.full.replace('%1',FMAX).replace('%2',done)
    : (fail===0 ? M.ok.replace('%1',done) : M.part.replace('%1',done).replace('%2',fail));
  st.textContent = summary + '\n\n' + log.join('\n');
  setTimeout(()=>location.reload(), fail>0 ? 6000 : 1200);
};
document.getElementById('clr').onclick=async()=>{
  document.getElementById('st').textContent=M.del;
  const r=await fetch('/foto/clear',{method:'POST'});
  const t=await r.text();
  document.getElementById('st').textContent=t;
  setTimeout(()=>location.reload(), 800);
};
document.getElementById('rotsave').onclick=async()=>{
  const v=document.getElementById('rot').value;
  document.getElementById('rotst').textContent=M.sav;
  const r=await fetch('/foto/rotate',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'min='+encodeURIComponent(v)});
  const t=await r.text();
  document.getElementById('rotst').textContent=t;
};
</script></body></html>)HTML";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleFotoRaw() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain; charset=utf-8", "POST only");
    return;
  }
  // raw body
  if (!SPIFFS.begin(true)) {
    server.send(500, "text/plain; charset=utf-8", "SPIFFS");
    return;
  }
  if (server.hasArg("plain") == false && server.arg("plain").length() == 0) {
    // try client content length stream
  }
  String body = server.arg("plain");
  // WebServer may not put binary in plain — use upload style alternative
  // Fallback: if body length matches
  if (body.length() == FOTO_BYTES) {
    File f = SPIFFS.open("/foto.rgb", "w");
    if (!f) { server.send(500, "text/plain; charset=utf-8", "open"); return; }
    f.write((const uint8_t*)body.c_str(), body.length());
    f.close();
    loadFotoFromFS();
    server.send(200, "text/plain; charset=utf-8", "OK");
    return;
  }
  server.send(400, "text/plain; charset=utf-8", "Need 98304 bytes RGB565");
}

void startOtaServer() {
  if (otaServerStarted || WiFi.status() != WL_CONNECTED) return;

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/foto", HTTP_GET, handleFotoPage);
  server.on("/foto", HTTP_POST, handleFotoDone, handleFotoUpload);
  server.on("/foto/clear", HTTP_POST, []() {
    if (!checkWebAuth()) return;
    if (!sameOriginOk()) return;
    if (!SPIFFS.begin(true)) { server.send(500, "text/plain; charset=utf-8", WT("Depolama okunamadi")); return; }
    clearOldFotoFiles();
    for (int i = 0; i < FOTO_MAX; i++) {
      String p = fotoPath(i);
      if (SPIFFS.exists(p)) SPIFFS.remove(p);
    }
    fotoCount = 0;
    fotoActiveIdx = 0;
    fotoLoaded = false;
    size_t freeB = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    server.send(200, "text/plain; charset=utf-8", WT("Tum fotograflar silindi, bos alan=") + String(freeB / 1024) + "KB");
  });
  server.on("/foto/rotate", HTTP_POST, []() {
    if (!checkWebAuth()) return;
    if (!sameOriginOk()) return;
    if (server.hasArg("min")) {
      int m = server.arg("min").toInt();
      if (m < 1) m = 1;
      if (m > 180) m = 180;
      fotoRotateMin = m;
      saveSettings();
    }
    server.send(200, "text/plain; charset=utf-8", WT("Kaydedildi: ") + String(fotoRotateMin) + WT(" dk"));
  });
  server.on("/logo.png", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "max-age=604800");
    server.send_P(200, "image/png", (const char*)WEB_LOGO_PNG, WEB_LOGO_PNG_LEN);
  });
  { static const char* hk[] = {"Cookie", "Origin", "Referer"}; server.collectHeaders(hk, 3); }

  // 2.4.0: giris (sadece sifre), sifre degistirme ve yazilim guncelleme
  server.on("/login", handleLogin);
  server.on("/logout", []() {
    server.sendHeader("Set-Cookie", "mmt=; Max-Age=0; Path=/");
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain; charset=utf-8", "");
  });
  server.on("/pass", HTTP_POST, handlePassChange);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  // 2.5.0: Karb-Not ve glukagon videosu
  server.on("/karb", HTTP_GET, handleKarbPage);
  server.on("/karb/save", HTTP_POST, handleKarbSave);

  server.begin();
  otaServerStarted = true;

  if (MDNS.begin(deviceName().c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://" + deviceName() + ".local");
  }
  Serial.print("Web: http://"); Serial.println(WiFi.localIP());
  Serial.print("OTA: http://"); Serial.print(WiFi.localIP()); Serial.println("/update");
}

void stopOtaServer() {
  if (!otaServerStarted) return;
  server.stop();
  otaServerStarted = false;
}

float parseAgeField(JsonVariant v) {
  if (v.isNull()) return -1;
  if (v.is<float>() || v.is<int>() || v.is<double>()) {
    float a = v.as<float>();
    return (a >= 0 && a < 10000) ? a : -1;
  }
  return -1;
}

// ---------------------------------------------------------------- 4.0 AILE MODU: kisi baglamlari
// Kodun geri kalani tek kisilik global degiskenlerle calisir (sgv, history, kaynak bilgileri...).
// Aile modunda her kisinin bu degiskenleri bir "yuvada" saklanir; o an ekranda gosterilen (dispCtx)
// veya verisi cekilen (fetchCtx) kisinin yuvasi globallere yuklenir (ctxSwitch). Diziler isaretciyle
// degisir, digerleri kopyalanir. Tek kisilik kullanimda (personCount = 1) hicbir sey degismez.
#define CTX_FIELDS \
  X(String, dataSource) X(String, nightscoutUrl) X(String, apiSecret) X(String, dexUser) X(String, dexPass) \
  X(String, dexServer) X(String, libreEmail) X(String, librePass) X(String, libreRegion) X(String, medEmail) \
  X(String, medPass) X(String, medCookie) X(int, medLastCode) X(String, dexAccountId) X(String, dexSessionId) \
  X(String, libreToken) X(String, libreAccountId) X(String, librePatientId) X(bool, nsUsedFallback200) \
  X(int, sgv) X(int, delta) X(bool, deltaInterp) X(bool, deltaUnknown) X(String, direction) \
  X(unsigned long, readingAgeSec) X(uint64_t, lastEntryDateMs) X(unsigned long, entrySeenAtMillis) \
  X(String, lastDateString) X(bool, dataValid) X(bool, ageNtpAnchored) X(bool, medFillOk) X(bool, medHistOk) \
  X(bool, medResetPending) X(int, consecutiveFetchFails) X(int, lastErrorCode) X(String, lastErrorDetail) \
  X(bool, showErrorScreen) X(bool, errorDismissed) X(unsigned long, lastErrorShownAt) \
  X(float, sensorAgeHours) X(float, cannulaAgeHours) X(int, sensorLifeHours) X(int, cannulaLifeHours) \
  X(bool, sensorTrackOn) X(bool, cannulaTrackOn) X(float, lastSensorAgeForWarn) X(float, lastCannulaAgeForWarn) \
  X(int, historyCount) X(int, alarmLow) X(int, alarmHigh) X(int, alarmVeryHigh) X(int, alarmCritLow) \
  X(unsigned long, alarmSnoozeUntil) X(bool, snoozeActive) X(int, snoozedKind) X(bool, requestAlarmBurst) \
  X(bool, libreRingReset) X(bool, libBurstReset) X(float, nsIob) X(float, nsCob) X(unsigned long, nsIobOkMs) \
  X(unsigned long, nsCobOkMs) X(int, bolusCount) X(unsigned long, lastNsExtraFetch) X(int, localHistCount) \
  X(unsigned long, lhLastSave) X(bool, lhDirty) X(uint32_t, lastLocalAppendTs) X(int, lastLibreSgv) \
  X(uint32_t, sensInsTs) X(uint32_t, setInsTs) X(String, ctxLastSrc) X(int, lrN) X(bool, lrKeyTs) \
  X(unsigned long, lrLastBurst) X(int, lrLastKind) X(unsigned long, nsFullOkMs) X(unsigned long, medLastHistTry) \
  X(unsigned long, lastCritBurst) X(unsigned long, lastStaleBurst) X(String, personName)

struct PersonSlot {
#define X(t, n) t n;
  CTX_FIELDS
#undef X
  bool sWarn[4], cWarn[4];
  BolusRec bol[BOLUS_MAX];
  uint32_t rTs[10]; int16_t rV[10];
  bool lastOk;                        // son veri cekme basarili mi
};
PersonSlot pSlot[PERSON_MAX];

void ctxSaveTo(int k) {
  PersonSlot& s = pSlot[k];
#define X(t, n) s.n = n;
  CTX_FIELDS
#undef X
  memcpy(s.sWarn, sensorWarnFired, sizeof(s.sWarn)); memcpy(s.cWarn, cannulaWarnFired, sizeof(s.cWarn));
  memcpy(s.bol, boluses, sizeof(s.bol)); memcpy(s.rTs, lrTs, sizeof(s.rTs)); memcpy(s.rV, lrV, sizeof(s.rV));
}
void ctxLoadFrom(int k) {
  PersonSlot& s = pSlot[k];
#define X(t, n) n = s.n;
  CTX_FIELDS
#undef X
  memcpy(sensorWarnFired, s.sWarn, sizeof(s.sWarn)); memcpy(cannulaWarnFired, s.cWarn, sizeof(s.cWarn));
  memcpy(boluses, s.bol, sizeof(s.bol)); memcpy(lrTs, s.rTs, sizeof(s.rTs)); memcpy(lrV, s.rV, sizeof(s.rV));
  history = histStore[k]; historyDay = histDayStore[k]; historyTsSec = histTsStore[k];
  localHist = lhStore[k]; localHistTs = lhTsStore[k];
  readingAgeSec = (entrySeenAtMillis == 0) ? 0 : (millis() - entrySeenAtMillis) / 1000UL;   // yas guncel olsun
}
void ctxSwitch(int k) {
  if (k < 0 || k >= PERSON_MAX || k == liveCtx) return;
  ctxSaveTo(liveCtx);
  ctxLoadFrom(k);
  liveCtx = k;
}
// Kisi yuvasinin guncel hali (canli kisi icin once globallerden kopyalanir)
PersonSlot& ctxPeek(int k) {
  if (k == liveCtx) ctxSaveTo(k);
  return pSlot[k];
}
void localHistSaveAll() {   // 4.0: kapanirken tum kisilerin kendi kaydi
  int keep = liveCtx;
  for (int k = 0; k < personCount; k++) { ctxSwitch(k); localHistSave(true); }
  ctxSwitch(keep);
}
String personNameOf(int k) { return ctxPeek(k).personName; }
String personLabel(int k) {
  String n = ctxPeek(k).personName;
  n.trim();
  if (!n.length()) n = String(L("Kişi ", "Person ")) + String(k + 1);
  return n;
}
const char* personNs(int k) { return k == 1 ? "mmp1" : (k == 2 ? "mmp2" : "metematik"); }

// Acilista: 1. kisi globallerde (loadSettings); digerleri bos durumla + kendi ayarlariyla yuvalarina
void initPersons() {
  liveCtx = 0; dispCtx = 0;
  ctxSaveTo(0);
  for (int k = 1; k < PERSON_MAX; k++) {
    pSlot[k] = pSlot[0];                       // calisma durumu: acilistaki bos hal
    liveCtx = k; ctxLoadFrom(k);
    medCookie = ""; dexAccountId = ""; dexSessionId = ""; libreToken = ""; libreAccountId = ""; librePatientId = "";
    loadPersonPrefs(k);
    if (k < personCount) localHistLoad();
    ctxSaveTo(k);
  }
  liveCtx = 0; ctxLoadFrom(0);
}


// ---------------------------------------------------------------- 4.0 AILE MODU: ekran ve sira
int famPillX = 0, famPillY = 0, famPillW = 0, famPillH = 0;   // kisi adi hapi (dokununca siradaki kisi)
unsigned long famLastRot = 0;
int famRank[PERSON_MAX] = {0, 0, 0};

// Alarm onceligi: kritik > dusuk > cok yuksek > hizli dusus > veri yok > yuksek > hizli yukselis
int famKindRank(int kind) {
  switch (kind) { case 1: return 7; case 2: return 6; case 4: return 5; case 6: return 4; case 5: return 3; case 3: return 2; case 7: return 1; }
  return 0;
}
// canli kisinin alarm onceligi (kaynak hic kurulmamis kisi alarm vermez)
int famLiveRank() {
  if (!dataValid && lastErrorCode == 5) return 0;
  if (!dataValid && lastErrorCode == 0) return 0;      // henuz hic cekilmedi (yeni eklenen kisi)
  if (!isAlarmCondition()) return 0;
  return famKindRank(getAlarmKind());
}
void famSelect(int k) {
  if (k < 0 || k >= personCount) k = 0;
  if (k == dispCtx && k == liveCtx) return;
  dispCtx = k;
  ctxSwitch(k);
  graphDetailOpen = false;
}
// Kisi degistirmek guvenli mi (ayar duzenlenirken, Rehber akisi veya video sirasinda degil)
bool famSafeToSwitch() {
  return formId == 0 && !timePickerActive && !textPageActive && teacherStep == 0 && subScreen == 0 &&
         !showGuide && !ageWarnActive && currentScreen != SCR_VIDEO && currentScreen != SCR_THEME;
}
bool famAutoScreen() {   // otomatik sira sadece ana ekranda
  return isHomeScreen() && famSafeToSwitch() && !uiPreviewActive;
}

void drawPersonPill(int cx, int y) {
  if (personCount < 2) return;
  String n = personLabel(dispCtx);
  cv->setFont(&FreeSansTR7pt);
  int w = cv->textWidth(n.c_str()) + 34;
  cv->setTextFont(1);
  if (w > 190) w = 190;
  int x = cx - w / 2;
  cv->fillRoundRect(x, y, w, 22, 11, 0x2945);
  cv->drawRoundRect(x, y, w, 22, 11, 0x6B4D);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(n.c_str(), cx - 6, y + 11, &FreeSansTR7pt);
  int ax = x + w - 13, ay = y + 11;                   // ">" : dokun, siradaki kisi
  cv->drawLine(ax - 3, ay - 5, ax + 2, ay, 0xA514); cv->drawLine(ax + 2, ay, ax - 3, ay + 5, 0xA514);
  cv->drawLine(ax - 2, ay - 5, ax + 3, ay, 0xA514); cv->drawLine(ax + 3, ay, ax - 2, ay + 5, 0xA514);
  cv->setTextDatum(TL_DATUM);
  famPillX = x; famPillY = y; famPillW = w; famPillH = 22;
}
bool famPillHit(int x, int y) {
  return personCount > 1 && famPillW > 0 && x >= famPillX - 8 && x < famPillX + famPillW + 8 && y >= famPillY - 8 && y < famPillY + famPillH + 8;
}
void famRedraw() {
  if (showErrorScreen && isHomeScreen()) drawErrorScreenFull(); else drawCurrentScreen();
}
void famManualNext() {
  famSelect((dispCtx + 1) % personCount);
  famLastRot = millis();
  famRedraw();
}

// Bolunmus ana ekran: her kisi bir satir (isim, yas | deger, ok, delta)
int famSplitRowY(int k, int& h) {
  int top = 24;
  h = (240 - top) / personCount;
  return top + k * h;
}
void drawFamSplit() {
  cv->fillScreen(UI_BG);
  lightBg = false;
  drawStatusBar(false);
  int keep = liveCtx;
  for (int k = 0; k < personCount; k++) {
    ctxSwitch(k);
    int h, y0 = famSplitRowY(k, h);
    bool alarm = famLiveRank() > 0;
    homeStaleGrey = homeDataStale();
    uint16_t col = dataValid ? getGlucoseColor(sgv) : (uint16_t)0x8410;
    homeStaleGrey = false;
    uint16_t card = 0x10A2;
    bool blinkOn = alarm && k == dispCtx && visualAlarmOn && alarmBlinkState && (long)(millis() - visualBurstEnd) < 0;
    if (blinkOn) card = getAlarmColor();
    cv->fillRoundRect(4, y0 + 2, 312, h - 4, 10, card);
    if (alarm) { cv->drawRoundRect(4, y0 + 2, 312, h - 4, 10, sevColor(0)); cv->drawRoundRect(5, y0 + 3, 310, h - 6, 9, sevColor(0)); }
    int cy = y0 + h / 2;
    // sol: isim + yas
    cv->setTextColor(blinkOn ? (uint16_t)TFT_BLACK : (uint16_t)TFT_WHITE, card);
    cv->setTextDatum(ML_DATUM);
    String nm = personLabel(k);
    drawFreeTitle(nm.c_str(), 14, cy - 12, &FreeSansTR9pt);
    cv->setTextColor(blinkOn ? (uint16_t)TFT_BLACK : (uint16_t)0xA514, card);
    String sub;
    if (!dataValid && lastErrorCode == 5) sub = L("Kaynak yok", "No source");
    else if (!dataValid) sub = L("Veri yok", "No data");
    else { sub = ageText(); if (langIdx == 0) sub.replace("once", "önce"); }
    drawFreeTitle(sub.c_str(), 14, cy + 12, &FreeSansTR6pt);
    // sag: deger + ok + delta
    if (dataValid && sgv > 0) {
      uint16_t vc = blinkOn ? (uint16_t)TFT_BLACK : col;
      String gs = formatGlucose(sgv);
      cv->setTextColor(vc, card);
      cv->setTextDatum(MR_DATUM);
      int big = (h >= 90) ? 6 : 4;
      cv->drawString(gs.c_str(), 236, cy, big);
      drawTrendArrow(256, cy, direction, vc, (h >= 90) ? 14 : 11);
      cv->setTextDatum(MR_DATUM);
      dsN(cv, formatDelta(delta), 314, cy, 2);
    } else {
      cv->setTextColor(0x8410, card);
      cv->setTextDatum(MR_DATUM);
      cv->drawString("---", 300, cy, 4);
    }
  }
  ctxSwitch(keep);
  // alarmdaki (ekrandaki) kisi icin Ertele / Dikkat
  if (dataValid && shouldShowSnoozeButton()) {
    int h, y0 = famSplitRowY(dispCtx, h);
    int bx = 150, by = y0 + h - 30;
    cv->fillRoundRect(bx, by, 70, 24, 8, 0x3186);
    cv->setTextColor(TFT_WHITE, 0x3186);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("Ertele", "Snooze"), bx + 35, by + 12, &FreeSansTR6pt);
  }
  if (dataValid && shouldShowDikkat()) {
    int h, y0 = famSplitRowY(dispCtx, h);
    int bx = 150, by = y0 + 6;
    cv->fillRoundRect(bx, by, 70, 24, 8, sevColor(0));
    cv->setTextColor(TFT_WHITE, sevColor(0));
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("Dikkat", "Alert"), bx + 35, by + 12, &FreeSansTR6pt);
  }
  cv->setTextDatum(TL_DATUM);
}
// Bolunmus ekranda dokunma: true = islendi
bool famSplitTap(int x, int y) {
  if (!(personCount > 1 && famMode == 1 && isHomeScreen() && teacherStep == 0)) return false;
  int h, y0 = famSplitRowY(dispCtx, h);
  if (x >= 146 && x < 224) {
    if (dataValid && shouldShowSnoozeButton() && y >= y0 + h - 34 && y < y0 + h) { doSnooze(); return true; }
    if (dataValid && shouldShowDikkat() && y >= y0 + 2 && y < y0 + 34) { startTeacherFlow(); return true; }
  }
  return false;
}

// Ana donguden: alarmdaki kisiyi ekrana al; sirayla modunda 15 sn'de bir siradaki kisi
// 4.0.1: sesli alarm hangi kisi icin (en acil alarmdaki kisi; yoksa ekrandaki)
int famSoundCtx = 0;
int famPrevRank[PERSON_MAX] = {0, 0, 0};
void serviceAlarmSoundFam() {
  if (personCount < 2 || famSoundCtx == dispCtx || famSoundCtx >= personCount) { serviceAlarmSound(); return; }
  ctxSwitch(famSoundCtx);             // ekranda baska kisi olsa da alarmdaki kisinin sesi calar
  serviceAlarmSound();
  ctxSwitch(dispCtx);
}

void famService() {
  if (personCount < 2) { famSoundCtx = 0; if (dispCtx != 0 || liveCtx != 0) famSelect(0); return; }
  if (dispCtx >= personCount) { famSelect(0); if (autoRefreshOk()) famRedraw(); }
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 1000UL) return;
  lastCheck = millis();
  int best = -1, bestRank = 0, newAlarm = -1, newRank = 0;
  int keep = liveCtx;
  for (int k = 0; k < personCount; k++) {
    ctxSwitch(k);
    famRank[k] = famLiveRank();
    if (famRank[k] > bestRank) { bestRank = famRank[k]; best = k; }
    if (famRank[k] > famPrevRank[k] && famRank[k] > newRank) { newRank = famRank[k]; newAlarm = k; }   // yeni / agirlasan alarm
    famPrevRank[k] = famRank[k];
  }
  ctxSwitch(keep);
  famSoundCtx = (best >= 0 && famRank[dispCtx] < bestRank) ? best : dispCtx;
  // Otomatik gecis sadece ana ekranda; grafik / istatistik / sensor ve ayarlarda secilen kisi kalir
  if (!famAutoScreen()) return;
  int target = dispCtx;
  if (newAlarm >= 0 && newAlarm != dispCtx) target = newAlarm;                       // yeni alarm: hemen goster
  else if (famMode == 1) { if (best >= 0 && famRank[dispCtx] < bestRank) target = best; }   // bolunmus: Ertele/Dikkat en acil kiside
  else if (millis() - famLastRot > 15000UL && millis() - lastUserTouchMs > 15000UL) {
    // sirayla: alarm olsa da 15 sn'de bir siradaki kisi (kaynagi hic kurulmamis kisiler atlanir)
    for (int i = 1; i < personCount; i++) {
      int k = (dispCtx + i) % personCount;
      PersonSlot& ps = ctxPeek(k);
      if (ps.dataValid || ps.lastErrorCode != 5) { target = k; break; }
    }
    if (target == dispCtx) famLastRot = millis();
  }
  if (target != dispCtx) {
    famSelect(target);
    famLastRot = millis();
    if (autoRefreshOk()) famRedraw();
  }
}

// Veri cekme araligi: kisilerin en kisasi
unsigned long famFetchInterval() {
  if (personCount < 2) return fetchIntervalNow();
  unsigned long m = 0xFFFFFFFFUL;
  int keep = liveCtx;
  for (int k = 0; k < personCount; k++) { ctxSwitch(k); unsigned long v = fetchIntervalNow(); if (v < m) m = v; }
  ctxSwitch(keep);
  return m;
}
// ---------------------------------------------------------------- 3.4.0 arka planda veri cekme
// Veri cekme (fetchData) ayri bir gorevde (cekirdek 0) calisir. Ortak veriye tek
// kilit (dataMtx) ile erisilir: ana dongu kilidi hep tutar, her tur sonunda kisa
// bir an birakir. Veri gorevi kilidi sadece gelen cevabi islerken tutar; sunucudan
// cevap beklerken (asil yavas kisim) kilidi birakir, boylece ekran ve dokunma donmaz.
SemaphoreHandle_t dataMtx = NULL;
TaskHandle_t fetchTaskH = NULL;
TaskHandle_t loopTaskH = NULL;
volatile bool fetchBusy = false;       // gorev calisiyor (istek verildi, bitmedi)
volatile bool fetchDoneFlag = false;   // sonuc hazir, ana dongu isleyecek
volatile bool fetchResult = false;
volatile bool fetchAgain = false;      // calisirken yeni istek geldi: bitince bir daha
volatile bool fetchWaitLock = false;   // gorev kilidi bekliyor
volatile unsigned long fetchBeat = 0;  // gorevin son ilerleme ani (takilma bekcisi)
bool errRetryPending = false;          // hata ekranindaki "Yenile" sonucu bekleniyor
bool detailRefreshPending = false;     // grafik detayi acik: bitince yeniden ciz

bool inFetchTask() { return fetchTaskH != NULL && xTaskGetCurrentTaskHandle() == fetchTaskH; }
// 4.0: kilit birakilirken globallere ekrandaki kisi geri yuklenir, alinirken cekilen kisi
void netRelease() { if (inFetchTask()) { fetchBeat = millis(); ctxSwitch(dispCtx); xSemaphoreGiveRecursive(dataMtx); } }
void netAcquire() {
  if (!inFetchTask()) return;
  fetchWaitLock = true; fetchBeat = millis();
  xSemaphoreTakeRecursive(dataMtx, portMAX_DELAY);
  fetchWaitLock = false; fetchBeat = millis();
  if (fetchCtx >= 0) ctxSwitch(fetchCtx);
}

// 4.0: tum kisilerin verisini sirayla cek; sonuc ekrandaki kisinin sonucudur
bool fetchAllPersons() {
  bool okDisp = false, anyDiscard = false;
  for (int k = 0; k < personCount; k++) {
    fetchCtx = k;
    ctxSwitch(k);
    bool ok = fetchData();
    pSlot[k].lastOk = ok;
    if (fetchDiscarded) anyDiscard = true;
    if (k == dispCtx) okDisp = ok;
  }
  fetchCtx = -1;
  ctxSwitch(dispCtx);
  fetchDiscarded = anyDiscard;
  return okDisp;
}
// Sunucu cagrilari: beklerken kilit serbest
int netGET(HTTPClient& h) { netRelease(); int r = h.GET(); netAcquire(); return r; }
int netPOST(HTTPClient& h, const String& body) { netRelease(); int r = h.POST(body); netAcquire(); return r; }
String netBody(HTTPClient& h) { netRelease(); String s = h.getString(); netAcquire(); return s; }

// 3.4.8: internete cikilabiliyor mu? (1.1.1.1 / 8.8.8.8 HTTPS portuna kisa baglanti)
bool internetProbe() {
  netRelease();
  bool ok = false;
  const IPAddress hs[2] = {IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8)};
  for (int i = 0; i < 2 && !ok; i++) {
    WiFiClient c;
    if (c.connect(hs[i], 443, 2500)) ok = true;
    c.stop();
  }
  netAcquire();
  return ok;
}

// Gelen cevabi cozmek (JSON) yerel belgeye yazar, ortak veriye dokunmaz: kilit serbest
DeserializationError jsonParse(JsonDocument& d, const String& s) {
  netRelease(); DeserializationError e = deserializeJson(d, s); netAcquire(); return e;
}
DeserializationError jsonParseF(JsonDocument& d, const String& s, JsonDocument& f) {
  netRelease(); DeserializationError e = deserializeJson(d, s, DeserializationOption::Filter(f)); netAcquire(); return e;
}

// HTTPClient, istemci bagliyken yeni adrese de ayni baglantiyi kullanir: sunucu (anahtar)
// degistiyse once baglantiyi kapat
void secureFor(WiFiClientSecure& c, String& lastKey, const String& key) {
  if (key != lastKey) { c.stop(); lastKey = key; }
}
bool nsIsToken(const String& s) {
  int d = s.lastIndexOf('-');
  if (d < 1 || (int)s.length() - d - 1 != 16) return false;
  for (int i = d + 1; i < (int)s.length(); i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}
String nsSecretHash() {
  static String lastIn = "", lastOut = "";
  if (apiSecret == lastIn && lastOut.length()) return lastOut;
  lastIn = apiSecret;
  bool hex40 = apiSecret.length() == 40;
  for (int i = 0; hex40 && i < 40; i++) if (!isxdigit((unsigned char)apiSecret[i])) hex40 = false;
  if (hex40) { lastOut = apiSecret; lastOut.toLowerCase(); return lastOut; }
  unsigned char out[20];
  mbedtls_sha1((const unsigned char*)apiSecret.c_str(), apiSecret.length(), out);
  char hx[41]; for (int i = 0; i < 20; i++) sprintf(hx + i * 2, "%02x", out[i]); hx[40] = 0;
  lastOut = String(hx);
  return lastOut;
}
void nsAuth(HTTPClient& h) {
  if (!apiSecret.length() || nsIsToken(apiSecret)) return;
  h.addHeader("api-secret", nsSecretHash());
}
String nsTok(const String& url) {
  if (!nsIsToken(apiSecret)) return url;
  return url + (url.indexOf('?') >= 0 ? "&" : "?") + "token=" + apiSecret;
}

// 3.6.1: kesintide delta. 5-7 dk: gercek fark. 7-15 dk: 5 dakikaya cevrilir ve yaninda * yazar
// (Nightscout gibi). 15 dk'dan uzun bosluk: delta bilinmiyor, "--" yazar.
int deltaGap(int diff, uint32_t age) {
  if (age <= 420) return diff;
  if (age <= 900) { deltaInterp = true; return (int)lroundf(diff * 300.0f / (float)age); }
  deltaUnknown = true; return 0;
}
int deltaFromHistory(int fallback) {
  deltaInterp = false; deltaUnknown = false;
  if (historyCount < 2) return fallback;
  uint32_t t1 = historyTsSec[historyCount - 1];
  if (t1 == 0) return fallback;
  int cur = history[historyCount - 1];
  if (cur < 40) return fallback;
  for (int j = historyCount - 2; j >= 0 && j >= historyCount - 10; j--) {
    if (history[j] < 40) continue;
    uint32_t t0 = historyTsSec[j];
    if (t0 == 0 || t0 > t1) continue;
    uint32_t age = t1 - t0;
    if (age < 150) continue;               // ayni / cift kayit
    return deltaGap(cur - history[j], age);   // 3.6.1
  }
  return fallback;
}

void fetchTaskFn(void* arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    fetchWaitLock = true; fetchBeat = millis();
    xSemaphoreTakeRecursive(dataMtx, portMAX_DELAY);
    fetchWaitLock = false; fetchBeat = millis();
    bool ok = fetchAllPersons();   // 4.0
    fetchResult = ok;
    fetchBusy = false;
    fetchDoneFlag = true;
    xSemaphoreGiveRecursive(dataMtx);
  }
}

// setup sonunda: gorevi baslat, ana dongu kilidi alir
void bgFetchStart() {
  loopTaskH = xTaskGetCurrentTaskHandle();
  dataMtx = xSemaphoreCreateRecursiveMutex();
  if (!dataMtx) return;
  xSemaphoreTakeRecursive(dataMtx, portMAX_DELAY);
  if (xTaskCreatePinnedToCore(fetchTaskFn, "fetch", 16384, NULL, 1, &fetchTaskH, 0) != pdPASS) fetchTaskH = NULL;
}

// Veri cekme istegi. Gorev yoksa (kurulum sirasinda) eskisi gibi hemen cek.
void requestFetch() {
  if (!fetchTaskH) { fetchResult = fetchAllPersons(); fetchDoneFlag = true; return; }
  if (fetchBusy) { fetchAgain = true; return; }
  fetchBusy = true; fetchBeat = millis();
  xTaskNotifyGive(fetchTaskH);
}

// Ana dongu turu bitti: veri gorevi bekliyorsa kilidi ona birak
void bgYield() {
  if (!dataMtx || !fetchTaskH) return;
  xSemaphoreGiveRecursive(dataMtx);
  if (fetchWaitLock) vTaskDelay(2);
  xSemaphoreTakeRecursive(dataMtx, portMAX_DELAY);
}

// Veri gorevi 170 sn'den uzun ilerlemediyse bekci beslenmez (kendini resetler)
bool fetchStuck() {
  return fetchTaskH && fetchBusy && !fetchWaitLock && (millis() - fetchBeat > 170000UL);
}

void loop() {
  loopBody();
  bgYield();
}

void fetchSageCage() {
  // Nightscout api/v2/properties/sage,cage — ornek Nightscout yaniti ile uyumlu
  // 3.4.0: istek basarisizsa eski degerler kalir; sadece basarili cevapta guncellenir
  if (dataSource != "nightscout") { sensorAgeHours = -1; cannulaAgeHours = -1; return; }

  HTTPClient http;
  String url = nightscoutUrl;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/api/v2/properties/sage,cage";
  url = nsTok(url);

  if (url.startsWith("https")) {
    http.begin(nsSecure(), url);
  } else {
    http.begin(url);
  }
  http.setTimeout(15000);
  nsAuth(http);

  int code = netGET(http);
  if (code != 200) {
    http.end();
    return;
  }
  String payload = netBody(http);
  http.end();

  DynamicJsonDocument doc(12288);
  if (jsonParse(doc, payload)) return;
  sensorAgeHours = -1;
  cannulaAgeHours = -1;

  // Sensor: once Sensor Change, yoksa Sensor Start
  if (doc["sage"]["Sensor Change"]["found"] | false) {
    sensorAgeHours = doc["sage"]["Sensor Change"]["age"].as<float>();
  } else if (doc["sage"]["Sensor Start"]["found"] | false) {
    sensorAgeHours = doc["sage"]["Sensor Start"]["age"].as<float>();
  }

  // Set / cannula
  if (doc["cage"]["found"] | false) {
    cannulaAgeHours = doc["cage"]["age"].as<float>();
  }

  if (sensorAgeHours < 0) sensorAgeHours = -1;
  if (cannulaAgeHours < 0) cannulaAgeHours = -1;
}

// Sure-dolum uyari kuyruguna bir esik ekler (ayni anda birden fazla esik
// tetiklenirse - ornegin cihaz uzun sure kapaliyken acilinca - kuyruga
// alinip sirayla, en fazla AGE_WARN_SHOW_MS suresince gosterilir).
int ageWarnQueuePerson[4];
int ageWarnPerson = 0;                // 4.0: uyarinin ait oldugu kisi
void pushAgeWarn(int kind, int threshIdx) {
  if (ageWarnQueueLen >= 4) return; // pratikte hic dolmaz (sensor+set = en fazla 2)
  ageWarnQueuePerson[ageWarnQueueLen] = liveCtx;
  ageWarnQueueKind[ageWarnQueueLen] = kind;
  ageWarnQueueIdx[ageWarnQueueLen] = threshIdx;
  ageWarnQueueLen++;
}

// Sensor VEYA set/pompa icin: yeni bir degisim algila (yas geriye sicradiysa
// esik bayraklarini sifirla), sonra kalan sureyi hesaplayip AGE_WARN_THRESH_H
// esiklerinden (kucukten buyuge) henuz haber verilmemis en acil olani
// kuyruga ekle. Ayni check icinde birden fazla esik birden gecilmisse (ornek:
// cihaz uzun sure kapaliydi) hepsi "haber verildi" sayilir ama sadece EN
// ACIL olani (en kucuk kalan sureye sahip esik) gosterilir - eski/gecilmis
// esikler icin ayrica uyari birikmez.
// 3.4.14: hangi esiklerin bildirildigi kalici (cihaz kapatilip acilinca ayni uyari tekrar gelmesin)
void saveAgeWarnState(int kind, const bool* warnFired, float ageHours) {
  uint8_t m = 0;
  for (int i = 0; i < 4; i++) if (warnFired[i]) m |= (1 << i);
  Preferences pp;                     // 4.0: kisinin kendi alani
  pp.begin(personNs(liveCtx), false);
  pp.putUChar(kind == 0 ? "awm0" : "awm1", m);
  pp.putFloat(kind == 0 ? "awa0" : "awa1", ageHours);
  pp.end();
}

void checkAgeOne(int kind, bool trackOn, float ageHours, int lifeHours, bool* warnFired, float &lastAgeForWarn) {
  bool changed = false;
  if (ageHours >= 0 && lastAgeForWarn >= 0 && ageHours < lastAgeForWarn - 1.0f) {
    for (int i = 0; i < 4; i++) { if (warnFired[i]) changed = true; warnFired[i] = false; }   // yeni sensor/set
  }
  if (ageHours >= 0) lastAgeForWarn = ageHours;
  if (changed) saveAgeWarnState(kind, warnFired, ageHours);
  if (!trackOn || ageHours < 0) return;
  float remH = (float)lifeHours - ageHours;
  if (remH <= 0) return; // "Doldu" zaten Sensor/Set ekraninda gosteriliyor
  int mostUrgent = -1;
  for (int i = 0; i < 4; i++) { // AGE_WARN_THRESH_H: 0=15dk,1=1sa,2=3sa,3=12sa
    if (remH <= AGE_WARN_THRESH_H[i] && !warnFired[i]) {
      warnFired[i] = true;
      if (mostUrgent < 0) mostUrgent = i;
    }
  }
  if (mostUrgent >= 0) { pushAgeWarn(kind, mostUrgent); saveAgeWarnState(kind, warnFired, ageHours); }
}

void checkAgeWarnings() {
  checkAgeOne(0, sensorTrackOn, sensorAgeHours, sensorLifeHours, sensorWarnFired, lastSensorAgeForWarn);
  checkAgeOne(1, cannulaTrackOn, cannulaAgeHours, cannulaLifeHours, cannulaWarnFired, lastCannulaAgeForWarn);
}

// Kuyrukta bekleyen bir uyari varsa ve su an baska uyari gosterilmiyorsa
// aktif hale getirir, gerekiyorsa kisa bipi calar ve ekrani ceker. loop()
// icinde hem yeni veri geldikten sonra hem de bir uyari otomatik kapandiktan
// hemen sonra (sirada baskasi varsa) cagrilir.
void pumpAgeWarnQueue() {
  if (ageWarnActive || ageWarnQueueLen <= 0) return;
  if (currentScreen == SCR_VIDEO || teacherStep > 0) return;   // 3.4.20: video / Dikkat akisi bitince gosterilir
  ageWarnKind = ageWarnQueueKind[0];
  ageWarnThreshIdx = ageWarnQueueIdx[0];
  ageWarnPerson = ageWarnQueuePerson[0];
  for (int i = 1; i < ageWarnQueueLen; i++) {
    ageWarnQueuePerson[i - 1] = ageWarnQueuePerson[i];
    ageWarnQueueKind[i - 1] = ageWarnQueueKind[i];
    ageWarnQueueIdx[i - 1] = ageWarnQueueIdx[i];
  }
  ageWarnQueueLen--;
  ageWarnActive = true;
  ageWarnShowUntil = millis() + AGE_WARN_SHOW_MS;
  if (ageWarnThreshIdx == 0) playAgeWarnBeep(); // sadece "15 dakika kaldi" icin
  drawCurrentScreen();
}

// ISO "2024-08-25T14:30:00.000Z" veya "2024-08-25T14:30:00+03:00" -> unix sec (UTC yaklasik)
static uint32_t parseIsoToUnix(const char* iso) {
  if (!iso || strlen(iso) < 19) return 0;
  int Y=0,M=0,D=0,h=0,m=0,s=0;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) < 5) return 0;
  if (Y < 2020 || M < 1 || M > 12 || D < 1 || D > 31) return 0;
  struct tm t = {};
  t.tm_year = Y - 1900;
  t.tm_mon = M - 1;
  t.tm_mday = D;
  t.tm_hour = h;
  t.tm_min = m;
  t.tm_sec = s;
  time_t ut = mktime(&t); // cihaz TZ — yaklasik
  if (ut < 0) return 0;
  return (uint32_t)ut;
}

void fetchNsIobCobBolus(bool force = false) {
  if (dataSource != "nightscout") return;
  if (!force && lastNsExtraFetch != 0 && millis() - lastNsExtraFetch < NS_EXTRA_INTERVAL) return;
  lastNsExtraFetch = millis();

  // Eski degerleri silme — basarisiz istekte ekran bosalmasin; sadece basarili parse'da guncelle
  float newIob = -1.0f, newCob = -1.0f;
  int newBolus = 0;
  BolusRec tmpBol[BOLUS_MAX];

  String base = nightscoutUrl;
  while (base.endsWith("/")) base.remove(base.length() - 1);

  // 1) IOB / COB
  {
    HTTPClient http;
    String url = base + "/api/v2/properties/iob,cob";
    url = nsTok(url);
    if (url.startsWith("https")) { http.begin(nsSecure(), url); }
    else http.begin(url);
    http.setTimeout(6000);
    nsAuth(http);
    int code = netGET(http);
    if (code == 200) {
      String payload = netBody(http);
      DynamicJsonDocument doc(12288);
      if (!jsonParse(doc, payload)) {
        if (doc["iob"]["display"].is<float>() || doc["iob"]["display"].is<int>())
          newIob = doc["iob"]["display"].as<float>();
        else if (doc["iob"]["iob"].is<float>() || doc["iob"]["iob"].is<int>())
          newIob = doc["iob"]["iob"].as<float>();
        else if (doc["iob"].is<float>()) newIob = doc["iob"].as<float>();
        if (doc["cob"]["display"].is<float>() || doc["cob"]["display"].is<int>())
          newCob = doc["cob"]["display"].as<float>();
        else if (doc["cob"]["cob"].is<float>() || doc["cob"]["cob"].is<int>())
          newCob = doc["cob"]["cob"].as<float>();
        else if (doc["cob"].is<float>()) newCob = doc["cob"].as<float>();
      }
    }
    http.end();
  }

  // 2) Bolus — treatments (filtre yok, insulin/amount)
  {
    HTTPClient http;
    String url = base + "/api/v1/treatments.json?count=50";
    url = nsTok(url);
    if (url.startsWith("https")) { http.begin(nsSecure(), url); }
    else http.begin(url);
    http.setTimeout(8000);
    nsAuth(http);
    int code = netGET(http);
    if (code == 200) {
      String payload = netBody(http);
      StaticJsonDocument<256> tf;
      tf[0]["insulin"] = true; tf[0]["amount"] = true; tf[0]["date"] = true; tf[0]["created_at"] = true;
      tf[0]["timestamp"] = true; tf[0]["eventType"] = true; tf[0]["rate"] = true; tf[0]["absolute"] = true;
      DynamicJsonDocument doc(24576);
      if (!jsonParseF(doc, payload, tf) && doc.is<JsonArray>()) {
        // API genelde yeniden eskiye — 24s filtre yok, son 5 insulin>0
        for (JsonObject o : doc.as<JsonArray>()) {
          String et0 = o["eventType"] | "";
          if (et0.indexOf("Temp") >= 0 || et0.indexOf("Basal") >= 0 || !o["rate"].isNull() || !o["absolute"].isNull()) continue;
          float ins = 0;
          if (!o["insulin"].isNull()) ins = o["insulin"].as<float>();
          else if (!o["amount"].isNull() && et0.indexOf("Bolus") >= 0) ins = o["amount"].as<float>();
          if (ins <= 0.01f) continue;
          uint32_t ts = 0;
          if (!o["date"].isNull()) {
            uint64_t d = o["date"].as<uint64_t>();
            if (d > 100000000000ULL) ts = (uint32_t)(d / 1000ULL);
            else if (d > 1000000000ULL) ts = (uint32_t)d;
          }
          if (ts == 0 && o["created_at"].is<const char*>())
            ts = parseIsoToUnix(o["created_at"].as<const char*>());
          if (ts == 0 && o["timestamp"].is<const char*>())
            ts = parseIsoToUnix(o["timestamp"].as<const char*>());
          if (newBolus >= BOLUS_MAX) break;
          tmpBol[newBolus].ts = ts;
          tmpBol[newBolus].units = ins;
          tmpBol[newBolus].note[0] = 0;
          const char* et = o["eventType"] | "Bolus";
          strncpy(tmpBol[newBolus].note, et, 11);
          tmpBol[newBolus].note[11] = 0;
          newBolus++;
        }
      }
    }
    http.end();
  }

  if (newIob >= 0) { nsIob = newIob; nsIobOkMs = millis(); }
  if (newCob >= 0) { nsCob = newCob; nsCobOkMs = millis(); }
  if (newBolus > 0) {
    bolusCount = newBolus;
    for (int i = 0; i < newBolus; i++) boluses[i] = tmpBol[i];
  }
}


uint32_t fetchErrSeq = 0;
void setFetchError(int code, const String& detail) {
  fetchErrSeq++;
  lastErrorCode = code;
  lastErrorDetail = detail;
}

void clearFetchError() {
  lastErrorCode = 0;
  lastErrorDetail = "";
}

const char* errorTitle() {
  switch (lastErrorCode) {
    case 1: return L("WiFi bağlı değil", "WiFi not connected");
    case 2: return L("Giriş başarısız", "Login failed");
    case 3: return netNoInternet ? L("İnternet yok", "No internet") : L("Sunucu yanıt vermiyor", "Server not responding");
    case 4: return L("Veri gelmiyor", "No data");
    case 5: return L("Kaynak ayarı eksik", "Source not configured");
    case 6: return L("Veri okunamadı", "Could not read data");
    default: return L("Bir sorun oluştu", "Something went wrong");
  }
}

const char* errorHint() {
  switch (lastErrorCode) {
    case 1: return L("Ayni WiFi agini kontrol edin. BOOT 2sn.", "Check WiFi. Hold BOOT 2s.");
    case 2: return L("Kullanici veya sifre yanlis olabilir.", "User or password may be wrong.");
    case 3: return L("Internet veya kaynak adresini kontrol edin.", "Check internet or source URL.");
    case 4: return L("CGM henuz olcum gondermemis olabilir.", "CGM may not have sent data yet.");
    case 5: return L("QR / Web veya BOOT ile kaynak girin.", "Enter source via QR/Web or BOOT.");
    case 6: return L("Kaynak yaniti beklenmedik. Tekrar denenecek.", "Unexpected response. Will retry.");
    default: return L("Biraz sonra yeniden denenecek.", "Will retry shortly.");
  }
}

int errShownKey = -1;                         // 3.4.28: hata ekraninda gosterilen durum
void drawErrorScreenFull() {
  // 4.0: bolunmus ekranda hata, kisinin satirinda gosterilir (tam ekran hata yok)
  if (personCount > 1 && famMode == 1 && isHomeScreen() && WiFi.status() == WL_CONNECTED) {
    showErrorScreen = false;
    drawCurrentScreen();
    return;
  }
  errShownKey = lastErrorCode * 2 + (netNoInternet ? 1 : 0);
  applyRotation();
  cv = &tft;
  cv->fillScreen(UI_BG);
  cv->fillRect(0, 0, 320, 24, 0x18C3);
  cv->setTextColor(TFT_CYAN, 0x18C3);
  cv->setTextDatum(TC_DATUM);
  famPillW = 0;
  if (personCount > 1) drawPersonPill(160, 1);      // 4.0: kimin kaynaginda hata
  else dsN(cv, "MeteMatik", 160, 4, 2);

  cv->setTextColor(lastErrorCode == 1 || lastErrorCode == 2 ? sevColor(1) : TFT_WHITE, UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(errorTitle(), 160, 42, &FreeSansTR9pt);

  int hy = 70;
  String sub;
  if (lastErrorCode == 1) sub = String(L("Ağ: ", "Network: ")) + savedWifiName();
  else if (lastErrorCode == 2 || lastErrorCode == 5 || lastErrorCode == 4) sub = String(L("Kaynak: ", "Source: ")) + sourceLabel();
  else if (lastErrorCode == 3 || lastErrorCode == 6) {
    sub = sourceLabel();
    int hp = lastErrorDetail.indexOf("HTTP");
    if (hp >= 0) sub += "  (" + lastErrorDetail.substring(hp) + ")";
  }
  if (sub.length()) {
    if (sub.length() > 34) sub = sub.substring(0, 34) + "..";
    cv->setTextColor(TFT_CYAN, UI_BG);
    drawFreeTitle(sub.c_str(), 160, hy, &FreeSansTR7pt);
    hy += 22;
  }

  // Aciklama: Turkce font, kelime kaydirarak en fazla 3 satir
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  String h = errorHintLong();
  cv->setFreeFont(&FreeSansTR7pt);
  int lines = 0;
  while (h.length() && lines < 3) {
    int cut = h.length();
    while (cut > 0 && cv->textWidth(h.substring(0, cut)) > 300) {
      int sp = h.lastIndexOf(' ', cut - 1);
      cut = (sp > 0) ? sp : cut - 1;
    }
    drawFreeTitle(h.substring(0, cut).c_str(), 160, hy, &FreeSansTR7pt);
    h = h.substring(cut); h.trim();
    hy += 19; lines++;
  }

  // Butonlar: Yenile | Kurulum | Geri
  const int by = 150, bh = 56, bw = 96;
  const int bx[3] = {8, 112, 216};
  const uint16_t bc[3] = {0x2A6E, 0x140E, 0x3186};
  for (int i = 0; i < 3; i++) cv->fillRoundRect(bx[i], by, bw, bh, 10, bc[i]);
  cv->setTextDatum(MC_DATUM);
  cv->setTextColor(TFT_WHITE, bc[0]);
  drawFreeTitle(L("Yenile", "Retry"), bx[0] + bw / 2, by + bh / 2, &FreeSansTR9pt);
  cv->setTextColor(TFT_WHITE, bc[1]);
  drawFreeTitle(L("Kurulum", "Setup"), bx[1] + bw / 2, by + 20, &FreeSansTR9pt);
  drawFreeTitle(L("WiFi / giriş", "WiFi / login"), bx[1] + bw / 2, by + 40, &FreeSansTR6pt);
  cv->setTextColor(TFT_WHITE, bc[2]);
  {
    cv->setFreeFont(&FreeSansTR9pt);
    int tw = cv->textWidth(L("Geri", "Back"));
    int gx = bx[2] + bw / 2 - (10 + 8 + tw) / 2;
    drawBackIcon(gx + 5, by + bh / 2, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Geri", "Back"), gx + 18, by + bh / 2, &FreeSansTR9pt);
  }
  cv->setTextDatum(MC_DATUM);
  cv->setTextColor(0x8410, UI_BG);
  drawFreeTitle(L("Kurulumda kayıtlı ağlar silinmez - BOOT 2 sn = Kurulum", "Setup keeps saved networks - BOOT 2 s = Setup"), 160, 223, &FreeSansTR6pt);
  cv->setTextDatum(TL_DATUM);
  cv->setTextFont(1);
}

void drawWifiBanner() {
  cv->fillRect(0, 0, 320, 22, 0xB000);
  cv->setTextColor(TFT_WHITE, 0xB000);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("WiFi koptu - yeniden bağlanılıyor", "WiFi lost - reconnecting"), 160, 4, &FreeSansTR6pt);
}

// 3.4.24: pille calisiyor mu (pil olceri var, pil takili, sarjda degil)
bool onBatteryPower() { return inaPresent && batteryPercent >= 0 && !batteryCharging; }

// 3.4.24: pille calisirken hafif cekme - sadece son 4 kayit (~0,5 KB, 24 saat yerine).
// 1 = tamam, 0 = hata (hata yazildi), -1 = tam cekme gerekli (bosluk / beklenmeyen yanit)
int fetchNightscoutLight() {
  if (historyCount < 10 || historyTsSec[historyCount - 1] == 0) return -1;
  StaticJsonDocument<128> filter;
  filter[0]["sgv"] = true; filter[0]["date"] = true; filter[0]["direction"] = true;
  HTTPClient http;
  String url = nightscoutUrl;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/api/v1/entries/sgv.json?count=4";
  url = nsTok(url);
  if (url.startsWith("https")) http.begin(nsSecure(), url); else http.begin(url);
  http.setTimeout(12000);
  http.setConnectTimeout(6000);
  nsAuth(http);
  int code = netGET(http);
  if (code == 401 || code == 403) { http.end(); setFetchError(2, "HTTP " + String(code)); return 0; }
  if (code != 200) { http.end(); setFetchError(3, "HTTP " + String(code)); return 0; }
  String payload = netBody(http);
  http.end();
  if (payload.length() < 8) return -1;
  DynamicJsonDocument doc(3072);
  if (jsonParseF(doc, payload, filter) || !doc.is<JsonArray>() || doc.size() == 0) return -1;
  int k = doc.size();
  uint32_t lastTs = historyTsSec[historyCount - 1];
  uint32_t ts[4]; int nNew = 0;
  for (int i = 0; i < k && i < 4; i++) {
    uint64_t dr = doc[i]["date"].as<uint64_t>();
    uint64_t dm = (dr > 100000000000ULL) ? dr : (dr > 1000000000ULL ? dr * 1000ULL : 0);
    ts[i] = (uint32_t)(dm / 1000ULL);
    if (ts[i] == 0) return -1;
    if (ts[i] > lastTs + 30) nNew++;
  }
  if (k > 4) k = 4;
  if (nNew == k) return -1;                               // eski kayitla ortusme yok: arada bosluk olabilir
  JsonObject latest = doc[0];
  if ((int)(latest["sgv"] | 0) <= 20) { setFetchError(4, "SGV"); return 0; }
  sgv = latest["sgv"] | 0;
  direction = latest["direction"] | "Flat";
  delta = (k > 1) ? (sgv - (int)(doc[1]["sgv"] | sgv)) : 0;
  markNewReading((uint64_t)ts[0] * 1000ULL, "");
  trackLocalReading(sgv, ts[0], lastLocalAppendTs);
  for (int i = k - 1; i >= 0; i--) {                      // yeni kayitlari eskiden yeniye ekle
    if (ts[i] <= lastTs + 30) continue;
    if (historyCount >= HIST_MAX) {
      memmove(history, history + 1, (HIST_MAX - 1) * sizeof(history[0]));
      memmove(historyTsSec, historyTsSec + 1, (HIST_MAX - 1) * sizeof(historyTsSec[0]));
      memmove(historyDay, historyDay + 1, (HIST_MAX - 1) * sizeof(historyDay[0]));
      historyCount = HIST_MAX - 1;
    }
    history[historyCount] = doc[i]["sgv"] | 0;
    historyTsSec[historyCount] = ts[i];
    historyDay[historyCount] = dayStampFromMs((uint64_t)ts[i] * 1000ULL);
    historyCount++;
  }
  delta = deltaFromHistory(delta);
  checkAgeWarnings();
  fetchNsIobCobBolus(false);
  clearFetchError();
  return 1;
}

bool fetchNightscout() {
  // 3.4.24: pille calisirken 30 dk'da bir tam (24 saat), arada sadece son kayitlar
  if (onBatteryPower() && nsFullOkMs != 0 && millis() - nsFullOkMs < 1800000UL) {
    int r = fetchNightscoutLight();
    if (r >= 0) return r == 1;
  }
  // NS: once count=288 dene; bos/JSON hata → count=200 (eski stabil yol)
  // Dexcom ayri: maxCount=288 kalir
  StaticJsonDocument<128> filter;
  filter[0]["sgv"] = true;
  filter[0]["date"] = true;
  filter[0]["direction"] = true;

  String payload;
  int usedCount = 0;

  for (int attempt = 0; attempt < 2; attempt++) {
    int want = (attempt == 0) ? 288 : 200;
    HTTPClient http;
    String url = nightscoutUrl;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    url += "/api/v1/entries/sgv.json?count=" + String(want);   // 3.4.20: sadece sensor (parmak/kalibrasyon kaydi degil)
    url = nsTok(url);
    if (url.startsWith("https")) {
      http.begin(nsSecure(), url);
    } else {
      http.begin(url);
    }
    http.setTimeout(12000);
    http.setConnectTimeout(6000);
    nsAuth(http);
    int nsCode = netGET(http);
    if (nsCode == 401 || nsCode == 403) {
      http.end();
      setFetchError(2, "HTTP " + String(nsCode));
      return false;
    }
    if (nsCode != 200) {
      http.end();
      if (attempt == 0 && nsCode > 0) continue; // 288 HTTP hata → 200 dene (baglanti yoksa tekrar deneme)
      setFetchError(3, "HTTP " + String(nsCode));
      return false;
    }
    payload = netBody(http);
    http.end();
    if (payload.length() < 8) {
      if (attempt == 0) continue; // EmptyInput onlemi
      setFetchError(6, "Empty body");
      return false;
    }
    // Parse — 288 icin orta buffer (cift buyuk doc EmptyInput yapabiliyordu)
    size_t cap = (want >= 288) ? 36864 : 28672;
    DynamicJsonDocument doc(cap);
    DeserializationError jerr = jsonParseF(doc, payload, filter);
    if (jerr) {
      if (attempt == 0) continue;
      setFetchError(6, String(jerr.c_str()));
      return false;
    }
    if (!doc.is<JsonArray>() || doc.size() == 0) {
      if (attempt == 0) continue;
      setFetchError(4, "Bos liste");
      return false;
    }

    JsonObject latest = doc[0];
    if ((int)(latest["sgv"] | 0) <= 20) { setFetchError(4, "SGV"); return false; }   // 3.4.0: once kontrol
    sgv = latest["sgv"] | 0;
    direction = latest["direction"] | "Flat";
    delta = (doc.size() > 1) ? (sgv - (int)(doc[1]["sgv"] | sgv)) : 0;
    uint64_t dateRaw = latest["date"].as<uint64_t>();
    uint64_t dateMs = 0;
    if (dateRaw > 100000000000ULL) dateMs = dateRaw;
    else if (dateRaw > 1000000000ULL) dateMs = dateRaw * 1000ULL;
    markNewReading(dateMs, "");
    trackLocalReading(sgv, (dateMs > 0) ? (uint32_t)(dateMs / 1000ULL) : (uint32_t)time(nullptr), lastLocalAppendTs);
    int n = min((int)doc.size(), HIST_MAX);
    historyCount = n;
    usedCount = n;
    nsUsedFallback200 = (attempt == 1);
    for (int i = 0; i < n; i++) {
      history[i] = doc[n - 1 - i]["sgv"] | 0;
      uint64_t dr = doc[n - 1 - i]["date"].as<uint64_t>();
      uint64_t dm = 0;
      if (dr > 100000000000ULL) dm = dr;
      else if (dr > 1000000000ULL) dm = dr * 1000ULL;
      historyDay[i] = dayStampFromMs(dm);
      historyTsSec[i] = (dm > 0) ? (uint32_t)(dm / 1000ULL) : 0;
    }
    delta = deltaFromHistory(delta);      // 3.4.3
    // NS normalde 288/24s doner ama 288 istegi basarisiz olup 200'e (16.6s)
    // dusuldugunde localHist'teki cihaz-tarafli birikim kalan bosluğu doldurur.
    mergeLocalHistory();
    fetchSageCage();
    checkAgeWarnings();
    fetchNsIobCobBolus(false);
    if (sgv <= 20) { setFetchError(4, "SGV"); return false; }
    clearFetchError();
    nsFullOkMs = millis();
    (void)usedCount;
    return true;
  }
  setFetchError(6, "NS parse");
  return false;
}

extern const uint8_t crtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t crtBundleEnd[] asm("_binary_x509_crt_bundle_end");
WiFiClientSecure caClient;
// Sabit sunucular (Dexcom, Libre, Medtrum): sunucu sertifikasi Mozilla kok listesiyle dogrulanir.
// Saat henuz ayarli degilse (acilista NTP gelmeden) sertifika tarihi kontrol edilemez: o tek
// deneme dogrulamasiz yapilir.
bool caBundleSet = false;
void caBundleOnce() {
  if (!caBundleSet) { caClient.setCACertBundle(crtBundleStart, crtBundleEnd - crtBundleStart); caBundleSet = true; }
}

// 3.4.9: Nightscout da Mozilla kok listesiyle dogrulanir (saat ayarli degilse o tek deneme dogrulamasiz)
WiFiClientSecure& nsSecure() {
  String key = String("ns|") + nightscoutUrl;
  if (time(nullptr) < 1600000000) {
    secureFor(secureClient, secKey, String("ins|") + key);
    secureClient.setInsecure(); secureClient.setHandshakeTimeout(12); return secureClient;
  }
  caBundleOnce();
  secureFor(caClient, caKey, key);
  caClient.setHandshakeTimeout(12);
  return caClient;
}

WiFiClientSecure& caReady() {
  // Not: cekirdekte setInsecure() bir kez cagrilinca istemci kalici olarak dogrulamasiz kalir
  // (setCACertBundle bunu geri almaz). O yuzden iki ayri istemci: saat yokken eski
  // dogrulamasiz istemci, saat varken yalnizca sertifika listeli istemci.
  String key = dataSource + "|" + libreRegion + "|" + dexServer;
  if (time(nullptr) < 1600000000) {
    secureFor(secureClient, secKey, String("ins|") + key);
    secureClient.setInsecure(); secureClient.setHandshakeTimeout(12); return secureClient;
  }
  caBundleOnce();
  secureFor(caClient, caKey, key);
  caClient.setHandshakeTimeout(12);
  return caClient;
}

String dexBase() {
  return (dexServer == "us") ? "https://share1.dexcom.com" : "https://shareous1.dexcom.com";
}

bool dexGetAccountId() {
  HTTPClient http;
  
  http.begin(caReady(), dexBase() + "/ShareWebServices/Services/General/AuthenticatePublisherAccount");
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  DynamicJsonDocument body(256);
  body["accountName"] = normDexUser(dexUser);
  body["password"] = dexPass;
  body["applicationId"] = "d89443d2-327c-4a6f-89e5-496bbb0317db";
  String bodyStr; serializeJson(body, bodyStr);
  int code = netPOST(http, bodyStr);
  String resp = netBody(http);
  http.end();
  dexLastCode = code;
  if (code != 200 || resp.length() < 5) return false;
  dexAccountId = resp; dexAccountId.replace("\"", ""); dexAccountId.trim();
  return dexAccountId.length() > 8;
}

bool dexGetSessionId() {
  HTTPClient http;
  
  http.begin(caReady(), dexBase() + "/ShareWebServices/Services/General/LoginPublisherAccountById");
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  DynamicJsonDocument body(256);
  body["accountId"] = dexAccountId;
  body["password"] = dexPass;
  body["applicationId"] = "d89443d2-327c-4a6f-89e5-496bbb0317db";
  String bodyStr; serializeJson(body, bodyStr);
  int code = netPOST(http, bodyStr);
  String resp = netBody(http);
  http.end();
  dexLastCode = code;
  if (code != 200) { dexAccountId = ""; return false; }
  dexSessionId = resp; dexSessionId.replace("\"", ""); dexSessionId.trim();
  return dexSessionId.length() > 8;
}

bool fetchDexcom() {
  if (dexUser.length() == 0 || dexPass.length() == 0) { setFetchError(5, "Dexcom kullanici/sifre"); return false; }
  if (dexAccountId.length() == 0 && !dexGetAccountId()) {
    if (dexLastCode <= 0) setFetchError(3, "Dexcom baglanti");
    else setFetchError(2, "Dexcom giris " + String(dexLastCode));
    return false;
  }
  if (dexSessionId.length() == 0 && !dexGetSessionId()) {
    if (dexLastCode <= 0) setFetchError(3, "Dexcom baglanti");
    else setFetchError(2, "Dexcom oturum " + String(dexLastCode));
    return false;
  }

  HTTPClient http;
  String url = dexBase() + "/ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues";
  // 24s hedef: maxCount=288 (API bazen daha az doner; 200'e zorla dusme yok)
  url += "?sessionId=" + dexSessionId + "&minutes=1440&maxCount=288";
  
  http.begin(caReady(), url);
  http.setTimeout(15000);
  http.addHeader("Accept", "application/json");
  int code = netGET(http);
  String payload = netBody(http);
  http.end();

  if (code == 500 && payload.indexOf("Session") >= 0) {
    dexSessionId = "";
    static int dexDepth = 0;
    if (dexDepth > 0) { setFetchError(2, "Dexcom oturum"); return false; }
    dexDepth++; bool r = fetchDexcom(); dexDepth--;
    return r;
  }
  if (code == 401 || code == 403) {
    dexSessionId = "";
    dexAccountId = "";
    setFetchError(2, "Dexcom yetki");
    return false;
  }
  if (code != 200) {
    setFetchError(3, "Dexcom HTTP " + String(code));
    return false;
  }

  // Onemli: Nightscout tarafi gibi FILTRE kullanmiyorduk - Dexcom'un
  // yanitindaki her kayit "WT"/"ST"/"DT" gibi uzun "/Date(...)/" metinleri
  // + Value/Trend iceriyor, hicbiri filtrelenmeden ayristirilinca (288
  // kayit icin) DynamicJsonDocument(72000) çoğu zaman YETMIYORDU - bu da
  // deserializeJson()'in sessizce hata donmesine, ve kodun bunu "CGM veri
  // gondermemis" (Veri gelmiyor) diye yanlis yorumlamasina yol aciyordu,
  // oysa telefonda veri akiyor ve giris basariliydi (kullanicinin bildirdigi
  // durum tam olarak buydu). Sadece kullandigimiz 3 alani (Value/Trend/ST)
  // tutan bir filtre ile, tipki fetchNightscout()'taki gibi, gereksiz
  // WT/DT metinleri hic ayristirilmiyor ve arabellek 288 kayit icin de
  // rahatlikla yetiyor.
  StaticJsonDocument<128> filter;
  filter[0]["Value"] = true;
  filter[0]["Trend"] = true;
  filter[0]["ST"] = true;

  DynamicJsonDocument doc(40960);
  DeserializationError jerr = jsonParseF(doc, payload, filter);
  if (jerr || !doc.is<JsonArray>() || doc.size() == 0) {
    setFetchError(4, "Dexcom JSON/veri");
    return false;
  }

  int n = min((int)doc.size(), HIST_MAX);
  historyCount = n;
  for (int i = 0; i < n; i++) {
    history[i] = doc[n - 1 - i]["Value"] | 0;
    String st = doc[n - 1 - i]["ST"] | "";
    uint64_t dateMs = 0;
    int a = st.indexOf('('), b = st.indexOf(')');
    if (a >= 0 && b > a) {
      String num = st.substring(a + 1, b);
      int dash = num.indexOf('-'); if (dash > 0) num = num.substring(0, dash);
      int plus = num.indexOf('+'); if (plus > 0) num = num.substring(0, plus);
      dateMs = strtoull(num.c_str(), NULL, 10);
    }
    historyDay[i] = dayStampFromMs(dateMs);
    historyTsSec[i] = (dateMs > 0) ? (uint32_t)(dateMs / 1000ULL) : 0;
  }

  JsonObject latest = doc[0];
  sgv = latest["Value"] | 0;
  direction = latest["Trend"] | "Flat";
  delta = (n > 1) ? (sgv - (int)(doc[1]["Value"] | sgv)) : 0;
  delta = deltaFromHistory(delta);        // 3.4.3

  String st = latest["ST"] | "";
  uint64_t dateMs = 0;
  int a = st.indexOf('('), b = st.indexOf(')');
  if (a >= 0 && b > a) {
    String num = st.substring(a + 1, b);
    int dash = num.indexOf('-'); if (dash > 0) num = num.substring(0, dash);
    int plus = num.indexOf('+'); if (plus > 0) num = num.substring(0, plus);
    dateMs = strtoull(num.c_str(), NULL, 10);
  }
  markNewReading(dateMs, String(dateMs));
  sensorAgeHours = -1; cannulaAgeHours = -1;
  nsIob = -1; nsCob = -1; bolusCount = 0; graphDetailOpen = false;
  if (sgv <= 20) { setFetchError(4, "Dexcom SGV"); return false; }
  clearFetchError();
  return true;
}

String libreBase() {
  String r = libreRegion; r.toUpperCase();
  if (r == "US") return "https://api-us.libreview.io";
  if (r == "DE") return "https://api-de.libreview.io";
  if (r == "EU2") return "https://api-eu2.libreview.io";
  if (r == "AP") return "https://api-ap.libreview.io";
  if (r == "AU") return "https://api-au.libreview.io";
  if (r == "CA") return "https://api-ca.libreview.io";
  if (r == "FR") return "https://api-fr.libreview.io";
  if (r == "JP") return "https://api-jp.libreview.io";
  return "https://api-eu.libreview.io";
}

void libreAddHeaders(HTTPClient& http, bool auth) {
  http.addHeader("Content-Type", "application/json");
  http.addHeader("product", "llu.ios");
  http.addHeader("version", "4.16.0");
  http.addHeader("User-Agent", "Mozilla/5.0 (iPhone; CPU OS 17_4 like Mac OS X) AppleWebKit/605.1.15");
  if (auth && libreToken.length()) {
    http.addHeader("Authorization", "Bearer " + libreToken);
    http.addHeader("Account-Id", sha256Hex(libreAccountId));
  }
}

bool libreLogin() {
  HTTPClient http;
  
  http.begin(caReady(), libreBase() + "/llu/auth/login");
  http.setTimeout(15000);
  libreAddHeaders(http, false);
  StaticJsonDocument<512> bj; bj["email"] = libreEmail; bj["password"] = librePass;
  String body; serializeJson(bj, body);
  int code = netPOST(http, body);
  String resp = netBody(http);
  http.end();
  libreLastCode = code; libreLastStatus = -1;
  if (code != 200) return false;
  DynamicJsonDocument doc(16384);                   // 3.4.20: giris cevabi (kullanici bilgisi + jeton) 4 KB'i asabiliyor
  if (jsonParse(doc, resp)) return false;
  libreLastStatus = doc["status"].as<int>();
  if (libreLastStatus != 0) return false;
  if (doc["data"]["redirect"].as<bool>()) {
    String reg = doc["data"]["region"] | "EU";
    reg.toUpperCase(); libreRegion = reg; saveSettings();
    static int lrDepth = 0;
    if (lrDepth > 0) return false;                       // tek yonlendirme
    lrDepth++; bool r = libreLogin(); lrDepth--;
    return r;
  }
  libreToken = doc["data"]["authTicket"]["token"] | "";
  libreAccountId = doc["data"]["user"]["id"] | "";
  return libreToken.length() > 10;
}

// LibreLinkUp "FactoryTimestamp" (UTC): "9/29/2026 8:12:34 AM" -> unix sn (0 = okunamadi)
uint32_t libreParseTs(const char* s) {
  if (!s) return 0;
  int mo = 0, d = 0, y = 0, h = 0, mi = 0, se = 0; char ap[3] = {0};
  int n = sscanf(s, "%d/%d/%d %d:%d:%d %2s", &mo, &d, &y, &h, &mi, &se, ap);
  if (n < 6 || y < 2020 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  if (n == 7) { if ((ap[0] == 'P' || ap[0] == 'p') && h < 12) h += 12; if ((ap[0] == 'A' || ap[0] == 'a') && h == 12) h = 0; }
  // gunler (1970'ten) - civil_from_days tersi
  int yy = y - (mo <= 2 ? 1 : 0);
  int era = (yy >= 0 ? yy : yy - 399) / 400;
  unsigned yoe = (unsigned)(yy - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + se);
}

bool fetchLibre() {
  if (libreEmail.length() == 0 || librePass.length() == 0) {
    setFetchError(5, "Libre e-posta/sifre");
    return false;
  }
  if (libreToken.length() == 0 && !libreLogin()) { libreLoginError(); return false; }
  HTTPClient http;
  
  http.begin(caReady(), libreBase() + "/llu/connections");
  http.setTimeout(15000);
  libreAddHeaders(http, true);
  int code = netGET(http);
  String resp = netBody(http);
  http.end();
  if (code == 401) {
    libreToken = "";
    if (!libreLogin()) { libreLoginError(); return false; }
    static int lfDepth = 0;
    if (lfDepth > 0) { setFetchError(2, "Libre oturum"); return false; }
    lfDepth++; bool r = fetchLibre(); lfDepth--;
    return r;
  }
  if (code != 200) { setFetchError(3, "Libre HTTP " + String(code)); return false; }
  DynamicJsonDocument doc(32768);                   // 3.4.20: birden fazla kisi takip edilince 8 KB yetmiyordu
  if (jsonParse(doc, resp)) { setFetchError(6, "Libre JSON"); return false; }
  if (doc["status"].as<int>() != 0 || !doc["data"].is<JsonArray>() || doc["data"].size() == 0) {
    setFetchError(4, "Libre baglanti yok");
    return false;
  }
  JsonObject patient = doc["data"][0];
  librePatientId = patient["patientId"] | "";
  // 3.4.0: grafik once indirilir (sunucu beklenirken kilit serbest); ekrandaki degerler
  // ondan sonra, arada ag beklemesi olmadan yazilir (yarim veri gorunmesin)
  String libGraph; bool libGraphOk = false;
  if (librePatientId.length()) {
    HTTPClient http2;
    http2.begin(caReady(), libreBase() + "/llu/connections/" + librePatientId + "/graph");
    http2.setTimeout(15000);
    libreAddHeaders(http2, true);
    if (netGET(http2) == 200) { libGraph = netBody(http2); libGraphOk = true; }
    http2.end();
  }
  DynamicJsonDocument gdoc(libGraphOk ? 49152 : 16);
  bool gOk = libGraphOk && !jsonParse(gdoc, libGraph) && gdoc["data"]["graphData"].is<JsonArray>();
  libGraph = String();
  JsonObject gm = patient["glucoseMeasurement"];
  sgv = gm["ValueInMgPerDl"] | 0;
  uint32_t libTs = libreParseTs(gm["FactoryTimestamp"] | (const char*)nullptr);
  int curSgv = sgv;
  int trend = gm["TrendArrow"] | 3;
  if (trend == 5) direction = "SingleUp";
  else if (trend == 4) direction = "FortyFiveUp";
  else if (trend == 3) direction = "Flat";
  else if (trend == 2) direction = "FortyFiveDown";
  else if (trend == 1) direction = "SingleDown";
  else direction = "Flat";
  delta = 0; deltaInterp = false; deltaUnknown = false;
  if (!gOk && historyCount == 0) { historyCount = 1; history[0] = sgv; historyDay[0] = localDayStamp(); historyTsSec[0] = (uint32_t)time(nullptr); }
  sensorAgeHours = -1; cannulaAgeHours = -1;
  nsIob = -1; nsCob = -1; bolusCount = 0; graphDetailOpen = false;
  if (gOk) {
    {
      {
        JsonArray arr = gdoc["data"]["graphData"].as<JsonArray>();
        int n = min((int)arr.size(), HIST_MAX);
        historyCount = n;
        uint32_t today = localDayStamp();
        uint32_t nowSec = (uint32_t)time(nullptr);
        // Libre graph: gercek timestamp varsa kullan, yoksa ~15dk aralik
        for (int i = 0; i < n; i++) {
          history[i] = arr[i]["ValueInMgPerDl"] | 0;
          uint32_t ts = 0;
          if (arr[i]["Timestamp"].is<const char*>()) {
            // "M/D/Y H:M:S AM" veya benzeri — parse basarisizsa tahmine dus
            ts = 0;
          }
          if (arr[i]["FactoryTimestamp"].is<const char*>())
            ts = libreParseTs(arr[i]["FactoryTimestamp"].as<const char*>());   // 3.4.3: gercek zaman
          if (ts == 0) {
            // API cogu zaman 15dk ornekler; sondan geriye
            ts = (nowSec > (uint32_t)((n - 1 - i) * 900)) ? nowSec - (n - 1 - i) * 900 : 0;
          }
          historyTsSec[i] = ts;
          historyDay[i] = (ts > 0) ? dayStampFromMs((uint64_t)ts * 1000ULL) : today;
        }
        // grafik noktalari ~15 dk arali: guncel degeri EZME (eskiden son grafik noktasi yaziliyordu)
      }
    }
  }
  sgv = curSgv;
  // Olcum zamani: yeni olcumde alarm kontrolu (requestAlarmBurst) ve gercek yas
  if (sgv > 20) {
    uint32_t nowSec2 = (uint32_t)time(nullptr);
    // delta: ~5 dk onceki guncel olcume gore (son 10 olcum tutulur)
    uint32_t* rTs = lrTs; int16_t* rV = lrV; int& rN = lrN;   // 4.0: kisi basina
    if (libreRingReset) { rN = 0; libreRingReset = false; }
    uint32_t tsKey = libTs ? libTs : nowSec2;
    bool& lastKeyTs = lrKeyTs;
    if ((libTs != 0) != lastKeyTs) { rN = 0; lastKeyTs = (libTs != 0); }   // zaman kaynagi degisti: halkayi sifirla
    bool isNewTs = (rN == 0 || tsKey > rTs[(rN - 1) % 10]);
    if (libTs == 0) isNewTs = (lastLibreSgv < 0 || sgv != lastLibreSgv);   // zaman okunamazsa eski yontem
    if (isNewTs) { rTs[rN % 10] = tsKey; rV[rN % 10] = sgv; rN++; }
    delta = 0; deltaInterp = false; deltaUnknown = false;
    for (int k = rN - 2; k >= 0 && k >= rN - 10; k--) {
      uint32_t dt = tsKey - rTs[k % 10];
      if (dt >= 240) { delta = deltaGap(sgv - rV[k % 10], dt); break; }   // 3.6.1: kesintide 5 dk'ya cevir / bilinmiyor
    }
    unsigned long& lastLibBurst = lrLastBurst;
    int& lastLibKind = lrLastKind;
    if (libBurstReset) { lastLibBurst = 0; lastLibKind = -1; libBurstReset = false; }
    bool fresh = false;
    if (libTs) {
      fresh = markNewReading((uint64_t)libTs * 1000ULL, String(libTs));
      if (fresh) trackLocalReading(sgv, libTs, lastLocalAppendTs);     // 3.4.20: olcum zamaniyla
    } else if (isNewTs) {
      fresh = markNewReading((uint64_t)nowSec2 * 1000ULL, String(nowSec2));
      trackLocalReading(sgv, nowSec2, lastLocalAppendTs);
    }
    if (fresh) {
      int kNow = getAlarmKind();
      // 3.4.20: ~4,5 dk bekleme sadece GERCEKTEN ses calan alarmla baslar. Susturulan olcum (ses bastirma,
      // erteleme, sessiz/gece modu, o alarmin sesi kapali) beklemeyi baslatmaz - dusus baslayinca hemen calar.
      bool audible = kNow > 0 && isAlarmCondition() && effectiveSoundLevel() > 0 &&
                     alarmSoundEnabledForKind(kNow) && !isRecovering(kNow);
      if (kNow != lastLibKind) { lastLibKind = kNow; lastLibBurst = audible ? millis() : 0; }   // tur degisti: hemen uyar
      else if (lastLibBurst != 0 && millis() - lastLibBurst < 270000UL) requestAlarmBurst = false;
      else lastLibBurst = audible ? millis() : 0;
    }
    lastLibreSgv = sgv;
  }
  if (sgv <= 20) { setFetchError(4, "Libre SGV"); return false; }
  // Cihazin kendi biriktirdigi 24 saatlik gunluk (localHist) ile API'den
  // gelen (genelde daha kisa sureli) grafigi birlestir - amac zamanla
  // (cihaz ne kadar suredir acik/baglıysa) 24 saate/288 olcume yaklasmak.
  mergeLocalHistory();
  clearFetchError();
  return true;
}

// 3.4.0: basarisiz cekmede son gecerli olcum ekranda kalir (kismen yazilanlar geri alinir).
// Eskiligine yasa gore karar verilir: "veri yok" alarmi ve gri deger ayarlanan sureden sonra.
static int bkHist[HIST_MAX];
static uint32_t bkDay[HIST_MAX], bkTs[HIST_MAX];
bool fetchData() {
  wdtFeed();
  { String& lastSrc = ctxLastSrc;          // 4.0: kisi basina
    if (lastSrc != dataSource) {
      if (lastSrc.length()) {                 // kaynak degisti: eski kaynagin degeri kalmasin
        clearSourceData();
      }
      historyCount = 0; lastSrc = dataSource;
    } }
  if (WiFi.status() != WL_CONNECTED) {
    setFetchError(1, savedWifiName());
    consecutiveFetchFails++;
    return false;
  }
  // 3.4.8: internet yokken her turda kaynagi (uzun zaman asimlariyla) deneme; kisa kontrol yeter.
  // Dakikada bir yine de tam deneme (kontrol adresleri engelli aglar icin).
  if (netNoInternet && millis() - netFullTry < 60000UL) {
    if (!internetProbe()) {
      consecutiveFetchFails++;
      setFetchError(3, L("İnternet yok", "No internet"));
      lastFetch = millis();
      updateAge();
      return false;
    }
    netNoInternet = false;
  }
  // NS config
  if (dataSource == "nightscout") {
    String u = nightscoutUrl;
    u.trim();
    if (u.length() < 8 || u == "https://") {
      setFetchError(5, "Nightscout URL");
      dataValid = false;
      return false;
    }
  }
  uint32_t errSeq0 = fetchErrSeq;
  uint32_t gen0 = credGen;
  int bSgv = sgv, bDelta = delta, bCnt = historyCount;
  bool bDI = deltaInterp, bDU = deltaUnknown;   // 3.6.1
  String bDir = direction, bDate = lastDateString;
  unsigned long bSeen = entrySeenAtMillis, bAge = readingAgeSec;
  uint64_t bEntry = lastEntryDateMs;
  bool bAnch = ageNtpAnchored;
  memcpy(bkHist, history, sizeof(bkHist)); memcpy(bkDay, historyDay, sizeof(bkDay)); memcpy(bkTs, historyTsSec, sizeof(bkTs));
  bool ok = false;
  if (dataSource == "dexcom") ok = fetchDexcom();
  else if (dataSource == "libre") ok = fetchLibre();
  else if (dataSource == "medtrum") ok = fetchMedtrum();
  else ok = fetchNightscout();
  bool staleGen = (credGen != gen0);
  fetchDiscarded = staleGen;
  if (staleGen) {
    // cekme surerken ayarlar degisti: bu turun sonucu ve aldigi oturumlar atilir
    dexAccountId = ""; dexSessionId = ""; libreToken = ""; libreAccountId = ""; librePatientId = ""; medCookie = "";
    ok = false;
  }
  if (ok) {
    if (entrySeenAtMillis == 0) entrySeenAtMillis = millis();   // yas hep hesaplansin (veri yok alarmi)
    dataValid = true;
    consecutiveFetchFails = 0;
    netNoInternet = false;
    clearFetchError();
    showErrorScreen = false;
    errorDismissed = false;
  } else if (staleGen && srcChangedPending) {
    // 3.4.2: kaynak/hesap degisti: eski deger geri gelmesin
    clearSourceData();
  } else {
    sgv = bSgv; delta = bDelta; deltaInterp = bDI; deltaUnknown = bDU; historyCount = bCnt; direction = bDir; lastDateString = bDate;
    entrySeenAtMillis = bSeen; readingAgeSec = bAge; lastEntryDateMs = bEntry; ageNtpAnchored = bAnch;
    memcpy(history, bkHist, sizeof(bkHist)); memcpy(historyDay, bkDay, sizeof(bkDay)); memcpy(historyTsSec, bkTs, sizeof(bkTs));
    if (!staleGen) consecutiveFetchFails++;
    if (!staleGen && (fetchErrSeq == errSeq0 || lastErrorCode == 0)) setFetchError(3, sourceLabel());
    if (!staleGen && lastErrorCode == 3) { netFullTry = millis(); netNoInternet = !internetProbe(); }   // 3.4.8
    // 3.4.20: hata ekrani burada ACILMAZ (arka plan gorevi ekrani cizemez; gorunmeyen ekran dokunmayi
    // ele geciriyordu). Veri yoksa ana dongu tam hata ekranini cizer, veri varsa ustte "Giris hatasi" hapi.
  }
  lastFetch = millis();
  updateAge();
  // 3.4.20: IOB/COB 15 dk guncellenmezse gizlenir (eski deger doz kararini yaniltmasin)
  if (nsIob >= 0 && millis() - nsIobOkMs > 900000UL) nsIob = -1;
  if (nsCob >= 0 && millis() - nsCobOkMs > 900000UL) nsCob = -1;
  if (ok && dataSource != "nightscout") { computeManualAges(); checkAgeWarnings(); }
  if (currentScreen >= screenCount()) currentScreen = homeScreenIdx();
  return ok;
}

// ===================== EKRANLAR =====================

void drawArrowSimple(int cx, int cy, const String& dir, uint16_t col) {
  drawTrendArrow(cx, cy, dir, col, 28);
}

void drawUiBigDigits() {
  cv->fillScreen(UI_BG);
  if (!dataValid) {
    cv->setTextColor(sevColor(1), UI_BG);
    cv->setTextDatum(MC_DATUM);
    dsN(cv, "...", 160, 120, 4);
    return;
  }
  bool alarmOn = isAlarmActive();
  bool visBlink = alarmOn && visualAlarmOn && ((long)(millis() - visualBurstEnd) < 0);
  if (visBlink && alarmBlinkState) cv->fillScreen(getAlarmColor());
  uint16_t bg = (visBlink && alarmBlinkState) ? getAlarmColor() : UI_BG;
  uint16_t col = getGlucoseColor(sgv);
  if (visBlink && alarmBlinkState) col = TFT_BLACK;
  char buf[12];
  { String _gs = formatGlucose(sgv); strncpy(buf, _gs.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0; }
  cv->setTextColor(col, bg);
  cv->setTextDatum(MC_DATUM);
  // v1.30.0 boyutu: font 6 + size 4, ortali
  cv->setTextSize(4);
  cv->drawString(buf, 160, 120, 6);
  cv->setTextSize(1);
  // Buyuk arayuz: sadece bar (cift yas yazisi yok)
  if (useMinuteBars()) drawMinuteBars(208, readingAgeSec, false);
  else drawAgeFooter(210);
}

void drawUiCalendar() {
  cv->fillScreen(UI_BG);
  struct timeval tv; gettimeofday(&tv, nullptr);
  time_t local = tv.tv_sec + 3 * 3600;
  struct tm *tm = gmtime(&local);
  int y = tm->tm_year + 1900;
  int m = tm->tm_mon + 1;
  int d = tm->tm_mday;
  static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  // Kullanici istegiyle artik kisaltma degil, ay adinin tam hali (orn.
  // "Ağu" degil "Ağustos") - genislik kontrolu yapildi, en uzunu
  // ("Temmuz 2026") bile 115px, 320px ekranda tasma riski yok.
  int dim = mdays[m];
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) dim = 29;
  // tm_wday: 0=Pazar ... 6=Cumartesi → Pazartesi baslangic (0=Pzt)
  int firstSun = tm->tm_wday - ((d - 1) % 7);
  if (firstSun < 0) firstSun += 7;
  int w0 = (firstSun + 6) % 7; // 0=Pzt ... 6=Paz

  uint16_t accent = cv->color565(0x4C, 0x8D, 0xFF); // marka rengi — eski neon cyan yerine
  char title[28];
  snprintf(title, sizeof(title), "%s %04d", monthName(m), y);
  cv->setTextColor(accent, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(title, 150, 4, &FreeSansTR9pt);

  // TR: Pazartesi → Pazar
  const char* wd[] = {L("Pt", "Mo"), L("Sa", "Tu"), L("Ça", "We"), L("Pe", "Th"), L("Cu", "Fr"), L("Ct", "Sa"), L("Pz", "Su")};
  cv->setTextColor(TFT_DARKGREY, UI_BG);
  cv->setTextDatum(MC_DATUM);
  for (int i = 0; i < 7; i++) {
    drawFreeTitle(wd[i], 24 + i * 42, 36, &FreeSansTR7pt);
  }

  for (int day = 1; day <= dim; day++) {
    int col = (w0 + day - 1) % 7;
    int row = (w0 + day - 1) / 7;
    int cx = 24 + col * 42;
    int cy = 58 + row * 26;
    if (day == d) {
      cv->fillRoundRect(cx - 14, cy - 10, 28, 20, 4, accent);
      cv->setTextColor(TFT_WHITE, accent);
    } else {
      cv->setTextColor(TFT_WHITE, UI_BG);
    }
    char db[4]; sprintf(db, "%d", day);
    dsN(cv, db, cx, cy, 2);
  }

  // Sol alt saat, sag alt seker rozeti (+ delta + trend oku SEKERDEN SONRA)
  char tb[16];
  sprintf(tb, "%02d:%02d", tm->tm_hour, tm->tm_min);
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(BL_DATUM);
  dsN(cv, tb, 8, 235, 4);
  if (dataValid) {
    // Not: eskiden sgv dogrudan "%d" ile yaziliyordu - bu, birim ayari
    // mmol/L oldugunda yanlis (donusturulmemis, ham mg/dL) bir sayi
    // gosteriyordu. Diger tum ekranlarda oldugu gibi formatGlucose()
    // kullanildi. Kullanici istegiyle seker artik Foto Cerceve modundaki
    // gibi bir rozetin (cerceve) icinde, delta + trend oku da rozetten
    // SONRA (sagda) geliyor. Grup sag kenari x=315'te sabit, genislikler
    // cv->textWidth() ile olculup soldan sirayla (rozet->delta->ok)
    // yerlestiriliyor. Gorsel alarm aktifken (Buyuk Rakam ekranindaki
    // ayni visBlink/alarmBlinkState mantigiyla) rozetin ic dolgusu
    // esik rengiyle alarm rengi arasinda yanip soner; rozet metni her
    // zaman siyah, delta/ok ise sabit esik renginde kalir (yanip sonmez).
    uint16_t gcol = getGlucoseColor(sgv);
    bool alarmOnCal = isAlarmActive();
    bool visBlinkCal = alarmOnCal && visualAlarmOn && ((long)(millis() - visualBurstEnd) < 0);
    uint16_t badgeFill = (visBlinkCal && alarmBlinkState) ? getAlarmColor() : gcol;

    char gbuf[12]; { String _gs = formatGlucose(sgv); strncpy(gbuf, _gs.c_str(), sizeof(gbuf)-1); gbuf[sizeof(gbuf)-1]=0; }
    cv->setTextFont(4);
    int gW = cv->textWidth(gbuf);
    cv->setTextFont(2);
    String dstr = formatDelta(delta);
    int dW = cv->textWidth(dstr);
    cv->setTextFont(1);

    int arrowSize = 14;
    int gap1 = 6, gap2 = 10;   // ok-delta ve delta-rozet arasi bosluk
    int padX = 10, borderT = 2;
    int innerW = gW + padX * 2;
    int innerH = 32;
    int badgeBottom = 235;
    int badgeTop = badgeBottom - innerH;
    int midY = badgeTop + innerH / 2;
    int groupRight = 315;

    int arrowCx = groupRight - arrowSize / 2;
    int deltaRightX = arrowCx - arrowSize / 2 - gap1;
    int badgeRight = deltaRightX - dW - gap2;
    int badgeLeft = badgeRight - innerW;

    cv->fillRoundRect(badgeLeft - borderT, badgeTop - borderT, innerW + borderT * 2, innerH + borderT * 2, 6, 0x4208);
    cv->fillRoundRect(badgeLeft, badgeTop, innerW, innerH, 6, badgeFill);
    cv->setTextColor(TFT_BLACK, badgeFill);
    cv->setTextDatum(MC_DATUM);
    dsN(cv, gbuf, badgeLeft + innerW / 2, midY, 4);

    cv->setTextColor(gcol, UI_BG); // delta: sekerle ayni esik rengi, sabit (yanip sonmez)
    cv->setTextDatum(MR_DATUM);
    dsN(cv, dstr, deltaRightX, midY, 2);

    drawTrendArrow(arrowCx, midY, direction, gcol, arrowSize);
  }
}

// Anim A: kayan yazi (marquee)

void drawUiPortrait() {
  cv->fillScreen(UI_BG);
  struct timeval tv; gettimeofday(&tv, nullptr);
  time_t local = tv.tv_sec + 3 * 3600;
  struct tm *tm = gmtime(&local);
  char line[24];
  sprintf(line, "%02d:%02d", tm->tm_hour, tm->tm_min);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  cv->setTextDatum(TC_DATUM);
  dsN(cv, line, 120, 36, 4); // saat biraz asagida
  if (!dataValid) {
    cv->setTextColor(sevColor(1), UI_BG);
    dsN(cv, "...", 120, 120, 4);
    return;
  }
  uint16_t col = getGlucoseColor(sgv);
  char buf[12]; { String _gs = formatGlucose(sgv); strncpy(buf, _gs.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0; }
  cv->setTextColor(col, UI_BG);
  cv->setTextSize(3);
  cv->drawString(buf, 120, 110, 6);
  cv->setTextSize(1);
  // Ok biraz daha asagida
  drawTrendArrow(120, 250, direction, col, 28);
  { String _ds = formatDelta(delta); strncpy(buf, _ds.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0; }
  cv->setTextColor(col, UI_BG); // delta: sekerle ayni esik rengi
  dsN(cv, buf, 120, 300, 4);
}

// Coklu foto dosya adlandirmasi: "/pic0.rgb".."/pic9.rgb", sirali/bosluksuz
// (upload her zaman fotoCount'a yazar). "foto1/2/3.rgb" bilerek kullanilmadi
// - clearOldFotoFiles() bu isimleri eski/artik surumden kalinti sayip
// siliyordu, karisiklik olmasin diye yeni yapida "pic" onekine gecildi.
String fotoPath(int idx) { return "/pic" + String(idx) + ".rgb"; }

bool fotoFileValid(int idx) {
  if (idx < 0) return false;
  if (!SPIFFS.exists(fotoPath(idx))) return false;
  File f = SPIFFS.open(fotoPath(idx), FILE_READ);
  if (!f) return false;
  size_t sz = f.size();
  f.close();
  return sz == (size_t)FOTO_BYTES;
}

bool loadFotoFromFS() {
  fotoLoaded = false;
  if (!SPIFFS.begin(true)) return false;
  clearOldFotoFiles();
  // fotoCount kalici bir sayaca guvenmek yerine her seferinde gercek
  // dosyalara bakarak sayiliyor (0'dan basla, ilk eksik/gecersiz slotta
  // dur) — boylece yarim kalmis bir yukleme veya bozuk dosya kendini
  // otomatik duzeltir, kalici bir sayac gibi senkron disi kalamaz.
  int n = 0;
  while (n < FOTO_MAX && fotoFileValid(n)) n++;
  fotoCount = n;
  if (fotoActiveIdx >= fotoCount || fotoActiveIdx < 0) fotoActiveIdx = 0;
  if (fotoCount == 0) return false;
  fotoLoaded = fotoFileValid(fotoActiveIdx);
  return fotoLoaded;
}

// Seker rozeti (sadece deger, delta/ok YOK — Kullanici istegiyle kaldirildi)
// — Foto Cerceve modunda IKI yerden cizildi (tam ekran
// drawUiFotoPlaceholder() ve kisa restoreFotoTopBand()) — tek kaynaktan
// cizilsin diye ortak fonksiyona alindi, ileride biri guncellenip digeri
// unutulmasin diye (uiModeName() ile ayni gerekce). Gorsel alarm aktifken
// ic dolgu esik rengiyle alarm rengi arasinda yanip soner; rozet metni
// sabit siyah.
void drawFotoGlucoseBadge() {
  if (!dataValid) return;
  bool alarmOnFoto = isAlarmActive();
  bool visBlinkFoto = alarmOnFoto && visualAlarmOn && ((long)(millis() - visualBurstEnd) < 0);
  uint16_t c = getGlucoseColor(sgv);
  uint16_t badgeFill = (visBlinkFoto && alarmBlinkState) ? getAlarmColor() : c;
  cv->fillRoundRect(6, 6, 92, 44, 8, 0x4208);
  cv->fillRoundRect(8, 8, 88, 40, 8, badgeFill);
  char b[12];
  { String _gs = formatGlucose(sgv); strncpy(b, _gs.c_str(), sizeof(b)-1); b[sizeof(b)-1]=0; }
  cv->setTextColor(TFT_BLACK, badgeFill);
  cv->setTextDatum(MC_DATUM);
  dsN(cv, b, 52, 28, 4);
}

// Foto ust seridini cerceveyi bozmadan yenile (kutu gizlenince)
void restoreFotoTopBand() {
  const uint16_t MATTE = 0x3186;
  const uint16_t GOLD  = 0xD600;
  const uint16_t EDGE  = 0x4208;
  const uint16_t CREAM = 0xEF5D;
  cv->fillRect(0, 0, 320, 40, MATTE);
  int ox = (320 - FOTO_W) / 2;
  int oy = (240 - FOTO_H) / 2;
  if (fotoLoaded && SPIFFS.begin(true)) {
    File f = SPIFFS.open(fotoPath(fotoActiveIdx), FILE_READ);
    if (f && f.size() == (size_t)FOTO_BYTES) {
      static uint16_t lineBuf[320];
      cv->setSwapBytes(true);
      for (int y = 0; y < FOTO_H; y++) {
        if (f.read((uint8_t*)lineBuf, FOTO_W * 2) != (size_t)(FOTO_W * 2)) break;
        int sy = oy + y;
        if (sy < 0) continue;
        if (sy >= 40) break;
        cv->pushImage(ox, sy, FOTO_W, 1, lineBuf);
      }
      cv->setSwapBytes(false);
      f.close();
      // ust cerceve cizgileri
      cv->drawRect(ox - 1, oy - 1, FOTO_W + 2, FOTO_H + 2, CREAM);
      cv->drawRect(ox - 3, oy - 3, FOTO_W + 6, FOTO_H + 6, GOLD);
      cv->drawRect(ox - 5, oy - 5, FOTO_W + 10, FOTO_H + 10, EDGE);
      cv->drawRect(ox - 7, oy - 7, FOTO_W + 14, FOTO_H + 14, GOLD);
    } else if (f) f.close();
  }
  cv->drawRect(0, 0, 320, 240, GOLD);
  cv->drawRect(3, 3, 314, 234, EDGE);
  cv->fillRect(0, 0, 10, 10, GOLD);
  cv->fillRect(310, 0, 10, 10, GOLD);
  drawFotoGlucoseBadge();
}

void drawUiFotoPlaceholder() {
  // Sabit gri passepartout
  const uint16_t MATTE = 0x3186;
  const uint16_t GOLD  = 0xD600;
  const uint16_t CREAM = 0xEF5D;
  const uint16_t EDGE  = 0x4208;
  cv->fillScreen(MATTE);

  int ox = (320 - FOTO_W) / 2;
  int oy = (240 - FOTO_H) / 2;

  if (fotoLoaded) {
    if (!SPIFFS.begin(true)) { fotoLoaded = false; }
    else {
      File f = SPIFFS.open(fotoPath(fotoActiveIdx), FILE_READ);
      if (f && f.size() == (size_t)FOTO_BYTES) {
        static uint16_t lineBuf[320];
        cv->setSwapBytes(true);
        for (int y = 0; y < FOTO_H; y++) {
          if (f.read((uint8_t*)lineBuf, FOTO_W * 2) != (size_t)(FOTO_W * 2)) break;
          cv->pushImage(ox, oy + y, FOTO_W, 1, lineBuf);
        }
        cv->setSwapBytes(false);
        f.close();
      } else {
        if (f) f.close();
        fotoLoaded = false;
      }
    }
  }

  if (fotoLoaded) {
    cv->drawRect(ox - 1, oy - 1, FOTO_W + 2, FOTO_H + 2, CREAM);
    cv->drawRect(ox - 3, oy - 3, FOTO_W + 6, FOTO_H + 6, GOLD);
    cv->drawRect(ox - 5, oy - 5, FOTO_W + 10, FOTO_H + 10, EDGE);
    cv->drawRect(ox - 7, oy - 7, FOTO_W + 14, FOTO_H + 14, GOLD);
    cv->drawRect(0, 0, 320, 240, GOLD);
    cv->drawRect(3, 3, 314, 234, EDGE);
    const int cs = 10;
    cv->fillRect(0, 0, cs, cs, GOLD);
    cv->fillRect(320 - cs, 0, cs, cs, GOLD);
    cv->fillRect(0, 240 - cs, cs, cs, GOLD);
    cv->fillRect(320 - cs, 240 - cs, cs, cs, GOLD);
  } else {
    // Foto yok: QR + yazilar ortali ve biraz asagida
    String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
    String url = ip.length() ? ("http://" + ip + "/foto") : "";
    cv->setTextColor(TFT_CYAN, MATTE);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Foto yükle", "Upload photo"), 160, 28, &FreeSansTR7pt);
    if (url.length() == 0) {
      cv->setTextColor(sevColor(0), MATTE);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(L("WiFi bağlı değil", "WiFi not connected"), 160, 120, &FreeSansTR7pt);
    } else {
      QRCode qrcode;
      uint8_t qrbuf[qrcode_getBufferSize(3)];
      qrcode_initText(&qrcode, qrbuf, 3, ECC_LOW, url.c_str());
      int scale = 4;
      int qsize = qrcode.size * scale;
      int qx = (320 - qsize) / 2;
      int qy = 52; // onceki 28'den asagi — ortaya yakin
      cv->fillRect(qx - 3, qy - 3, qsize + 6, qsize + 6, TFT_WHITE);
      for (uint8_t yy = 0; yy < qrcode.size; yy++) {
        for (uint8_t xx = 0; xx < qrcode.size; xx++) {
          if (qrcode_getModule(&qrcode, xx, yy))
            cv->fillRect(qx + xx * scale, qy + yy * scale, scale, scale, TFT_BLACK);
        }
      }
      cv->setTextColor(sevColor(2), MATTE);
      cv->setTextDatum(TC_DATUM);
      dsN(cv, ip + "/foto", 160, qy + qsize + 10, 1);
      cv->setTextColor(TFT_LIGHTGREY, MATTE);
      drawFreeTitle(L("Okut - webden foto yükle", "Scan - upload from web"), 160, qy + qsize + 26, &FreeSansTR6pt);
    }
  }

  // Seker rozeti EN SONDA — sol ust (delta/ok yok, alarm yanip sonmesi
  // var) — ortak fonksiyon, restoreFotoTopBand() ile aynen paylasiliyor.
  drawFotoGlucoseBadge();
}

void drawScreen0() {
  if (teacherStep > 0) { drawTeacherFlow(); return; }
  cv->fillScreen(UI_BG);
  if (!dataValid) {
    // Bilinen hata: ekrana sigan kisa mesaj (tam hata ekrani dokununca)
    if (lastErrorCode > 0) {
      cv->setTextColor(sevColor(1), UI_BG);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(errorTitle(), 160, 64, &FreeSansTR9pt);
      cv->setTextColor(TFT_LIGHTGREY, UI_BG);
      String h = errorHintLong();
      cv->setFreeFont(&FreeSansTR7pt);
      int hy = 96, lines = 0;
      while (h.length() && lines < 3) {
        int cut = h.length();
        while (cut > 0 && cv->textWidth(h.substring(0, cut)) > 296) {
          int sp = h.lastIndexOf(' ', cut - 1);
          cut = (sp > 0) ? sp : cut - 1;
        }
        drawFreeTitle(h.substring(0, cut).c_str(), 160, hy, &FreeSansTR7pt);
        h = h.substring(cut); h.trim();
        hy += 20; lines++;
      }
      cv->fillRoundRect(80, 180, 160, 36, 18, 0x140E);
      cv->setTextColor(TFT_WHITE, 0x140E);
      drawFreeTitle(L("Dokunun: çözüm", "Tap: fix it"), 160, 198, &FreeSansTR7pt);
    } else {
      cv->setTextColor(sevColor(1), UI_BG);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(L("Veri bekleniyor...", "Waiting for data..."), 160, 110, &FreeSansTR9pt);
    }
    return;
  }

  bool alarmOn = isAlarmActive();
  bool visBlink = alarmOn && visualAlarmOn && ((long)(millis() - visualBurstEnd) < 0);
  if (visBlink) {
    if (millis() - lastBlink > 400) { alarmBlinkState = !alarmBlinkState; lastBlink = millis(); }
    if (alarmBlinkState) cv->fillScreen(getAlarmColor());
  }

  uint16_t bg = (visBlink && alarmBlinkState) ? getAlarmColor() : UI_BG;
  uint16_t col = getGlucoseColor(sgv);
  if (visBlink && alarmBlinkState) col = TFT_BLACK;

  // Ana sayfa: eski buyuk duzene yakin (sadece cok hafif asagida)
  char buf[12];
  { String _gs = formatGlucose(sgv); strncpy(buf, _gs.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0; }
  cv->setTextColor(col, bg);
  cv->setTextDatum(MC_DATUM);
  cv->setTextSize(3);
  cv->drawString(buf, 160, 96, 6);                  // 3.4.2: ust cubuga yer (78 -> 96)
  cv->setTextSize(1);

  { String _ds = formatDelta(delta); strncpy(buf, _ds.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0; }
  cv->setTextColor(col, bg); // delta: sekerle ayni esik rengi
  {
    // Delta + ok grubu ana ekranda sayfaya (x=160) gore ortalaniyor. Eskiden
    // sabit x=135/215 kullaniliyordu; bu, 2 haneli delta degerlerinde
    // (orn. "+15") grubu belirgin sekilde sola kaydiriyordu. drawUiSade'deki
    // ile ayni yontemle (cv->textWidth() ile olcup tek grup genisligi
    // uzerinden konumlandirarak) duzeltildi.
    cv->setTextFont(4);
    int deltaW = cv->textWidth(buf) * 5 / 2; // 3.4.3: delta setTextSize(2.5)
    cv->setTextFont(1);
    int arrowSize = 26;
    int gap = 16;
    int totalW = deltaW + gap + arrowSize;
    int groupLeft = 160 - totalW / 2;
    cv->setTextDatum(MR_DATUM);
    cv->setTextSize(2.5f);
    int deltaRightX = groupLeft + deltaW;
    dsN(cv, buf, deltaRightX, 172, 4);
    cv->setTextSize(1);
    int arrowCx = groupLeft + deltaW + gap + arrowSize / 2;
    drawTrendArrow(arrowCx, 172, direction, col, arrowSize);
  }

  // Dakika bar: ertele/dikkat veya ogretmen akisi varken gizle - bu iki
  // buton (188-232 arasi) tam bu barin alanina denk geldigi icin, bar
  // acikken cizilirse butonlarin ustunu (siyahla) kapatiyordu. Yorum satiri
  // bunu zaten soyluyordu ama kod sadece ogretmen modunu kontrol ediyordu -
  // normal moddaki Ertele/Dikkat kontrolu eksikti, simdi eklendi.
  bool hideBars = shouldShowSnoozeButton() || shouldShowDikkat() ||
                  (teacherModeOn && teacherStep > 0);
  if (!hideBars) {
    if (useMinuteBars()) drawMinuteBars(205, readingAgeSec);
    else drawAgeFooter(205);
  } else if (!(teacherModeOn && teacherStep > 0)) {
    drawCompactAge(160, 196);
  }

  // Ertele — artik yuvarlak degil: Dikkat ile ayni kart dilinde, sag altta (Dikkat'in aynasi)
  if (shouldShowSnoozeButton()) {
    uint16_t erteleBg = cv->color565(0x4A, 0x8C, 0xE0); // hafif/yumusak mavi
    cv->fillRoundRect(212, 188, 100, 44, 8, erteleBg);
    cv->setTextColor(TFT_WHITE, erteleBg);
    // Ikon+yazi ikilisi, Dikkat kartindaki gibi kutunun tam ortasina (x=262)
    // gelecek sekilde calisma zamaninda textWidth() ile olculerek ortalandi
    // (TR/EN metin uzunlugu farkli oldugu icin sabit x kullanilamaz).
    {
      cv->setFreeFont(&FreeSansTR7pt);
      int erteleTextW = cv->textWidth(L("Ertele", "Snooze"));
      cv->setTextFont(1);
      int erteleIconW = 16, erteleGap = 8;
      int erteleGroupW = erteleIconW + erteleGap + erteleTextW;
      int erteleStartX = 262 - erteleGroupW / 2;
      int erteleIconCx = erteleStartX + erteleIconW / 2;
      int erteleTextX = erteleStartX + erteleIconW + erteleGap;
      drawClockIcon(erteleIconCx, 200, TFT_WHITE);
      cv->setTextDatum(ML_DATUM);
      drawFreeTitle(L("Ertele", "Snooze"), erteleTextX, 200, &FreeSansTR7pt);
    }
    char sl[10];
    snprintf(sl, sizeof(sl), L("%d dk", "%d min"), snoozeShownMins());
    cv->setTextDatum(MC_DATUM);
    dsN(cv, sl, 262, 220, 1);
  }
  // Dikkat: alarmLow alti veya alarmVeryHigh ustu — Ertele ile ayni anda gorunebilir (birbirini engellemez)
  if (shouldShowDikkat()) {
    cv->fillRoundRect(8, 188, 100, 44, 8, 0x780F);
    cv->setTextColor(TFT_WHITE, 0x780F);
    drawWarnIcon(58, 200, TFT_WHITE, 0x780F);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("Dikkat", "Alert"), 58, 220, &FreeSansTR7pt);
  }
}

void drawUiSade() {
  // Eski sade duzen (ortali saat + seker + dakika)
  cv->fillScreen(UI_BG);

  // Not: eskiden burada ust kisimda kucuk bir "METEMATIK" kimlik seridi
  // vardi (nokta + yazi + ayirici cizgi) - kullanici talebiyle kaldirildi.

  struct timeval tv; gettimeofday(&tv, nullptr);
  time_t local = tv.tv_sec + 3 * 3600;
  struct tm *tm = gmtime(&local);
  char timeStr[16], dateStr[32];
  if (tv.tv_sec > 1600000000) {
    strftime(timeStr, sizeof(timeStr), "%H:%M", tm);
    strftime(dateStr, sizeof(dateStr), "%d.%m.%Y", tm);
  } else { strcpy(timeStr, "--:--"); strcpy(dateStr, "----"); }

  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(TC_DATUM);
  cv->drawString(timeStr, 160, 24, 6);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  dsN(cv, dateStr, 160, 68, 2);

  if (dataValid) {
    uint16_t col = getGlucoseColor(sgv);
    char numBuf[12]; { String _gs = formatGlucose(sgv); strncpy(numBuf, _gs.c_str(), sizeof(numBuf)-1); numBuf[sizeof(numBuf)-1]=0; }
    char deltaBuf[12];
    if (deltaUnknown) strcpy(deltaBuf, "--");          // 3.6.1
    else if (delta > 0) sprintf(deltaBuf, "+%d%s", delta, deltaInterp ? "*" : "");
    else if (delta < 0) sprintf(deltaBuf, "%d%s", delta, deltaInterp ? "*" : "");
    else sprintf(deltaBuf, "0%s", deltaInterp ? "*" : "");

    // Buyuk seker + delta + ok grubu artik sayfaya (x=160) gore ortalaniyor.
    // Eskiden sabit x=90/220/280 kullaniliyordu; bu da grubun agirlik
    // merkezini (ozellikle en buyuk oge olan sekeri) sayfanin gercek
    // ortasindan (160) belirgin sekilde sola kaydiriyordu. Simdi uc ogenin
    // genisligi de calisma zamaninda cv->textWidth() ile olculup, tek bir
    // grup genisligi (totalW) uzerinden 160'a gore ortalaniyor - boylece 2/3
    // haneli seker degerlerinde ve +/- delta isaretlerinde de dogru kaliyor.
    cv->setTextSize(1);
    cv->setTextFont(7);
    int numW = cv->textWidth(numBuf);
    cv->setTextFont(4);
    int deltaW = cv->textWidth(deltaBuf) * 2; // delta setTextSize(2) ile cizilecek
    cv->setTextFont(1);
    int arrowSize = 24;
    int gap1 = 14, gap2 = 16; // seker-delta ve delta-ok arasi bosluk
    int totalW = numW + gap1 + deltaW + gap2 + arrowSize;
    int groupLeft = 160 - totalW / 2;

    cv->setTextColor(col, UI_BG);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(numBuf, groupLeft + numW / 2, 125, 7);

    cv->setTextColor(col, UI_BG); // delta: sekerle ayni esik rengi
    cv->setTextDatum(MR_DATUM);
    cv->setTextSize(2);
    int deltaRightX = groupLeft + numW + gap1 + deltaW;
    dsN(cv, deltaBuf, deltaRightX, 130, 4);
    cv->setTextSize(1);
    int arrowCx = groupLeft + numW + gap1 + deltaW + gap2 + arrowSize / 2;
    drawTrendArrow(arrowCx, 130, direction, col, arrowSize); // delta ile ayni hatta (y=130) ortalandi
    // NS: AINS / AKRB — mini grafikten ustte, biraz buyuk
    if (dataSource == "nightscout") {
      char line[40];
      char a[12], c[12];
      if (nsIob >= 0) dtostrf(nsIob, 0, 2, a); else strcpy(a, "-");
      if (nsCob >= 0) dtostrf(nsCob, 0, 1, c); else strcpy(c, "-");
      snprintf(line, sizeof(line), L("AINS %s U   AKRB %s g", "IOB %s U   COB %s g"), a, c);
      cv->setTextColor(TFT_CYAN, UI_BG);
      cv->setTextDatum(TC_DATUM);
      dsN(cv, line, 160, 154, 2);
    }
    // Basit: son 3 saat mini grafik
    {
      uint32_t nowSec = (uint32_t)time(nullptr);
      uint32_t cut = (nowSec > 3UL * 3600UL) ? nowSec - 3UL * 3600UL : 0;
      int idx[HIST_MAX]; int cnt = 0;
      for (int i = 0; i < historyCount; i++) {
        if (historyTsSec[i] == 0 || historyTsSec[i] >= cut) {
          if (cnt < HIST_MAX) idx[cnt++] = i;
        }
      }
      int gy = 176, gh = 36, gw = 280, sx = 20;
      cv->fillRect(sx, gy, gw, gh, 0x1082);
      cv->drawRoundRect(sx - 2, gy - 2, gw + 4, gh + 4, 4, cv->color565(0x1B, 0x21, 0x2B)); // ince kart cercevesi
      int yHi = gy + gh - ((180 - 40) * gh) / (300 - 40);
      int yLo = gy + gh - ((70 - 40) * gh) / (300 - 40);
      if (yLo > yHi) cv->fillRect(sx, yHi, gw, yLo - yHi, 0x0320);
      cv->setTextColor(TFT_DARKGREY, UI_BG);
      cv->setTextDatum(TC_DATUM);
      dsN(cv, "-3s", sx + 2, gy + gh + 1, 1);
      dsN(cv, "-1.5s", sx + gw / 2, gy + gh + 1, 1);
      drawFreeTitle(L("şimdi", "now"), sx + gw - 8, gy + gh + 1, &FreeSansTR6pt);
      if (cnt > 1) {
        for (int i = 0; i < cnt - 1; i++) {
          int v1 = history[idx[i]], v2 = history[idx[i+1]];
          int x1 = sx + (i * gw) / (cnt - 1);
          int x2 = sx + ((i + 1) * gw) / (cnt - 1);
          int yy1 = constrain(gy + gh - ((constrain(v1,40,300)-40)*gh)/(300-40), gy, gy+gh);
          int yy2 = constrain(gy + gh - ((constrain(v2,40,300)-40)*gh)/(300-40), gy, gy+gh);
          cv->drawLine(x1, yy1, x2, yy2, getGlucoseColor((v1+v2)/2));
        }
      }
    }
  }
}

void drawScreen1() {
  if (lastUiChromeMs == 0) lastUiChromeMs = millis();
  // 2. sayfa = secili arayuz + sag ust acilir kutu
  switch (uiPageMode) {
    case 1: drawUiBigDigits(); break;
    case 2: drawUiCalendar(); break;
    case 3: drawUiVertical(); break;
    case 4: drawUiFotoPlaceholder(); break;
    case 5: drawUiRing(); break;
    case 6: drawUiCardTheme(); break;
    case 7: drawUiClock(); break;
    case 8: drawUiWave(); break;
    case 9: drawUiMinimal(); break;
    case 10: drawUiThermo(); break;
    case 11: drawUiPanel(); break;
    case 12: drawUiPulse(); break;
    case 13: drawUiStars(); break;
    case 14: drawUiBalloon(); break;
    case 15: drawUiSpace(); break;
    case 16: drawUiAquarium(); break;
    case 17: drawUiSmiley(); break;
    default: drawUiSade(); break;
  }
}

void drawNsDetailScreen() {
  cv->fillScreen(UI_BG);
  drawInfoIconM(14, 14, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("DETAY (NS)", "DETAIL (NS)"), 168, 4, &FreeSansTR7pt);
  cv->drawFastHLine(8, 26, 304, 0x2104);

  // Kart 1: AINS / AKRB yan yana
  cv->drawRoundRect(8, 30, 304, 68, 10, 0x2945);
  cv->drawFastVLine(160, 38, 50, 0x2945);
  cv->setTextDatum(TL_DATUM);
  cv->setTextColor(sevColor(1), UI_BG);
  drawFreeTitle(L("AINS", "IOB"), 16, 36, &FreeSansTR7pt);
  cv->setTextColor(TFT_WHITE, UI_BG);
  char buf[24];
  if (nsIob >= 0) {
    dtostrf(nsIob, 0, 2, buf);
    strcat(buf, " U");
    dsN(cv, buf, 16, 58, 4);
  } else dsN(cv, "-", 16, 58, 4);

  cv->setTextColor(sevColor(1), UI_BG);
  drawFreeTitle(L("AKRB", "COB"), 172, 36, &FreeSansTR7pt);
  cv->setTextColor(TFT_WHITE, UI_BG);
  if (nsCob >= 0) {
    dtostrf(nsCob, 0, 1, buf);
    strcat(buf, " g");
    dsN(cv, buf, 172, 58, 4);
  } else dsN(cv, "-", 172, 58, 4);

  // Kart 2: bolus tablosu — son 5
  cv->drawRoundRect(8, 104, 304, 110, 10, 0x2945);
  cv->setTextColor(TFT_CYAN, UI_BG);
  drawFreeTitle(L("Son 5 bolus", "Last 5 boluses"), 16, 110, &FreeSansTR7pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  drawFreeTitle(L("Saat", "Time"), 16, 128, &FreeSansTR6pt);
  dsN(cv, "U", 100, 128, 1);
  drawFreeTitle(L("Tip", "Type"), 140, 128, &FreeSansTR6pt);
  int row = 0;
  for (int i = 0; i < bolusCount && row < 5; i++) {
    int y = 140 + row * 12;
    if (boluses[i].ts > 0) {
      time_t t = (time_t)boluses[i].ts + 3 * 3600;
      struct tm tm = *gmtime(&t);
      sprintf(buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else sprintf(buf, "--:--");
    cv->setTextColor(TFT_WHITE, UI_BG);
    dsN(cv, buf, 16, y, 1);
    dtostrf(boluses[i].units, 0, 2, buf);
    dsN(cv, buf, 100, y, 1);
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    dsN(cv, boluses[i].note, 140, y, 1);
    row++;
  }
  if (bolusCount == 0) {
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    drawFreeTitle(L("Bolus kaydı yok / plugin kapalı", "No bolus / plugin off"), 16, 144, &FreeSansTR6pt);
  }

  cv->fillRoundRect(200, 6, 110, 36, 6, 0x4B72);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  drawBackIcon(218, 24, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 234, 24, &FreeSansTR7pt);
}

void drawScreen2() {
  if (dataSource != "nightscout") graphDetailOpen = false;
  cv->fillScreen(UI_BG);
  drawBackChevron(12, 26);

  if (graphDetailOpen && dataSource == "nightscout") {
    drawNsDetailScreen();
    return;
  }

  if (dataValid) {
    // Ust satir hizali: seker | ok | delta | saat kutusu
    uint16_t col = getGlucoseColor(sgv);
    char buf[40];
    { String _gs = formatGlucose(sgv); strncpy(buf, _gs.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0; }
    cv->setTextColor(col, UI_BG);
    cv->setTextDatum(ML_DATUM);
    dsN(cv, buf, 70, 26, 4);
    cv->setTextFont(4);
    int gvw = cv->textWidth(buf);
    cv->setTextFont(1);
    drawTrendArrow(70 + gvw + 12, 26, direction, col, 14);
    {
      // Delta oka bitisik, sol hizali (ML_DATUM) cizilir - onceki sabit
      // sag-hizali (MR_DATUM) yerleşim tasmayi onluyordu ama kisa/sik
      // delta degerlerini oktan koparip sagdaki Detay/saat kutusuna
      // yapistiriyordu. Simdi delta once oka bitisik sol hizali cizilir;
      // 2 haneli (orn. "+15", "-99") ve mmol/L ondalikli (orn. "+3.1")
      // genis degerlerde tasmayi onlemek icin metin genisligi calisma
      // zamaninda cv->textWidth() ile olculup gerekirse baslangic
      // noktasi sola kaydiriliyor.
      String ds = formatDelta(delta);
      cv->setTextColor(col, UI_BG); // delta: sekerle ayni esik rengi (digerleriyle tutarli)
      cv->setTextFont(4);
      int deltaW = cv->textWidth(ds);
      cv->setTextFont(1);
      int deltaSafeRight = (dataSource == "nightscout") ? 174 : 226;
      int deltaX = 70 + gvw + 26;
      if (deltaX + deltaW > deltaSafeRight) deltaX = deltaSafeRight - deltaW;
      cv->setTextDatum(ML_DATUM);
      dsN(cv, ds, deltaX, 28, 4);
    }

    // NS: Detay — delta ile saat kutusu arasinda ayri buton
    if (dataSource == "nightscout") {
      cv->fillRoundRect(178, 6, 48, 40, 6, 0x5A0A);
      cv->setTextColor(TFT_WHITE, 0x5A0A);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(L("Detay", "Detail"), 202, 26, &FreeSansTR6pt);
    }

    // Sagda acilir saat kutusu
    cv->fillRoundRect(230, 6, 84, 40, 6, 0x4B72);
    cv->setTextColor(TFT_WHITE, 0x4B72);
    cv->setTextDatum(MC_DATUM);
    char hb[12];
    snprintf(hb, sizeof(hb), L("%d saat", "%dh"), graphHourLabelFor(graphHoursIdx));
    dsN(cv, hb, 272, 18, 2);
    dsN(cv, graphMenuOpen ? "^" : "v", 272, 34, 1);

    // Saat menusu: sadece 3/6/12/24 (Detay ayri buton)
    int ghMax = (dataSource == "libre") ? 3 : GRAPH_HOURS_N;
    if (dataSource == "libre" && graphHoursIdx > 2) graphHoursIdx = 2;

    if (graphMenuOpen) {
      const int rowH = 36;
      for (int i = 0; i < ghMax; i++) {
        int my = 48 + i * rowH;
        bool sel = (i == graphHoursIdx);
        cv->fillRoundRect(226, my, 90, rowH - 3, 6, sel ? 0x0B5E : 0x3186);
        cv->setTextColor(TFT_WHITE, sel ? 0x0B5E : 0x3186);
        cv->setTextDatum(MC_DATUM);
        char tb[12];
        snprintf(tb, sizeof(tb), L("%d s", "%dh"), graphHourLabelFor(i));
        dsN(cv, tb, 271, my + (rowH - 3) / 2, 2);
      }
    }

    // Filtre
    uint32_t nowSec = (uint32_t)time(nullptr);
    int useHours = graphHourLabelFor(graphHoursIdx);
    if (dataSource == "libre" && useHours > 12) useHours = 12;
    uint32_t cutoff = (nowSec > (uint32_t)useHours * 3600UL)
                      ? nowSec - (uint32_t)useHours * 3600UL : 0;
    int idx[HIST_MAX];
    int cnt = 0;
    for (int i = 0; i < historyCount; i++) {
      if (historyTsSec[i] == 0 || historyTsSec[i] >= cutoff) idx[cnt++] = i;
    }
    if (cnt < 2) {
      cnt = historyCount;
      for (int i = 0; i < cnt; i++) idx[i] = i;
    }

    int minV = 40, maxV = 300;
    int graphY = graphMenuOpen ? 195 : 70;
    int graphH = graphMenuOpen ? 30 : 140;
    int graphW = 280, startX = 32;
    if (graphH < 40) graphH = 40;

    // Hedef bant golgesi 70-180
    {
      int yHi = graphY + graphH - ((180 - minV) * graphH) / (maxV - minV);
      int yLo = graphY + graphH - ((70 - minV) * graphH) / (maxV - minV);
      if (yHi < graphY) yHi = graphY;
      if (yLo > graphY + graphH) yLo = graphY + graphH;
      if (yLo > yHi) cv->fillRect(startX + 1, yHi, graphW - 2, yLo - yHi,
                                  colorBlindMode ? cv->color565(0x00, 0x33, 0x26) : (uint16_t)0x0320); // koyu yesil golge
    }
    const int levels[] = {40, 55, 70, 180, 250, 300};
    for (int li = 0; li < 6; li++) {
      int lv = levels[li];
      int yy = graphY + graphH - ((lv - minV) * graphH) / (maxV - minV);
      if (yy < graphY || yy > graphY + graphH) continue;
      uint16_t lc = (lv == 70 || lv == 180) ? sevColor(2) : (uint16_t)0x3186;
      cv->drawFastHLine(startX, yy, graphW, lc);
      cv->setTextColor(TFT_DARKGREY, UI_BG);
      cv->setTextDatum(MR_DATUM);
      char lb[6]; sprintf(lb, "%d", lv);
      dsN(cv, lb, startX - 2, yy, 1);
    }
    cv->drawRect(startX, graphY, graphW, graphH, TFT_DARKGREY);

    // Sabit gun saatleri 00 03 06 09 12 15 18 21 (pencere icinde)
    if (!graphMenuOpen && useHours >= 3) {
      uint32_t winStart = nowSec - (uint32_t)useHours * 3600UL;
      uint32_t span = (uint32_t)useHours * 3600UL;
      if (span < 1) span = 1;
      time_t localNow = (time_t)nowSec + 3 * 3600;
      struct tm tmL = *gmtime(&localNow);
      int secSinceMid = tmL.tm_hour * 3600 + tmL.tm_min * 60 + tmL.tm_sec;
      uint32_t localMidUtc = nowSec - (uint32_t)secSinceMid;
      for (int dayOff = 0; dayOff <= 1; dayOff++) {
        uint32_t mid = localMidUtc - (uint32_t)dayOff * 86400UL;
        for (int h = 0; h < 24; h += 3) {
          uint32_t mark = mid + (uint32_t)h * 3600UL;
          if (mark < winStart || mark > nowSec) continue;
          int xx = startX + (int)((uint64_t)(mark - winStart) * (uint64_t)graphW / (uint64_t)span);
          if (xx < startX || xx > startX + graphW) continue;
          if (mark > winStart && mark < nowSec)
            cv->drawFastVLine(xx, graphY + 1, graphH - 2, 0x3186);
          char hl[4];
          sprintf(hl, "%02d", h);
          cv->setTextColor(TFT_DARKGREY, UI_BG);
          cv->setTextDatum(TC_DATUM);
          dsN(cv, hl, xx, graphY + graphH + 2, 1);
        }
      }
    }

    if (cnt > 1 && !graphMenuOpen) {
      for (int i = 0; i < cnt - 1; i++) {
        int v1 = history[idx[i]], v2 = history[idx[i + 1]];
        int x1 = startX + (i * graphW) / (cnt - 1);
        int x2 = startX + ((i + 1) * graphW) / (cnt - 1);
        int y1 = constrain(graphY + graphH - ((constrain(v1, minV, maxV) - minV) * graphH) / (maxV - minV), graphY, graphY + graphH);
        int y2 = constrain(graphY + graphH - ((constrain(v2, minV, maxV) - minV) * graphH) / (maxV - minV), graphY, graphY + graphH);
        cv->drawLine(x1, y1, x2, y2, getGlucoseColor((v1 + v2) / 2));
      }
    }
  }
}

// "Xg Ys" / "Xd Yh" kisa sure metni (Sensor/Set ekrani icin - genis "X gun Y
// saat" formu iki sutunlu dar kartlara sigmiyor).
static void formatAgeShort(float hours, char* out, size_t outSize) {
  int d = (int)(hours / 24);
  int h = (int)hours % 24;
  snprintf(out, outSize, L("%dg %dsa", "%dd %dh"), d, h);
}

// Sensor/Set yasi karti: ust baslik + cerceveli kutu icinde "Gecen | Kalan"
// iki sutunu (Istatistik ekranindaki Min/Maks/Ort kartiyla ayni cerceve
// dili - drawRoundRect + orta ayirici cizgi). Nightscout sadece degisimden
// bu yana GECEN sureyi veriyor; KALAN sure, kullanicinin (web arayuzunden
// gun+saat olarak girdigi) ortalama omur (lifeHours) esas alinarak burada
// hesaplaniyor, bu yuzden "tahmini" oldugu ekran altindaki notta ayrica
// belirtiliyor.
void drawAgeCard(int cardY, const char* label, float ageHours, int lifeHours) {
  cv->setTextDatum(TC_DATUM);
  cv->setTextColor(TFT_WHITE, UI_BG);
  drawFreeTitle(label, 160, cardY, &FreeSansTR7pt);

  int boxY = cardY + 28;
  cv->drawRoundRect(10, boxY, 300, 62, 12, 0x2945);
  cv->drawFastVLine(160, boxY + 8, 46, 0x2945);
  cv->setTextDatum(MC_DATUM);

  if (ageHours >= 0) {
    float lifeHoursF = (float)lifeHours;
    uint16_t c = ageHours >= lifeHoursF ? sevColor(0) : (ageHours >= lifeHoursF * 0.7f ? sevColor(1) : sevColor(2));
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("Geçen", "Elapsed"), 85, boxY + 10, &FreeSansTR6pt);
    drawFreeTitle(L("Kalan", "Left"), 235, boxY + 10, &FreeSansTR6pt);

    char buf[24];
    formatAgeShort(ageHours, buf, sizeof(buf));
    cv->setTextColor(c, UI_BG);
    dsN(cv, buf, 85, boxY + 38, 4);

    float remH = lifeHoursF - ageHours;
    // Kalan sure rengi 'Gecen'den (c, yuzdeye gore) BAGIMSIZ: mutlak kalan
    // saate gore - son 24 saatte sari, son 3 saatte (ve Doldu'da) kirmizi.
    uint16_t remColor = (remH <= 3.0f) ? sevColor(0) : (remH <= 24.0f) ? sevColor(1) : sevColor(2);
    cv->setTextColor(remColor, UI_BG);
    if (remH <= 0) {
      drawFreeTitle(L("Doldu", "Overdue"), 235, boxY + 38, &FreeSansTR9pt);
    } else {
      formatAgeShort(remH, buf, sizeof(buf));
      dsN(cv, buf, 235, boxY + 38, 4);
    }
  } else {
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    drawFreeTitle(L("Veri yok", "No data"), 160, boxY + 30, &FreeSansTR9pt);
  }
  cv->setTextDatum(TL_DATUM);
}

void drawScreen3_Sensor() {
  cv->fillScreen(UI_BG);
  drawFormHeaderBtn(L("SENSÖR / SET", "SENSOR / SET"), L("Ayarla", "Setup"), true);
  if (!sensorTrackOn && !cannulaTrackOn) {
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Sensör/Set takibi kapalı", "Sensor/Set tracking is off"), 160, 100, &FreeSansTR9pt);
    drawFreeTitle(L("Açmak için sağ üstte Ayarla", "Use Setup (top right) to turn it on"), 160, 130, &FreeSansTR7pt);
  } else if (sensorTrackOn && cannulaTrackOn) {
    drawAgeCard(44, L("Sensör", "Sensor"), sensorAgeHours, sensorLifeHours);
    drawAgeCard(138, L("İnfüzyon seti / pompa", "Infusion set / pump"), cannulaAgeHours, cannulaLifeHours);
  } else if (sensorTrackOn) {
    drawAgeCard(80, L("Sensör", "Sensor"), sensorAgeHours, sensorLifeHours);
  } else {
    drawAgeCard(80, L("İnfüzyon seti / pompa", "Infusion set / pump"), cannulaAgeHours, cannulaLifeHours);
  }
  if (!(sensorTrackOn && cannulaTrackOn)) {
    cv->setTextDatum(TC_DATUM);
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    drawFreeTitle(L("Kalan süre = ömür - geçen süre", "Remaining = life - elapsed"), 160, 200, &FreeSansTR6pt);
  }
  cv->setTextDatum(TL_DATUM);
}

// Sensor/Set sure-dolum uyari sayfasi (12sa/3sa/1sa/15dk kala) - birkac
// saniye gosterilip loop() tarafindan otomatik kapatilir (bkz. AGE_WARN_SHOW_MS
// ve loop() basindaki ageWarnActive kontrolu). Esik ne kadar acilse (15 dk)
// renk o kadar "sicak" (kirmizi); 12 saat gibi uzak esikler daha sakin (cyan).
void drawAgeWarnScreen() {
  const uint16_t urgColor[4] = {sevColor(0), sevColor(3), sevColor(1), TFT_CYAN};
  uint16_t c = urgColor[constrain(ageWarnThreshIdx, 0, 3)];
  cv->fillScreen(UI_BG);
  cv->drawRoundRect(10, 10, 300, 220, 16, c);
  cv->drawRoundRect(12, 12, 296, 216, 14, c);

  int cx = 160, cy = 62;
  cv->fillTriangle(cx, cy - 22, cx - 22, cy + 18, cx + 22, cy + 18, c);
  cv->fillRect(cx - 3, cy - 8, 6, 16, UI_BG);
  cv->fillCircle(cx, cy + 14, 3, UI_BG);

  cv->setTextDatum(TC_DATUM);
  cv->setTextColor(TFT_WHITE, UI_BG);
  drawFreeTitle(ageWarnKind == 0 ? L("Sensör", "Sensor") : L("Set / Pompa", "Set / Pump"),
                160, 100, &FreeSansTR9pt);

  // 3.4.14: esigin adi degil, gercek kalan sure (cihaz kapaliyken esik gecildiyse de dogru)
  int keepCtx = liveCtx;                       // 4.0: uyarinin ait oldugu kisinin sureleri
  if (personCount > 1 && ageWarnPerson < personCount) {
    ctxSwitch(ageWarnPerson);
    cv->setTextColor(TFT_CYAN, UI_BG);
    drawFreeTitle(personLabel(ageWarnPerson).c_str(), 160, 16, &FreeSansTR7pt);
    cv->setTextColor(TFT_WHITE, UI_BG);
  }
  float ageH = (ageWarnKind == 0) ? sensorAgeHours : cannulaAgeHours;
  int lifeH = (ageWarnKind == 0) ? sensorLifeHours : cannulaLifeHours;
  ctxSwitch(keepCtx);
  long remMin = (ageH >= 0) ? (long)(((float)lifeH - ageH) * 60.0f + 0.5f) : (long)(AGE_WARN_THRESH_H[constrain(ageWarnThreshIdx, 0, 3)] * 60.0f);
  if (remMin < 1) remMin = 1;
  char rb[40];
  if (remMin < 60) snprintf(rb, sizeof(rb), L("%ld DAKİKA KALDI", "%ld MINUTES LEFT"), remMin);
  else if (remMin % 60 == 0) snprintf(rb, sizeof(rb), L("%ld SAAT KALDI", "%ld HOURS LEFT"), remMin / 60);
  else snprintf(rb, sizeof(rb), L("%ld SAAT %ld DK KALDI", "%ldh %ldmin LEFT"), remMin / 60, remMin % 60);
  cv->setTextColor(c, UI_BG);
  drawFreeTitle(rb, 160, 132, &FreeSansTR9pt);

  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  drawFreeTitle(L("süresi dolmak üzere", "is running out"), 160, 168, &FreeSansTR7pt);

  cv->setTextColor(TFT_DARKGREY, UI_BG);
  drawFreeTitle(L("Detaylar için Sensör/Set ekranına bakın", "See the Sensor/Set screen for details"), 160, 200, &FreeSansTR6pt);

  cv->setTextDatum(TL_DATUM);
}

void drawStatsScreen() {
  cv->fillScreen(UI_BG);
  drawBackChevron(14, 14);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("İSTATİSTİK", "STATISTICS"), 168, 6, &FreeSansTR9pt);
  cv->drawFastHLine(10, 26, 300, 0x2104);

  // Son 24 saat (bakilan andan geriye)
  int minV = 999, maxV = 0, avgV = 0, pct70180 = 0, pct70140 = 0, used = 0;
  uint32_t tMin = 0, tMax = 0;
  calcLast24hStats(minV, maxV, avgV, pct70180, pct70140, used, tMin, tMax);
  if (!statsEnough(used, tMin, tMax)) {
    // 3.4.1: kisa veriyle (orn. Libre 12 saat) yaniltici istatistik gosterme
    int h = statsSpanHours(used, tMin, tMax);
    cv->fillRoundRect(14, 58, 292, 150, 14, 0x2104);
    cv->setTextColor(TFT_WHITE, 0x2104);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("Henüz yeterli veri yok", "Not enough data yet"), 160, 84, &FreeSansTR9pt);
    cv->setTextColor(TFT_LIGHTGREY, 0x2104);
    char b1[48]; snprintf(b1, sizeof(b1), L("İstatistik için en az %d saat gerekir.", "Needs at least %d hours of data."), STATS_MIN_HOURS);
    drawFreeTitle(b1, 160, 116, &FreeSansTR7pt);
    char b2[40]; snprintf(b2, sizeof(b2), L("Şu an: yaklaşık %d saat", "Now: about %d hours"), h);
    drawFreeTitle(b2, 160, 140, &FreeSansTR7pt);
    cv->setTextColor(UI_SUBTEXT, 0x2104);
    drawFreeTitle(L("Cihaz açık kaldıkça kendi kaydını biriktirir.", "The device keeps its own log while it runs."), 160, 170, &FreeSansTR6pt);
    drawFreeTitle(sourceLabel().c_str(), 160, 190, &FreeSansTR6pt);
    cv->setTextDatum(TL_DATUM);
    return;
  }

  char aralik[56];
  if (tMin > 0 && tMax > 0) {
    time_t a = (time_t)tMin + 3 * 3600, b = (time_t)tMax + 3 * 3600;
    struct tm ta = *gmtime(&a);
    struct tm tb = *gmtime(&b);
    sprintf(aralik, "%02d.%02d %02d:%02d - %02d:%02d",
            ta.tm_mday, ta.tm_mon + 1, ta.tm_hour, ta.tm_min, tb.tm_hour, tb.tm_min);
  } else {
    snprintf(aralik, sizeof(aralik), "%s", estimatedHoursLabel(used).c_str());
  }
  cv->setTextColor(sevColor(1), UI_BG);
  cv->setTextDatum(TC_DATUM);
  dsN(cv, aralik, 160, 30, 2);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  {
    char hb[16]; snprintf(hb, sizeof(hb), L("~%d saat", "~%dh"), statsSpanHours(used, tMin, tMax));
    String subLine = String(L("Son veri: ", "Last: ")) + ageText() + "  -  " + hb;
    dsN(cv, subLine, 160, 50, 1);
  }

  // Min / Maks / Ort karti
  cv->drawRoundRect(10, 68, 300, 62, 12, 0x2945);
  cv->drawFastVLine(110, 76, 46, 0x2945);
  cv->drawFastVLine(210, 76, 46, 0x2945);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Min", "Min"), 60, 78, &FreeSansTR7pt);
  drawFreeTitle(L("Maks", "Max"), 160, 78, &FreeSansTR7pt);
  drawFreeTitle(L("Ort", "Avg"), 260, 78, &FreeSansTR7pt);
  if (used > 0) {
    cv->setTextColor(TFT_CYAN, UI_BG); dsN(cv, formatGlucose(minV), 60, 104, 4);
    cv->setTextColor(sevColor(3), UI_BG); dsN(cv, formatGlucose(maxV), 160, 104, 4);
    cv->setTextColor(sevColor(2), UI_BG); dsN(cv, formatGlucose(avgV), 260, 104, 4);
  } else {
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    dsN(cv, "-", 60, 104, 4);
    dsN(cv, "-", 160, 104, 4);
    dsN(cv, "-", 260, 104, 4);
  }

  // Baslik satiri (kart disinda, ustte) - "Hedef aralikta kalma", ekrana ortali
  char buf[32];
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("Hedef aralıkta kalma", "Time in range"), 160, 140, &FreeSansTR7pt);
  cv->setTextDatum(TL_DATUM);

  // Hedef aralikta kalma karti - genislik, en kotu durumdaki (3 haneli
  // "100%") satirin gercek piksel genisligi calisma zamaninda cv->textWidth()
  // ile olculup, "70"nin solundaki sabit bosluk (hkPad) kadar sagdan da pay
  // birakilarak hesaplaniyor - boylece kutu olabilecek en genis metni
  // (yuzde/birim ne olursa olsun) tam kapsayacak minimum genislikte kalir.
  int hkPad = 14; // "70"nin solundaki mevcut sabit bosluk
  char worst1[24], worst2[24];
  sprintf(worst1, "%s-%s: 100%%", formatGlucose(70).c_str(), formatGlucose(180).c_str());
  sprintf(worst2, "%s-%s: 100%%", formatGlucose(70).c_str(), formatGlucose(140).c_str());
  cv->setTextFont(4);
  int worstW = max(cv->textWidth(worst1), cv->textWidth(worst2));
  cv->setTextFont(1);
  int hkW = worstW + hkPad * 2;
  int hkX = (320 - hkW) / 2;
  cv->drawRoundRect(hkX, 168, hkW, 64, 12, 0x2945);
  sprintf(buf, "%s-%s: %d%%", formatGlucose(70).c_str(), formatGlucose(180).c_str(), pct70180);
  cv->setTextColor(pct70180 >= 70 ? sevColor(2) : sevColor(1), UI_BG);
  dsN(cv, buf, hkX + hkPad, 172, 4);
  sprintf(buf, "%s-%s: %d%%", formatGlucose(70).c_str(), formatGlucose(140).c_str(), pct70140);
  cv->setTextColor(pct70140 >= 50 ? sevColor(2) : sevColor(1), UI_BG);
  dsN(cv, buf, hkX + hkPad, 202, 4);
}

void drawGuideFooterDots() {
  int total = GUIDE_PAGES;
  int startX = 160 - (total * 16) / 2 + 8;
  for (int i = 0; i < total; i++) {
    uint16_t c = (i == guidePage) ? TFT_WHITE : TFT_DARKGREY;
    cv->fillCircle(startX + i * 16, 218, (i == guidePage) ? 4 : 3, c);
  }
  cv->setTextColor(TFT_DARKGREY, UI_BG);
  cv->setTextDatum(BC_DATUM);
  drawFreeTitle(L("dokun: ileri", "tap: next"), 160, 235, &FreeSansTR6pt);
}

void drawGuidePage() {
  cv->fillScreen(UI_BG);
  cv->setTextDatum(TL_DATUM);
  if (guidePage == 0) {
    drawWarnIcon(30, 20, sevColor(0), TFT_BLACK);
    cv->setTextColor(sevColor(0), UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("1. ÖNEMLİ UYARI", "1. IMPORTANT"), 172, 6, &FreeSansTR9pt);
    cv->drawFastHLine(12, 40, 296, TFT_DARKGREY);
    cv->setTextColor(sevColor(3), UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(L("İnsülin veya tedavi kararı için", "Do not use for insulin or"), 12, 54, &FreeSansTR7pt);
    drawFreeTitle(L("KULLANMAYIN.", "treatment decisions."), 12, 78, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("- Tıbbi cihaz değildir", "- Not a medical device"), 12, 112, &FreeSansTR7pt);
    drawFreeTitle(L("- Sadece bilgilendirme amaçlıdır", "- Informational purposes only"), 12, 136, &FreeSansTR7pt);
    drawFreeTitle(L("- Risk kullanıcıya aittir", "- Risk is the user's"), 12, 160, &FreeSansTR7pt);
    cv->setTextColor(sevColor(1), UI_BG);
    drawFreeTitle(L("Resmi CGM uygulamanızı esas alın.", "Rely on your official CGM app."), 12, 190, &FreeSansTR6pt);
  } else if (guidePage == 1) {
    drawBookIcon(30, 20, TFT_CYAN);
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("2. SAYFALAR", "2. PAGES"), 172, 6, &FreeSansTR9pt);
    cv->drawFastHLine(12, 40, 296, TFT_DARKGREY);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(L("1  Ana şeker + trend + delta", "1  Glucose + trend + delta"), 12, 52, &FreeSansTR7pt);
    drawFreeTitle(L("2  Arayüz (Basit/Büyük/Fotoğraf..)", "2  UI (Simple/Big/Photo..)"), 12, 76, &FreeSansTR7pt);
    drawFreeTitle(L("3  Grafik 3/6/12/24 saat", "3  Graph 3/6/12/24 h"), 12, 100, &FreeSansTR7pt);
    drawFreeTitle(L("4  İstatistik (gün boyu)", "4  Daily statistics"), 12, 124, &FreeSansTR7pt);
    drawFreeTitle(L("5  Zamanlayıcı", "5  Timer"), 12, 148, &FreeSansTR7pt);
    drawFreeTitle(L("6  Ayarlar", "6  Settings"), 12, 172, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("Geçiş: ekrana dokun (sol/sağ)", "Navigate: tap left / right"), 12, 198, &FreeSansTR6pt);
  } else if (guidePage == 2) {
    drawMoreIcon(30, 20, TFT_CYAN);
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("3. AYARLAR", "3. SETTINGS"), 172, 6, &FreeSansTR9pt);
    cv->drawFastHLine(12, 40, 296, TFT_DARKGREY);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(L("Üst: Parlaklık | Ses | Alarm", "Top: Bright | Sound | Alarm"), 12, 52, &FreeSansTR7pt);
    drawFreeTitle(L("Alt: Hedefler | QR | Diğer", "Bot: Targets | QR | More"), 12, 76, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("Hedefler: kritik/düşük/yüksek", "Targets: crit/low/high limits"), 12, 106, &FreeSansTR7pt);
    drawFreeTitle(L("Alarm: görsel, ertele, modlar", "Alarm: visual, snooze, modes"), 12, 130, &FreeSansTR7pt);
    drawFreeTitle(L("QR: telefondan web ayar", "QR: phone web setup"), 12, 154, &FreeSansTR7pt);
    drawFreeTitle(L("BOOT 2 sn: WiFi / kaynak", "BOOT 2s: WiFi / source"), 12, 178, &FreeSansTR7pt);
  } else if (guidePage == 3) {
    drawBellIcon(30, 20, TFT_CYAN);
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("4. MODLAR + DİKKAT", "4. MODES + ALERT"), 172, 6, &FreeSansTR9pt);
    cv->drawFastHLine(12, 40, 296, TFT_DARKGREY);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(L("Okul / Gece: saat + ses/ışık", "School/Night: hours + vol/bright"), 12, 52, &FreeSansTR7pt);
    drawFreeTitle(L("Rehber: Dikkat butonu", "Guide: Alert button"), 12, 76, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("  Hipo/hiper kartları eşiğe göre", "  Cards follow your thresholds"), 12, 100, &FreeSansTR7pt);
    drawFreeTitle(L("  Bilinç yoksa Glukagon rehberi", "  Unconscious: Glucagon guide"), 12, 124, &FreeSansTR7pt);
    cv->setTextColor(TFT_WHITE, UI_BG);
    drawFreeTitle(L("Ertele: 15 / 30 / 60 dk", "Snooze: 15 / 30 / 60 min"), 12, 154, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("Kaynak: NS / Dexcom / Libre / Medtrum", "Source: NS / Dexcom / Libre / Medtrum"), 12, 178, &FreeSansTR7pt);
  } else if (guidePage == 4) {
    drawGlobeIcon(30, 20, TFT_CYAN);
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("5. WEB + OTA", "5. WEB + OTA"), 172, 6, &FreeSansTR9pt);
    cv->drawFastHLine(12, 40, 296, TFT_DARKGREY);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(L("Aynı WiFi'de tarayıcı açın:", "Same WiFi, open a browser:"), 12, 52, &FreeSansTR7pt);
    cv->setTextColor(sevColor(2), UI_BG);
    dsN(cv, String("  http://") + L("CIHAZ-IP", "DEVICE-IP") + "/", 12, 80, 2);
    dsN(cv, String("  http://") + L("CIHAZ-IP", "DEVICE-IP") + "/update", 12, 104, 2);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("QR ile de açabilirsiniz.", "Or scan the on-device QR."), 12, 134, &FreeSansTR7pt);
    drawFreeTitle(L("OTA: .bin dosyasını yükleyin.", "OTA: upload the .bin file."), 12, 158, &FreeSansTR7pt);
    drawFreeTitle(L("Bilgi sayfasında IP yazar.", "IP is shown on the Info page."), 12, 182, &FreeSansTR7pt);
  } else {
    // 6. sayfa: WiFi kurulumu - ozellikle telefonun KENDI hotspot'unu
    // hedef ag olarak kullanan aileler icin (kullanicinin bildirdigi "ikinci
    // telefon gerekiyor" sorunu). Kurulum ekranindaki QR sade kalsin diye
    // bu adim adim yonlendirme oraya degil, buraya eklendi.
    drawQrIcon(30, 20, TFT_CYAN);
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("6. WiFi KURULUMU", "6. WIFI SETUP"), 172, 6, &FreeSansTR9pt);
    cv->drawFastHLine(12, 40, 296, TFT_DARKGREY);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(L("QR'ı taratın veya \"metematik\"", "Scan the QR, or connect to"), 12, 52, &FreeSansTR7pt);
    drawFreeTitle(L("ağına elle bağlanın.", "\"metematik\" manually."), 12, 76, &FreeSansTR7pt);
    cv->setTextColor(sevColor(1), UI_BG);
    drawFreeTitle(L("Kendi hotspot'unuzu mu", "Using your own phone's"), 12, 106, &FreeSansTR7pt);
    drawFreeTitle(L("kullanacaksınız?", "hotspot as the network?"), 12, 130, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("1. Hotspot'u kapatın", "1. Turn your hotspot off"), 12, 156, &FreeSansTR6pt);
    drawFreeTitle(L("2. metematik-.... ağına bağlanıp doldurun", "2. Connect to metematik-...., fill it in"), 12, 176, &FreeSansTR6pt);
    drawFreeTitle(L("3. Hotspot'u tekrar açın", "3. Turn your hotspot back on"), 12, 196, &FreeSansTR6pt);
  }
  drawGuideFooterDots();
}

void drawAlarmSetupScreen() {
  cv->fillScreen(UI_BG);
  // Not: 10 (Hedefler izgarasi) ve esik duzenleme adimlari (1/2/4/5/7) kendi
  // basliklarini ciziyor; genel "ALARM"/"HEDEFLER" basligini burada tekrar
  // cizmiyoruz, yoksa ust uste iki baslik gorunuyordu.
  bool ownHeader = (alarmEditStep == 3 || alarmEditStep == 10 || alarmEditStep == 1 || alarmEditStep == 2 ||
                    alarmEditStep == 4 || alarmEditStep == 5 || alarmEditStep == 7 ||
                    alarmEditStep == 21 || alarmEditStep == 22);
  if (!ownHeader) {
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    if (subScreen == 18) drawFreeTitle(L("HEDEFLER", "TARGETS"), 160, 4, &FreeSansTR9pt);
    else drawFreeTitle(L("ALARM", "ALARM"), 160, 4, &FreeSansTR9pt);
  }
  char buf[48];

  if (alarmEditStep == 0) {
    // Ust satir (3): Gorsel | Ertele | Modlar. Alt satir (2): Ses Bastirma | Geri
    // (Ses Bastirma, Kullanici istegiyle Geri'nin SOLUNA eklendi.)
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Ses seviyesi: Ayarlar > Ses", "Volume: Settings > Sound"), 160, 28, &FreeSansTR6pt);

    const char* labs3[] = { L("Görsel", "Visual"), L("Ertele", "Snooze"), L("Modlar", "Modes") };
    for (int i = 0; i < 3; i++) {
      int x = 12 + i * 102, y = 48;
      uint16_t bg = 0x4B72;
      if (i == 0 && visualAlarmOn) bg = getAlarmColor();
      if (i == 0 && !visualAlarmOn) bg = 0x4208;
      if (i == 2 && (nightModeOn || schoolModeOn || teacherModeOn)) bg = 0x5010;
      cv->fillRoundRect(x, y, 92, 50, 8, bg);
      cv->drawRoundRect(x, y, 92, 50, 8, 0x4A69);
      uint16_t fg = (i == 0 && visualAlarmOn) ? TFT_BLACK : TFT_WHITE;
      int cx = x + 46, iconY = y + 16;
      switch (i) {
        case 0: drawFlashIcon(cx, iconY, fg); break;
        case 1: drawClockIcon(cx, iconY, fg); break;
        case 2: drawGridIcon(cx, iconY, fg); break;
      }
      cv->setTextColor(fg, bg);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(labs3[i], cx, y + 38, &FreeSansTR7pt);
    }

    {
      // Alt satir (3): Ses Bastirma | Cift Ok | Geri - ust satirla ayni izgara
      int y2 = 110, h2 = 58;
      uint16_t accent = cv->color565(0x4A, 0x8C, 0xE0);
      // Ses Bastirma - acikken marka mavisi, kapaliyken sonuk
      uint16_t bgS = recoverySoundSuppressOn ? accent : 0x4208;
      cv->fillRoundRect(12, y2, 92, h2, 8, bgS);
      cv->drawRoundRect(12, y2, 92, h2, 8, 0x4A69);
      int cxS = 12 + 46, iconYS = y2 + 18;
      if (recoverySoundSuppressOn) drawSoundIcon(cxS, iconYS, TFT_WHITE);
      else drawMuteIcon(cxS, iconYS, TFT_WHITE);
      cv->setTextColor(TFT_WHITE, bgS);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(L("Ses Bastırma", "Suppress"), cxS, y2 + 44, &FreeSansTR7pt);

      // Cift Ok (hizli dusus/yukselis) - biri aciksa marka mavisi
      bool anyFast = alarmFastFallOn || alarmFastRiseOn;
      uint16_t bgF = anyFast ? accent : 0x4208;
      cv->fillRoundRect(114, y2, 92, h2, 8, bgF);
      cv->drawRoundRect(114, y2, 92, h2, 8, 0x4A69);
      int cxF = 114 + 46, iconYF = y2 + 18;
      drawDoubleChevronDown(cxF - 10, iconYF, alarmFastFallOn ? TFT_WHITE : 0x8410);
      drawDoubleChevronUp(cxF + 10, iconYF, alarmFastRiseOn ? TFT_WHITE : 0x8410);
      cv->setTextColor(TFT_WHITE, bgF);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(L("Çift Ok", "Fast Arrows"), cxF, y2 + 44, &FreeSansTR7pt);

      // Geri
      cv->fillRoundRect(216, y2, 92, h2, 8, 0x333333);
      cv->drawRoundRect(216, y2, 92, h2, 8, 0x4A69);
      int cxG = 216 + 46, iconYG = y2 + 18;
      drawBackIcon(cxG, iconYG, TFT_WHITE);
      cv->setTextColor(TFT_WHITE, 0x333333);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(L("Geri", "Back"), cxG, y2 + 44, &FreeSansTR7pt);
    }

  } else if (alarmEditStep == 22) {
    // Cift ok alarmlari: aciklama + iki ac/kapa + Geri
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("ÇİFT OK ALARMLARI", "FAST ARROW ALARMS"), 160, 6, &FreeSansTR7pt);
    cv->drawFastHLine(12, 28, 296, 0x2104);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(L("Şeker çok hızlı değişirken (çift ok)", "When glucose changes very fast"), 12, 36, &FreeSansTR6pt);
    drawFreeTitle(L("eşik dışına çıkmadan önce uyarır.", "(double arrow), warns before limits."), 12, 54, &FreeSansTR6pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("Nightscout ve Dexcom'da çalışır.", "Works with Nightscout and Dexcom."), 12, 72, &FreeSansTR6pt);

    uint16_t accent = cv->color565(0x4A, 0x8C, 0xE0);
    for (int r = 0; r < 2; r++) {
      int y = 100 + r * 46;
      bool on = (r == 0) ? alarmFastFallOn : alarmFastRiseOn;
      cv->drawRoundRect(12, y, 296, 40, 8, 0x2945);
      if (r == 0) drawDoubleChevronDown(34, y + 20, sevColor(0));
      else drawDoubleChevronUp(34, y + 20, sevColor(3));
      cv->setTextColor(TFT_WHITE, UI_BG);
      cv->setTextDatum(ML_DATUM);
      drawFreeTitle(r == 0 ? L("Hızlı düşüş", "Fast fall") : L("Hızlı yükseliş", "Fast rise"), 56, y + 20, &FreeSansTR7pt);
      uint16_t tbg = on ? accent : 0x4208;
      cv->fillRoundRect(206, y + 5, 94, 30, 8, tbg);
      cv->setTextColor(TFT_WHITE, tbg);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(on ? L("AÇIK", "ON") : L("KAPALI", "OFF"), 253, y + 20, &FreeSansTR7pt);
    }

    cv->fillRoundRect(110, 196, 100, 34, 8, 0x333333);
    cv->drawRoundRect(110, 196, 100, 34, 8, 0x4A69);
    cv->setTextColor(TFT_WHITE, 0x333333);
    drawBackIcon(128, 213, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Geri", "Back"), 144, 213, &FreeSansTR7pt);

  } else if (alarmEditStep == 21) {
    // Ses Bastirma aciklama + ac/kapa
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("SES BASTIRMA", "SOUND SUPPRESS"), 160, 6, &FreeSansTR7pt);
    cv->drawFastHLine(12, 28, 296, 0x2104);

    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(L("Şeker eşik dışında olsa da", "Even if it's out of range,"), 12, 38, &FreeSansTR6pt);
    drawFreeTitle(L("normale belirgin gidiyorsa", "if it's clearly heading back"), 12, 56, &FreeSansTR6pt);
    drawFreeTitle(L("(en az 5 mg/dl fark),", "(5+ mg/dl change),"), 12, 74, &FreeSansTR6pt);
    drawFreeTitle(L("SES otomatik susar.", "SOUND mutes automatically."), 12, 92, &FreeSansTR6pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("Görsel alarm (renk/yanıp", "Visual alarm (color/blink)"), 12, 110, &FreeSansTR6pt);
    drawFreeTitle(L("sönme) aynen devam eder.", "still continues as normal."), 12, 128, &FreeSansTR6pt);
    cv->setTextColor(sevColor(3), UI_BG);
    drawFreeTitle(L("Kritik Düşük'te ASLA", "Never applies to Critical Low -"), 12, 146, &FreeSansTR6pt);
    drawFreeTitle(L("geçerli değil, her zaman çalar.", "that alarm always sounds."), 12, 164, &FreeSansTR6pt);

    uint16_t accent = cv->color565(0x4A, 0x8C, 0xE0);
    uint16_t tbg = recoverySoundSuppressOn ? accent : 0x4208;
    cv->fillRoundRect(40, 190, 150, 34, 8, tbg);
    cv->drawRoundRect(40, 190, 150, 34, 8, 0x4A69);
    cv->setTextColor(TFT_WHITE, tbg);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(recoverySoundSuppressOn ? L("AÇIK", "ON") : L("KAPALI", "OFF"), 115, 207, &FreeSansTR7pt);

    cv->fillRoundRect(200, 190, 80, 34, 8, 0x333333);
    cv->drawRoundRect(200, 190, 80, 34, 8, 0x4A69);
    cv->setTextColor(TFT_WHITE, 0x333333);
    drawBackIcon(216, 207, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Geri", "Back"), 232, 207, &FreeSansTR7pt);

  } else if (alarmEditStep == 20) {
    // Modlar: Okul / Gece / Ogretmen / Geri
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("MODLAR", "MODES"), 160, 28, &FreeSansTR7pt);
    const char* mlabs[] = {
      L("Okul Modu", "School Mode"), L("Gece Modu", "Night Mode"),
      L("Rehber Modu", "Guide Mode"), L("Geri", "Back")
    };
    for (int i = 0; i < 4; i++) {
      int x = 30 + (i % 2) * 140;
      int y = 60 + (i / 2) * 60;
      uint16_t bg = 0x4B72;
      if (i == 0 && schoolModeOn) bg = 0x5010;
      if (i == 1 && nightModeOn) bg = 0x2010;
      if (i == 2) bg = 0x780F;
      if (i == 3) bg = 0x333333;
      cv->fillRoundRect(x, y, 120, 48, 8, bg);
      cv->drawRoundRect(x, y, 120, 48, 8, 0x4A69);
      int cx = x + 60, iconY = y + 14;
      switch (i) {
        case 0: drawBookIcon(cx, iconY, TFT_WHITE); break;
        case 1: drawMoonIcon(cx, iconY, TFT_WHITE, bg); break;
        case 2: drawBellIcon(cx, iconY, TFT_WHITE); break;
        case 3: drawBackIcon(cx, iconY, TFT_WHITE); break;
      }
      cv->setTextColor(TFT_WHITE, bg);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(mlabs[i], x + 60, y + 35, &FreeSansTR7pt);
    }

  } else if (alarmEditStep == 10) {
    // 3+3: Kritik Dusuk Yuksek | CokYuksek VeriYok Geri
    drawTargetIcon(20, 12, TFT_CYAN);
    cv->setTextColor(TFT_CYAN, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("HEDEFLER", "TARGETS"), 168, 4, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    snprintf(buf, sizeof(buf), L("K<%s  D<%s  Y>%s  CY>%s  St%ddk", "C<%s  L<%s  H>%s  VH>%s  St%dm"),
            formatGlucose(alarmCritLow).c_str(), formatGlucose(alarmLow).c_str(),
            formatGlucose(alarmHigh).c_str(), formatGlucose(alarmVeryHigh).c_str(), staleAlarmMin);
    dsN(cv, buf, 160, 24, 1);
    cv->drawFastHLine(12, 40, 296, 0x2104);
    const char* labs[] = {
      L("Kritik", "Critical"), L("Düşük", "Low"), L("Yüksek", "High"),
      L("Çok yüksek", "Very high"), L("Veri yok", "No data"), L("Geri", "Back")
    };
    const uint16_t iconCol[] = { sevColor(0), sevColor(1), sevColor(3), sevColor(0), sevColor(1), TFT_WHITE };
    for (int i = 0; i < 6; i++) {
      int col = i / 3;
      int row = i % 3;
      int x = 16 + col * 152;
      int y = 48 + row * 52;
      uint16_t bg = (i == 5) ? 0x333333 : 0x4B72;
      cv->fillRoundRect(x, y, 136, 44, 10, bg);
      cv->drawRoundRect(x, y, 136, 44, 10, 0x4A69);
      int iconX = x + 20, iconY = y + 22;
      switch (i) {
        case 0: drawDoubleChevronDown(iconX, iconY, iconCol[i]); break;
        case 1: drawChevronDown(iconX, iconY, iconCol[i]); break;
        case 2: drawChevronUp(iconX, iconY, iconCol[i]); break;
        case 3: drawDoubleChevronUp(iconX, iconY, iconCol[i]); break;
        case 4: drawNoDataIcon(iconX, iconY, iconCol[i]); break;
        case 5: drawBackIcon(iconX, iconY, iconCol[i]); break;
      }
      cv->setTextColor(TFT_WHITE, bg);
      cv->setTextDatum(ML_DATUM);
      drawFreeTitle(labs[i], x + 36, y + 22, &FreeSansTR7pt);
    }

  } else if (alarmEditStep == 1 || alarmEditStep == 2 || alarmEditStep == 4 || alarmEditStep == 7) {
    const char* title = (alarmEditStep == 1) ? L("Düşük eşik", "Low threshold") :
                        (alarmEditStep == 2) ? L("Yüksek eşik", "High threshold") :
                        (alarmEditStep == 7) ? L("Çok yüksek eşik", "Very high threshold") : L("Kritik düşük eşik", "Critical low threshold");
    int* val = (alarmEditStep == 1) ? &alarmLow :
               (alarmEditStep == 2) ? &alarmHigh :
               (alarmEditStep == 7) ? &alarmVeryHigh : &alarmCritLow;
    bool isMin = (alarmEditStep == 1 || alarmEditStep == 4); // "bu deger altinda" mi
    uint16_t theme = (alarmEditStep == 1) ? sevColor(1) : (alarmEditStep == 2) ? sevColor(3) : sevColor(0);
    bool* sndOn = (alarmEditStep == 1) ? &alarmLowSound :
                  (alarmEditStep == 2) ? &alarmHighSound :
                  (alarmEditStep == 7) ? &alarmVeryHighSound : &alarmCritLowSound;
    cv->setTextColor(theme, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(title, 172, 6, &FreeSansTR7pt);
    if (alarmEditStep == 1) drawChevronDown(24, 15, theme);
    else if (alarmEditStep == 2) drawChevronUp(24, 15, theme);
    else if (alarmEditStep == 7) drawDoubleChevronUp(24, 15, theme);
    else drawDoubleChevronDown(24, 15, theme);
    // Bu esik icin ses ac/kapa - sag ust kose, dokununca degisir (asagida
    // touch handler'da). Kapaliyken gorsel alarm (varsa) yine calisir.
    {
      uint16_t sndBg = *sndOn ? theme : 0x4208;
      cv->fillRoundRect(280, 2, 36, 26, 8, sndBg);
      cv->drawRoundRect(280, 2, 36, 26, 8, 0x4A69);
      if (*sndOn) drawSoundIcon(298, 15, TFT_BLACK);
      else drawMuteIcon(298, 15, TFT_WHITE);
    }
    cv->drawFastHLine(16, 30, 288, 0x2104);

    cv->drawRoundRect(64, 40, 192, 74, 16, theme);
    cv->setTextColor(theme, UI_BG);
    cv->setTextDatum(MC_DATUM);
    sprintf(buf, "%d", *val);
    cv->drawString(buf, 160, 71, 6);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    dsN(cv, isMin ? L("bu degerin altinda - dokun: yaz", "below this value - tap to type") :
                           L("bu degerin ustunde - dokun: yaz", "above this value - tap to type"), 160, 104, 1);

    cv->fillRoundRect(30, 128, 110, 42, 16, 0x333333);
    cv->drawRoundRect(30, 128, 110, 42, 16, theme);
    cv->setTextColor(TFT_WHITE, 0x333333);
    cv->setTextDatum(MC_DATUM);
    dsN(cv, "-5", 85, 149, 4);
    cv->fillRoundRect(180, 128, 110, 42, 16, 0x333333);
    cv->drawRoundRect(180, 128, 110, 42, 16, theme);
    dsN(cv, "+5", 235, 149, 4);

    cv->fillRoundRect(70, 188, 180, 34, 12, 0x0B5E);
    cv->drawRoundRect(70, 188, 180, 34, 12, theme);
    cv->setTextColor(TFT_WHITE, 0x0B5E);
    drawCheckIcon(106, 205, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Tamam", "OK"), 124, 205, &FreeSansTR7pt);

  } else if (alarmEditStep == 5) {
    cv->setTextColor(sevColor(1), UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Veri yok alarmı", "No-data alarm"), 172, 6, &FreeSansTR7pt);
    drawNoDataIcon(24, 15, sevColor(1));
    // Diger esiklerle ayni ses ac/kapa dugmesi - sag ust kose
    {
      uint16_t sndBg = alarmStaleSound ? sevColor(1) : 0x4208;
      cv->fillRoundRect(280, 2, 36, 26, 8, sndBg);
      cv->drawRoundRect(280, 2, 36, 26, 8, 0x4A69);
      if (alarmStaleSound) drawSoundIcon(298, 15, TFT_BLACK);
      else drawMuteIcon(298, 15, TFT_WHITE);
    }
    cv->drawFastHLine(16, 30, 288, 0x2104);

    cv->drawRoundRect(64, 40, 192, 74, 16, sevColor(1));
    sprintf(buf, "%d", staleAlarmMin);
    cv->setTextColor(sevColor(1), UI_BG);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(buf, 160, 71, 6);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("dakika veri gelmezse uyar", "minutes without data"), 160, 104, &FreeSansTR6pt);

    cv->fillRoundRect(30, 128, 110, 42, 16, 0x333333);
    cv->drawRoundRect(30, 128, 110, 42, 16, sevColor(1));
    cv->setTextColor(TFT_WHITE, 0x333333);
    cv->setTextDatum(MC_DATUM);
    dsN(cv, "-5", 85, 149, 4);
    cv->fillRoundRect(180, 128, 110, 42, 16, 0x333333);
    cv->drawRoundRect(180, 128, 110, 42, 16, sevColor(1));
    dsN(cv, "+5", 235, 149, 4);

    cv->fillRoundRect(70, 188, 180, 34, 12, 0x0B5E);
    cv->drawRoundRect(70, 188, 180, 34, 12, sevColor(1));
    cv->setTextColor(TFT_WHITE, 0x0B5E);
    drawCheckIcon(106, 205, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Tamam", "OK"), 124, 205, &FreeSansTR7pt);

  } else if (alarmEditStep == 3) {
    drawVisualAlarmPage();

  } else if (alarmEditStep == 6) {
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("ERTELEME", "SNOOZE"), 160, 20, &FreeSansTR7pt);
    // Izgara modeli: 15/30/60 dk + Kapali, ardindan Geri
    const int opts[] = {15, 30, 60, 0};
    const char* olabs[] = {L("15 dk", "15 min"), L("30 dk", "30 min"), L("60 dk", "60 min")};
    for (int i = 0; i < 4; i++) {
      int x = 30 + (i % 2) * 140;
      int y = 55 + (i / 2) * 58;
      bool sel = (snoozeMinutes == opts[i]);
      uint16_t bg = sel ? SEL_BG : UNSEL_BG;
      uint16_t fg = sel ? SEL_FG : UNSEL_FG;
      cv->fillRoundRect(x, y, 120, 48, 8, bg);
      cv->drawRoundRect(x, y, 120, 48, 8, sel ? TFT_WHITE : 0x4A69);
      int cx = x + 60, iconY = y + 14;
      cv->setTextColor(fg, bg);
      cv->setTextDatum(MC_DATUM);
      if (i == 3) {
        drawXIcon(cx, iconY, fg);
        drawFreeTitle(L("Kapalı", "Off"), x + 60, y + 35, &FreeSansTR7pt);
      } else {
        drawClockIcon(cx, iconY, fg);
        drawFreeTitle(olabs[i], x + 60, y + 35, &FreeSansTR7pt);
      }
    }
    cv->fillRoundRect(80, 195, 160, 32, 6, 0x333333);
    cv->drawRoundRect(80, 195, 160, 32, 6, 0x555555);
    cv->setTextColor(TFT_WHITE, 0x333333);
    drawBackIcon(104, 211, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Geri", "Back"), 120, 211, &FreeSansTR7pt);
  }
}

void drawSoundScreen() {
  cv->fillScreen(UI_BG);
  drawSoundIcon(20, 18, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("SESLİ ALARM", "SOUND"), 172, 10, &FreeSansTR9pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  drawFreeTitle(L("Seviye seç (test çalar)", "Pick level (plays test)"), 160, 40, &FreeSansTR7pt);
  cv->drawFastHLine(10, 58, 300, 0x2104);
  const char* labels[] = {L("Sessiz", "Mute"), L("Düşük", "Low"), L("Orta", "Med"), L("Yüksek", "High")};
  for (int i = 0; i < 4; i++) {
    int x = 12 + (i % 2) * 154;
    int y = 70 + (i / 2) * 55;
    uint16_t bg = (soundLevel == i) ? SEL_BG : UNSEL_BG;
    uint16_t fg = (soundLevel == i) ? SEL_FG : UNSEL_FG;
    cv->fillRoundRect(x, y, 142, 48, 8, bg);
    cv->drawRoundRect(x, y, 142, 48, 8, (soundLevel == i) ? TFT_WHITE : 0x4A69);
    if (i == 0) drawMuteIcon(x + 20, y + 24, fg);
    else drawSoundLevelIcon(x + 20, y + 24, fg, i - 1);
    cv->setTextColor(fg, bg);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(labels[i], x + 38, y + 24, &FreeSansTR7pt);
  }
  cv->fillRoundRect(80, 195, 160, 36, 8, 0x4B72);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  drawBackIcon(104, 213, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 120, 213, &FreeSansTR7pt);
}

void drawBrightnessScreen() {
  cv->fillScreen(UI_BG);
  drawBrightnessIcon(20, 18, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("PARLAKLIK", "BRIGHTNESS"), 172, 10, &FreeSansTR9pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  drawFreeTitle(L("Seviye seç", "Select level"), 160, 42, &FreeSansTR7pt);
  // Alarm menusuyle ayni 2x2 ortali izgara: Dusuk / Orta / Yuksek / Geri
  const char* labels[] = {L("Düşük", "Low"), L("Orta", "Medium"), L("Yüksek", "High")};
  for (int i = 0; i < 4; i++) {
    int x = 30 + (i % 2) * 140;
    int y = 60 + (i / 2) * 60;
    bool sel = (i < 3 && brightnessLevel == i);
    uint16_t bg = (i == 3) ? 0x333333 : (sel ? SEL_BG : UNSEL_BG);
    uint16_t fg = (i == 3) ? TFT_WHITE : (sel ? SEL_FG : UNSEL_FG);
    cv->fillRoundRect(x, y, 120, 48, 8, bg);
    cv->drawRoundRect(x, y, 120, 48, 8, sel ? TFT_WHITE : 0x4A69);
    int cx = x + 60, iconY = y + 14;
    cv->setTextColor(fg, bg);
    cv->setTextDatum(MC_DATUM);
    if (i == 3) {
      drawBackIcon(cx, iconY, fg);
      drawFreeTitle(L("Geri", "Back"), x + 60, y + 35, &FreeSansTR7pt);
    } else {
      drawBrightnessLevelIcon(cx, iconY, fg, i);
      drawFreeTitle(labels[i], x + 60, y + 35, &FreeSansTR7pt);
    }
  }
}

// Faz 1: QR / web adres ekrani (telefon ayni WiFi)
void drawQrScreen() {
  cv->fillScreen(UI_BG);
  drawListHeader(L("QR / WEB", "QR / WEB"), false);
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  String url = ip.length() ? ("http://" + ip + "/") : "";
  if (url.length() == 0) {
    cv->drawRoundRect(8, 50, 157, 100, 12, sevColor(0));
    drawWarnIcon(86, 84, sevColor(0), TFT_BLACK);
    cv->setTextColor(sevColor(0), UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("WiFi", "WiFi"), 86, 112, &FreeSansTR7pt);
    drawFreeTitle(L("bağlı değil", "not connected"), 86, 132, &FreeSansTR6pt);
  } else {
    QRCode qrcode;
    uint8_t qrbuf[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrbuf, 3, ECC_LOW, url.c_str());
    int scale = 5, qsize = qrcode.size * scale, ox = 14, oy = 50;
    cv->fillRoundRect(ox - 6, oy - 6, qsize + 12, qsize + 12, 10, TFT_WHITE);
    for (uint8_t y = 0; y < qrcode.size; y++)
      for (uint8_t x = 0; x < qrcode.size; x++)
        if (qrcode_getModule(&qrcode, x, y)) cv->fillRect(ox + x * scale, oy + y * scale, scale, scale, TFT_BLACK);
    int qcx = ox + qsize / 2, qbot = oy + qsize;
    cv->setTextColor(sevColor(2), UI_BG);
    cv->setTextDatum(TC_DATUM);
    dsN(cv, ip, qcx, qbot + 10, 1);
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    drawFreeTitle(L("Telefondan okut", "Scan with phone"), qcx, qbot + 24, &FreeSansTR6pt);
  }
  int bx = 176;
  cv->setTextDatum(TL_DATUM);
  cv->setTextColor(TFT_CYAN, UI_BG);
  drawFreeTitle(L("Telefondan ayar", "Phone settings"), bx, 46, &FreeSansTR7pt);
  cv->setTextColor(TFT_WHITE, UI_BG);
  dsN(cv, deviceName() + ".local", bx, 68, 1);
  if (ip.length()) dsN(cv, ip, bx, 80, 1);
  cv->setTextColor(UI_SUBTEXT, UI_BG);
  drawFreeTitle(L("Web şifresi", "Web password"), bx, 96, &FreeSansTR6pt);
  cv->setTextColor(TFT_WHITE, UI_BG);
  drawPwSpaced(webPass, bx + 68, 110, 16, &lgfx::fonts::FreeMonoBold12pt7b, TFT_WHITE, UI_BG);   // 3.4.6
  const char* btn[2] = {L("Bilgi", "Info"), L("Kılavuz", "Guide")};
  for (int i = 0; i < 2; i++) {
    int by = 146 + i * 46;
    cv->fillRoundRect(bx, by, 136, 40, 10, UI_CARD);
    cv->fillCircle(bx + 22, by + 20, 13, i == 0 ? (uint16_t)0x5B75 : (uint16_t)0x93C7);
    if (i == 0) drawInfoIconM(bx + 22, by + 20, TFT_WHITE); else drawBookIcon(bx + 22, by + 20, TFT_WHITE);
    cv->setTextColor(TFT_WHITE, UI_CARD);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(btn[i], bx + 44, by + 21, &FreeSansTR9pt);
  }
  cv->setTextDatum(TL_DATUM);
}

// 4.0.1: Modlar > Aile modu: web sayfasinin QR'i + aciklama
int famQrFrom = -1;
void drawFamQrScreen() {
  cv->fillScreen(UI_BG);
  drawListHeader(L("AİLE MODU", "FAMILY MODE"), false);
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  String url = ip.length() ? ("http://" + ip + "/#famcard") : "";
  if (url.length() == 0) {
    cv->drawRoundRect(8, 50, 157, 100, 12, sevColor(0));
    drawWarnIcon(86, 84, sevColor(0), TFT_BLACK);
    cv->setTextColor(sevColor(0), UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("WiFi", "WiFi"), 86, 112, &FreeSansTR7pt);
    drawFreeTitle(L("bağlı değil", "not connected"), 86, 132, &FreeSansTR6pt);
  } else {
    QRCode qrcode;
    uint8_t qrbuf[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrbuf, 3, ECC_LOW, url.c_str());
    int scale = 5, qsize = qrcode.size * scale, ox = 14, oy = 50;
    cv->fillRoundRect(ox - 6, oy - 6, qsize + 12, qsize + 12, 10, TFT_WHITE);
    for (uint8_t y = 0; y < qrcode.size; y++)
      for (uint8_t x = 0; x < qrcode.size; x++)
        if (qrcode_getModule(&qrcode, x, y)) cv->fillRect(ox + x * scale, oy + y * scale, scale, scale, TFT_BLACK);
    int qcx = ox + qsize / 2, qbot = oy + qsize;
    cv->setTextColor(sevColor(2), UI_BG);
    cv->setTextDatum(TC_DATUM);
    dsN(cv, ip, qcx, qbot + 10, 1);
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    drawFreeTitle(L("Telefondan okut", "Scan with phone"), qcx, qbot + 24, &FreeSansTR6pt);
  }
  int bx = 176;
  cv->setTextDatum(TL_DATUM);
  cv->setTextColor(famEnabled ? sevColor(2) : (uint16_t)UI_SUBTEXT, UI_BG);
  String st = famEnabled ? (String(L("Açık", "On")) + " - " + String(famCount) + L(" kişi", " people")) : String(L("Kapalı", "Off"));
  drawFreeTitle(st.c_str(), bx, 46, &FreeSansTR9pt);
  cv->setTextColor(TFT_WHITE, UI_BG);
  const char* lines[] = {
    L("En fazla 3 kişinin", "Follow up to 3"),
    L("şekeri izlenebilir.", "people's glucose."),
    L("Kaynak ve eşik", "Scan the QR to set"),
    L("ayarları için", "sources and limits"),
    L("QR'ı okutun.", "for each person."),
  };
  for (int i = 0; i < 5; i++) drawFreeTitle(lines[i], bx, 70 + i * 16, &FreeSansTR7pt);
  if (famEnabled) {                                  // 4.0.2: ana ekran secimi buradan da
    const char* mn[2] = {L("Sırayla", "In turn"), L("Bölünmüş", "Split")};
    for (int i = 0; i < 2; i++) {
      int by = 152 + i * 26;
      bool sel = (famMode == i);
      uint16_t bg = sel ? SEL_BG : UNSEL_BG;
      cv->fillRoundRect(bx, by, 136, 23, 7, bg);
      if (sel) cv->drawRoundRect(bx, by, 136, 23, 7, TFT_WHITE);
      cv->setTextColor(sel ? SEL_FG : UNSEL_FG, bg);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(mn[i], bx + 68, by + 12, &FreeSansTR7pt);
    }
    cv->setTextDatum(TL_DATUM);
  }
  cv->setTextColor(UI_SUBTEXT, UI_BG);
  drawFreeTitle(L("Web şifresi", "Web password"), bx, 206, &FreeSansTR6pt);
  drawPwSpaced(webPass, bx + 68, 220, 16, &lgfx::fonts::FreeMonoBold12pt7b, TFT_WHITE, UI_BG);
  cv->setTextDatum(TL_DATUM);
}

// Eskiden BOOT/Kapat sifre ekraniydi. Sifre yanlislikla dokunmaya karsi
// korumaydi; simdi ayni korumayi PIN yerine "sec + Evet/Hayir onayla" akisi
// sagliyor (2 bilincli dokunus gerekiyor, ezberlenecek sifre yok).
void drawResetScreen() {
  cv->fillScreen(UI_BG);
  drawRefreshIcon(20, 14, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle("RESET", 168, 4, &FreeSansTR9pt);
  cv->drawFastHLine(10, 26, 300, 0x2104);

  if (resetAction == 0) {
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("Sıfırla veya cihazı kapat", "Reset or power off"), 160, 32, &FreeSansTR7pt);

    cv->fillRoundRect(10, 54, 145, 90, 10, 0x4B72);
    drawGlobeIcon(82, 80, TFT_WHITE);
    cv->setTextColor(TFT_WHITE, 0x4B72);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("Cihaz Sıfırla", "Reset Device"), 82, 108, &FreeSansTR7pt);
    cv->setTextColor(0xCE59, 0x4B72);
    drawFreeTitle(L("WiFi + kaynak girişleri", "WiFi + source logins"), 82, 130, &FreeSansTR6pt);

    cv->fillRoundRect(165, 54, 145, 90, 10, 0x8000);
    drawPowerIcon(237, 80, TFT_WHITE);
    cv->setTextColor(TFT_WHITE, 0x8000);
    drawFreeTitle(L("Cihazı Kapat", "Power off"), 237, 108, &FreeSansTR7pt);
    cv->setTextColor(0xFDA0, 0x8000);
    drawFreeTitle(L("Güvenli kapatma", "Safe shutdown"), 237, 130, &FreeSansTR6pt);

    cv->fillRoundRect(90, 194, 140, 40, 8, 0x333333);
    cv->drawRoundRect(90, 194, 140, 40, 8, 0x555555);
    cv->setTextColor(TFT_WHITE, 0x333333);
    drawBackIcon(112, 214, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Geri", "Back"), 128, 214, &FreeSansTR7pt);
  } else {
    bool poweroff = (resetAction == 2);
    uint16_t accent = poweroff ? sevColor(0) : TFT_CYAN;
    cv->drawRoundRect(20, 46, 280, 80, 12, accent);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(MC_DATUM);
    if (poweroff) {
      drawFreeTitle(L("Cihaz kapanacak.", "Device will power off."), 160, 72, &FreeSansTR7pt);
      cv->setTextColor(TFT_LIGHTGREY, UI_BG);
      drawFreeTitle(L("Emin misiniz?", "Are you sure?"), 160, 98, &FreeSansTR7pt);
    } else {
      drawFreeTitle(L("WiFi ve kaynak girişleri", "WiFi and source logins"), 160, 60, &FreeSansTR7pt);
      drawFreeTitle(L("(kullanıcı adı/şifre) silinecek.", "(user/password) will be erased."), 160, 80, &FreeSansTR7pt);
      cv->setTextColor(TFT_LIGHTGREY, UI_BG);
      drawFreeTitle(L("Alarm/ekran ayarları kalır. Emin misiniz?", "Alarm/display settings stay. Sure?"), 160, 104, &FreeSansTR6pt);
    }

    cv->fillRoundRect(40, 150, 110, 50, 12, 0x0B5E);
    cv->setTextColor(TFT_WHITE, 0x0B5E);
    drawCheckIcon(65, 175, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Evet", "Yes"), 82, 175, &FreeSansTR7pt);

    cv->fillRoundRect(170, 150, 110, 50, 12, 0x333333);
    cv->setTextColor(TFT_WHITE, 0x333333);
    drawXIcon(195, 175, TFT_WHITE);
    drawFreeTitle(L("Hayır", "No"), 212, 175, &FreeSansTR7pt);
  }
}

void drawInfoScreen() {
  cv->fillScreen(UI_BG);
  drawInfoIconM(20, 14, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  // Turkce karakterli font - 2. deneme: Adafruit'in resmi fontconvert araciyla
  // uretildi (bkz. yukaridaki FreeSansTR9pt tanimi). Ilk deneme (kendi
  // script'imizin ciktisi) cihazda bozuk cikmisti; bu surum farkli/guvenilir
  // bir uretim yolundan geliyor. Ekranda "BILGI" gibi degil gercek "BİLGİ"
  // gorunmesi beklenir.
  drawFreeTitle(L("BİLGİ", "INFO"), 168, 6, &FreeSansTR9pt);
  cv->drawFastHLine(10, 26, 300, 0x2104);

  // Baglanti karti
  cv->drawRoundRect(10, 32, 300, 66, 12, 0x2945);
  drawGlobeIcon(28, 50, WiFi.status() == WL_CONNECTED ? sevColor(2) : sevColor(0));
  cv->setTextDatum(TL_DATUM);
  if (WiFi.status() == WL_CONNECTED) {
    cv->setTextColor(sevColor(2), UI_BG);
    dsN(cv, WiFi.localIP().toString(), 44, 42, 2);
  } else {
    cv->setTextColor(sevColor(0), UI_BG);
    drawFreeTitle(L("Bağlı değil", "Not connected"), 44, 42, &FreeSansTR7pt);
  }
  String wifiName = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "-";
  if (wifiName.length() > 20) wifiName = wifiName.substring(0, 20) + "..";
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  dsN(cv, String(L("WiFi: ", "WiFi: ")) + wifiName, 44, 62, 1);
  cv->setTextColor(sevColor(1), UI_BG);
  dsN(cv, String(L("Kaynak: ", "Source: ")) + sourceLabel(), 44, 78, 1);
  // Pil (INA219 takiliysa)
  {
    String bt;
    if (!inaPresent) bt = L("Pil: olcer yok", "Battery: no gauge");
    else if (batteryPercent < 0) bt = L("Pil: takili degil", "Battery: none");
    else {
      bt = batteryPctText(L("Pil: %%%d", "Battery: %d%%"));
      bt += " " + String(batteryMv / 1000.0f, 2) + "V";
      if (batteryCharging) bt += L(" sarj", " chg");
    }
    cv->setTextColor(inaPresent && batteryPercent >= 0 ? sevColor(2) : TFT_DARKGREY, UI_BG);
    cv->setTextDatum(TR_DATUM);
    dsN(cv, bt, 302, 62, 1);
    cv->setTextDatum(TL_DATUM);
  }

  // Cihaz / guvenlik karti (BOOT sifresi kaldirildi iptal edildi)
  cv->drawRoundRect(10, 104, 300, 84, 12, 0x2945);
  drawLockIcon(28, 122, sevColor(1));
  cv->setTextColor(TFT_WHITE, UI_BG);
  dsN(cv, CONTACT_EMAIL, 44, 114, 1);
  dsN(cv, YOUTUBE_HANDLE, 44, 128, 1);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  dsN(cv, String("Web: ") + webPass, 44, 146, 1);
  cv->setTextColor(TFT_CYAN, UI_BG);
  dsN(cv, String("ID: ") + thisChipId + "  " + deviceName(), 44, 160, 1);
  cv->setTextColor(sevColor(2), UI_BG);
  drawFreeTitle(SIGNATURE_TEXT(), 44, 170, &FreeSansTR6pt);

  cv->fillRoundRect(20, 198, 130, 28, 8, 0x5010);
  cv->setTextColor(TFT_WHITE, 0x5010);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Kaynaklar", "Sources"), 85, 212, &FreeSansTR7pt);

  cv->fillRoundRect(170, 198, 130, 28, 8, 0x333333);
  cv->setTextColor(TFT_WHITE, 0x333333);
  drawBackIcon(192, 212, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 208, 212, &FreeSansTR7pt);
}

void drawSourcesScreen() {
  // Not: bu ekran, telife gore gozden gecirildi. Kod olarak kullanilmayan/
  // atif zorunlulugu olmayan satirlar (ilham projesi, genel bir bilgi
  // sitesi) kaldirildi; gercekten telif/lisans tasiyan iki grup (veri
  // kaynaklari + kutuphaneler/font) daha ferah bir duzenle birakildi.
  cv->fillScreen(UI_BG);
  drawSparkleIcon(18, 12, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("KAYNAKLAR", "SOURCES"), 170, 4, &FreeSansTR7pt);
  cv->drawFastHLine(8, 22, 304, 0x2104);

  // Kart 1: veri kaynaklari - hangi uclarin kullanildigi durustce belirtiliyor
  cv->drawRoundRect(8, 28, 304, 64, 10, 0x2945);
  drawGlobeIcon(24, 42, sevColor(1));
  cv->setTextDatum(TL_DATUM);
  cv->setTextColor(sevColor(1), UI_BG);
  drawFreeTitle(L("Veri kaynakları", "Data sources"), 40, 34, &FreeSansTR6pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  dsN(cv, "Nightscout - Dexcom - LibreLinkUp - Medtrum", 16, 50, 1);
  cv->setTextColor(TFT_DARKGREY, UI_BG);
  drawFreeTitle(L("Bazıları üçüncü taraf API uçları kullanır", "Some use third-party API endpoints"), 16, 64, &FreeSansTR6pt);

  // Kart 2: acik kaynak kutuphaneler + yazi tipi (lisans atiflari)
  cv->drawRoundRect(8, 98, 304, 78, 10, 0x2945);
  drawBookIcon(24, 112, sevColor(1));
  cv->setTextColor(sevColor(1), UI_BG);
  drawFreeTitle(L("Kütüphaneler", "Libraries"), 40, 104, &FreeSansTR6pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  dsN(cv, "LovyanGFX - WiFiManager - ArduinoJson", 16, 120, 1);
  dsN(cv, "QRCode", 16, 134, 1);
  cv->setTextColor(TFT_DARKGREY, UI_BG);
  drawFreeTitle(L("Yazı tipi: Noto Sans (dönüştürülmüş)", "Font: Noto Sans (converted)"), 16, 150, &FreeSansTR6pt);

  cv->setTextColor(TFT_DARKGREY, UI_BG);
  cv->setTextDatum(TC_DATUM);
  dsN(cv, L("Tibbi cihaz degildir. Markalar sahiplerine aittir.",
                   "Not a medical device. Brands belong to owners."), 160, 184, 1);

  cv->fillRoundRect(90, 200, 140, 30, 8, 0x333333);
  cv->setTextColor(TFT_WHITE, 0x333333);
  drawBackIcon(112, 215, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 128, 215, &FreeSansTR7pt);
}

void drawLangUnitScreen() {
  cv->fillScreen(UI_BG);
  drawGlobeIcon(20, 14, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("DİL / BİRİM", "LANG / UNIT"), 168, 6, &FreeSansTR9pt);
  cv->drawFastHLine(10, 28, 300, 0x2104);

  // Dil + Birim: Parlaklik/Arayuz ekranlarindaki ayni 2x2 ortali izgara
  // (120x48, ayni renkler) — Dil ve Birim tek bir 4'lu izgarada birlesti.
  // 3.6.0: ust satir tek genis dugme - dokununca dil secim izgarasi acilir
  {
    cv->fillRoundRect(30, 34, 260, 48, 8, SEL_BG);
    cv->drawRoundRect(30, 34, 260, 48, 8, TFT_WHITE);
    cv->setTextColor(SEL_FG, SEL_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(LANG_NAMES[langIdx], 160, 58, &FreeSansTR9pt);
  }
  const char* gridLabels[] = {"", "", "mg/dL", "mmol/L"};
  for (int i = 2; i < 4; i++) {
    int col = i % 2, row = i / 2;
    int x = 30 + col * 140;
    int y = 34 + row * 54;
    bool sel = (unitIdx == col);
    uint16_t bg = sel ? SEL_BG : UNSEL_BG;
    uint16_t fg = sel ? SEL_FG : UNSEL_FG;
    cv->fillRoundRect(x, y, 120, 48, 8, bg);
    cv->drawRoundRect(x, y, 120, 48, 8, sel ? TFT_WHITE : 0x4A69);
    cv->setTextColor(fg, bg);
    cv->setTextDatum(MC_DATUM);
    dsN(cv, gridLabels[i], x + 60, y + 24, 2);
  }

  // Renk Dostu Modu (eskiden "Renk körlüğü modu" — daha yumuşak bir ifade)
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  cv->setTextDatum(TL_DATUM);
  drawFreeTitle(L("Renk Dostu Modu", "Color Friendly Mode"), 24, 142, &FreeSansTR7pt);
  uint16_t cbOffBg = (!colorBlindMode) ? SEL_BG : UNSEL_BG;
  cv->fillRoundRect(20, 158, 130, 26, 6, cbOffBg);
  cv->setTextColor(!colorBlindMode ? SEL_FG : UNSEL_FG, cbOffBg);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Kapalı", "Off"), 85, 171, &FreeSansTR7pt);
  if (!colorBlindMode) cv->drawRoundRect(20, 158, 130, 26, 6, TFT_WHITE);
  uint16_t cbOnBg = colorBlindMode ? SEL_BG : UNSEL_BG;
  cv->fillRoundRect(166, 158, 130, 26, 6, cbOnBg);
  cv->setTextColor(colorBlindMode ? SEL_FG : UNSEL_FG, cbOnBg);
  drawFreeTitle(L("Açık", "On"), 231, 171, &FreeSansTR7pt);
  if (colorBlindMode) cv->drawRoundRect(166, 158, 130, 26, 6, TFT_WHITE);

  // Geri: diger ayar ekranlarindaki gibi buyuk, rahat dokunulabilir bir cubuk
  cv->fillRoundRect(90, 194, 140, 40, 8, 0x333333);
  cv->drawRoundRect(90, 194, 140, 40, 8, 0x555555);
  cv->setTextColor(TFT_WHITE, 0x333333);
  drawBackIcon(112, 214, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 128, 214, &FreeSansTR7pt);
}

void drawUiModeScreen() {
  cv->fillScreen(UI_BG);
  drawGridIcon(20, 14, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("ARAYÜZ", "INTERFACE"), 172, 4, &FreeSansTR9pt);
  cv->drawFastHLine(10, 50, 300, 0x2104);
  // Dikey arayuz kaldirildi: Klasik/Buyuk/Takvim/Foto Cerceve, Alarm menusuyle ayni ortali izgara.
  // Isimler artik burada tekrar yazilmiyor - tek kaynak (uiModeName()) kullaniliyor,
  // ileride isim degisirse iki yeri birden guncelleme riski olmasin diye.
  const int dispModes[] = {0, 1, 2, 4};
  for (int i = 0; i < 4; i++) {
    int x = 30 + (i % 2) * 140;
    int y = 60 + (i / 2) * 60;
    int m = dispModes[i];
    bool on = (uiPageMode == m) || (uiPreviewMode == m && uiPreviewActive);
    uint16_t bg = on ? SEL_BG : UNSEL_BG;
    uint16_t fg = on ? SEL_FG : UNSEL_FG;
    cv->fillRoundRect(x, y, 120, 48, 8, bg);
    cv->drawRoundRect(x, y, 120, 48, 8, (uiPageMode == m) ? TFT_WHITE : 0x4A69);
    cv->setTextColor(fg, bg);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(uiModeName(m), x + 60, y + 24, &FreeSansTR7pt);
  }
  cv->fillRoundRect(8, 185, 148, 36, 8, 0x0B5E);
  cv->setTextColor(TFT_WHITE, 0x0B5E);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Uygula", "Apply"), 82, 203, &FreeSansTR7pt);
  cv->fillRoundRect(164, 185, 148, 36, 8, 0x4B72);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  drawBackIcon(188, 203, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 204, 203, &FreeSansTR7pt);
}

void drawRotationScreen() {
  cv->fillScreen(UI_BG);
  drawRotateIcon(20, 18, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("ROTASYON", "ROTATION"), 172, 10, &FreeSansTR9pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  drawFreeTitle(L("USB yönüne göre yatay", "Landscape by USB side"), 160, 40, &FreeSansTR7pt);
  cv->drawFastHLine(10, 60, 300, 0x2104);

  // Not: kutu pozisyonlari (sol/sag) ve landRot degerleri (1/3) ayni kaldi,
  // sadece uzerlerindeki yazi kullaniciya gore ters oldugu icin yer degistirdi.
  uint16_t a = (landRot == 1) ? SEL_BG : UNSEL_BG;
  uint16_t b = (landRot == 3) ? SEL_BG : UNSEL_BG;
  cv->fillRoundRect(20, 70, 130, 70, 10, a);
  cv->drawRoundRect(20, 70, 130, 70, 10, (landRot == 1) ? TFT_WHITE : 0x4A69);
  cv->setTextColor(landRot == 1 ? SEL_FG : UNSEL_FG, a);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Sağ yan", "Right side"), 85, 105, &FreeSansTR7pt);
  cv->fillRoundRect(170, 70, 130, 70, 10, b);
  cv->drawRoundRect(170, 70, 130, 70, 10, (landRot == 3) ? TFT_WHITE : 0x4A69);
  cv->setTextColor(landRot == 3 ? SEL_FG : UNSEL_FG, b);
  drawFreeTitle(L("Sol yan", "Left side"), 235, 105, &FreeSansTR7pt);

  // Geri: alttaki bosluğu daha iyi kullanan, buyuk/rahat bir buton
  cv->fillRoundRect(60, 172, 200, 48, 10, 0x4B72);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  drawBackIcon(140, 196, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 158, 196, &FreeSansTR7pt);
}

void drawGeceScreen() {
  cv->fillScreen(UI_BG);
  drawMoonIcon(20, 14, TFT_CYAN, UI_BG);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("GECE MODU", "NIGHT MODE"), 168, 6, &FreeSansTR9pt);
  cv->drawFastHLine(10, 26, 300, 0x2104);

  uint16_t onBg = nightModeOn ? SEL_BG : UNSEL_BG;
  uint16_t offBg = !nightModeOn ? SEL_BG : UNSEL_BG;
  cv->fillRoundRect(12, 32, 144, 34, 10, onBg);
  cv->setTextColor(nightModeOn ? SEL_FG : UNSEL_FG, onBg);
  cv->setTextDatum(MC_DATUM);
  drawCheckIcon(30, 49, nightModeOn ? SEL_FG : UNSEL_FG);
  drawFreeTitle(L("Açık", "On"), 84, 49, &FreeSansTR7pt);
  if (nightModeOn) cv->drawRoundRect(12, 32, 144, 34, 10, TFT_WHITE);
  cv->fillRoundRect(164, 32, 144, 34, 10, offBg);
  cv->setTextColor(!nightModeOn ? SEL_FG : UNSEL_FG, offBg);
  drawXIcon(182, 49, !nightModeOn ? SEL_FG : UNSEL_FG);
  drawFreeTitle(L("Kapalı", "Off"), 236, 49, &FreeSansTR7pt);
  if (!nightModeOn) cv->drawRoundRect(164, 32, 144, 34, 10, TFT_WHITE);

  char buf[32];
  // Baslangic / Bitis — buyutulmus satirlar, +/- yarim saat adimlarla
  cv->fillRoundRect(12, 72, 296, 40, 12, 0x2945);
  drawClockIcon(28, 92, TFT_WHITE);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(ML_DATUM);
  sprintf(buf, "%s  %02d:%02d", L("Baslangic", "Start"), nightStartHour, nightStartMin);
  dsN(cv, buf, 44, 92, 2);
  cv->fillRoundRect(194, 76, 46, 32, 10, 0x333333);
  cv->setTextColor(TFT_WHITE, 0x333333);
  cv->setTextDatum(MC_DATUM);
  dsN(cv, "-", 217, 92, 4);
  cv->fillRoundRect(248, 76, 46, 32, 10, 0x333333);
  dsN(cv, "+", 271, 92, 4);

  cv->fillRoundRect(12, 118, 296, 40, 12, 0x2945);
  drawClockIcon(28, 138, TFT_WHITE);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(ML_DATUM);
  sprintf(buf, "%s  %02d:%02d", L("Bitis", "End"), nightEndHour, nightEndMin);
  dsN(cv, buf, 44, 138, 2);
  cv->fillRoundRect(194, 122, 46, 32, 10, 0x333333);
  cv->setTextColor(TFT_WHITE, 0x333333);
  cv->setTextDatum(MC_DATUM);
  dsN(cv, "-", 217, 138, 4);
  cv->fillRoundRect(248, 122, 46, 32, 10, 0x333333);
  dsN(cv, "+", 271, 138, 4);

  // Parlaklik / Ses / Kaydet — yan yana 3 buton
  cv->fillRoundRect(12, 166, 94, 50, 10, 0x4B72);
  cv->drawRoundRect(12, 166, 94, 50, 10, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  cv->setTextDatum(MC_DATUM);
  drawBrightnessLevelIcon(59, 184, TFT_WHITE, nightBrightLevel);
  drawFreeTitle(L("Parlaklık", "Bright"), 59, 204, &FreeSansTR6pt);

  cv->fillRoundRect(112, 166, 94, 50, 10, 0x2945);
  cv->drawRoundRect(112, 166, 94, 50, 10, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x2945);
  if (nightSoundLevel == 0) drawMuteIcon(159, 184, TFT_WHITE);
  else drawSoundLevelIcon(159, 184, TFT_WHITE, nightSoundLevel - 1);
  drawFreeTitle(L("Ses", "Sound"), 159, 204, &FreeSansTR6pt);

  cv->fillRoundRect(212, 166, 94, 50, 10, 0x0B5E);
  cv->drawRoundRect(212, 166, 94, 50, 10, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x0B5E);
  drawCheckIcon(259, 184, TFT_WHITE);
  drawFreeTitle(L("Kaydet", "Save"), 259, 204, &FreeSansTR6pt);
}

void drawOkulScreen() {
  cv->fillScreen(UI_BG);
  drawBookIcon(20, 14, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("OKUL MODU", "SCHOOL MODE"), 168, 6, &FreeSansTR9pt);
  cv->drawFastHLine(10, 26, 300, 0x2104);

  uint16_t onBg = schoolModeOn ? SEL_BG : UNSEL_BG;
  uint16_t offBg = !schoolModeOn ? SEL_BG : UNSEL_BG;
  cv->fillRoundRect(12, 32, 144, 34, 10, onBg);
  cv->setTextColor(schoolModeOn ? SEL_FG : UNSEL_FG, onBg);
  cv->setTextDatum(MC_DATUM);
  drawCheckIcon(30, 49, schoolModeOn ? SEL_FG : UNSEL_FG);
  drawFreeTitle(L("Açık", "On"), 84, 49, &FreeSansTR7pt);
  if (schoolModeOn) cv->drawRoundRect(12, 32, 144, 34, 10, TFT_WHITE);
  cv->fillRoundRect(164, 32, 144, 34, 10, offBg);
  cv->setTextColor(!schoolModeOn ? SEL_FG : UNSEL_FG, offBg);
  drawXIcon(182, 49, !schoolModeOn ? SEL_FG : UNSEL_FG);
  drawFreeTitle(L("Kapalı", "Off"), 236, 49, &FreeSansTR7pt);
  if (!schoolModeOn) cv->drawRoundRect(164, 32, 144, 34, 10, TFT_WHITE);

  char buf[32];
  cv->fillRoundRect(12, 72, 296, 40, 12, 0x2945);
  drawClockIcon(28, 92, TFT_WHITE);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(ML_DATUM);
  sprintf(buf, "%s  %02d:%02d", L("Baslangic", "Start"), schoolStartHour, schoolStartMin);
  dsN(cv, buf, 44, 92, 2);
  cv->fillRoundRect(194, 76, 46, 32, 10, 0x333333);
  cv->setTextColor(TFT_WHITE, 0x333333);
  cv->setTextDatum(MC_DATUM);
  dsN(cv, "-", 217, 92, 4);
  cv->fillRoundRect(248, 76, 46, 32, 10, 0x333333);
  dsN(cv, "+", 271, 92, 4);

  cv->fillRoundRect(12, 118, 296, 40, 12, 0x2945);
  drawClockIcon(28, 138, TFT_WHITE);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(ML_DATUM);
  sprintf(buf, "%s  %02d:%02d", L("Bitis", "End"), schoolEndHour, schoolEndMin);
  dsN(cv, buf, 44, 138, 2);
  cv->fillRoundRect(194, 122, 46, 32, 10, 0x333333);
  cv->setTextColor(TFT_WHITE, 0x333333);
  cv->setTextDatum(MC_DATUM);
  dsN(cv, "-", 217, 138, 4);
  cv->fillRoundRect(248, 122, 46, 32, 10, 0x333333);
  dsN(cv, "+", 271, 138, 4);

  // Parlaklik / Ses / Kaydet — yan yana 3 buton
  cv->fillRoundRect(12, 166, 94, 50, 10, 0x4B72);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  cv->setTextDatum(MC_DATUM);
  drawBrightnessLevelIcon(59, 184, TFT_WHITE, schoolBrightLevel);
  drawFreeTitle(L("Parlak", "Bright"), 59, 204, &FreeSansTR6pt);

  cv->fillRoundRect(112, 166, 94, 50, 10, 0x2945);
  cv->setTextColor(TFT_WHITE, 0x2945);
  if (schoolSoundLevel == 0) drawMuteIcon(159, 184, TFT_WHITE);
  else drawSoundLevelIcon(159, 184, TFT_WHITE, schoolSoundLevel - 1);
  drawFreeTitle(L("Ses", "Sound"), 159, 204, &FreeSansTR6pt);

  cv->fillRoundRect(212, 166, 94, 50, 10, 0x0B5E);
  cv->setTextColor(TFT_WHITE, 0x0B5E);
  drawCheckIcon(259, 184, TFT_WHITE);
  drawFreeTitle(L("Kaydet", "Save"), 259, 204, &FreeSansTR6pt);
}



int teacherSgv() {
  if (teacherPreview == 1 && teacherStep > 0) return (alarmLow - 15 < 40) ? 40 : alarmLow - 15;
  if (teacherPreview == 2 && teacherStep > 0) return alarmVeryHigh + 30;
  return sgv;
}
int teacherSensorCat() { int s = teacherSgv(); return (s > 0 && s < alarmLow) ? 1 : (s >= alarmVeryHigh ? 3 : 2); }
static void tfBtn(int x, int w, uint16_t bg, uint16_t fg, const char* t) {
  cv->fillRoundRect(x, 120, w, 50, 8, bg);
  cv->setTextColor(fg, bg);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(t, x + w / 2, 145, &FreeSansTR7pt);
}
static void tfBtn2(int x, int w, uint16_t bg, uint16_t fg, const char* t, const String& sub) {   // 3.4.7
  cv->fillRoundRect(x, 120, w, 50, 8, bg);
  cv->setTextColor(fg, bg);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(t, x + w / 2, 135, &FreeSansTR7pt);
  drawFreeTitle(sub.c_str(), x + w / 2, 157, &FreeSansTR6pt);
}
static void tfLine(const char* t, int y) { cv->setTextDatum(TL_DATUM); drawFreeTitle(t, 12, y, &FreeSansTR7pt); }
void drawTeacherFlow() {
  // 3.4.5: 1=bilinc? 2=parmak olcumu yakin mi? 4=parmak sonucu ne? 3=aksiyon karti; teacherGlucagonPage 1..4
  cv->fillScreen(UI_BG);

  if (teacherGlucagonPage > 0) {
    cv->setTextColor(sevColor(3), UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("GLUKAGON", "GLUCAGON"), 160, 6, &FreeSansTR7pt);
    char pg[12]; sprintf(pg, "%d/4", teacherGlucagonPage);
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    dsN(cv, pg, 160, 28, 1);
    cv->setTextDatum(TL_DATUM);
    if (teacherGlucagonPage == 1) {
      cv->setTextColor(sevColor(0), UI_BG);
      tfLine(L("Önce 112'yi aratın!", "Call emergency services first!"), 48);
      cv->setTextColor(TFT_WHITE, UI_BG);
      tfLine(L("- Ciddi düşük şeker ilacıdır", "- Medicine for severe low"), 78);
      tfLine(L("- Bilinç kapalıysa / yutamıyorsa", "- If unconscious / cannot swallow"), 100);
      tfLine(L("- Zaman kaybetmeden uygulayın", "- Give it without delay"), 122);
      tfLine(L("- Aile / okul planına göre", "- Follow the family/school plan"), 144);
    } else if (teacherGlucagonPage == 2) {
      cv->setTextColor(sevColor(1), UI_BG);
      tfLine(L("Nasıl hazırlanır?", "How to prepare?"), 48);
      cv->setTextColor(TFT_WHITE, UI_BG);
      tfLine(L("- Kit okulda hazır tutulsun", "- Keep the kit ready at school"), 78);
      tfLine(L("- Kutudaki resimli talimata bakın", "- Follow the picture guide"), 100);
      tfLine(L("- 4. sayfadaki videoyu izleyin", "- Watch the video on page 4"), 122);
      tfLine(L("- Doz yaş / kiloya göre değişir", "- Dose depends on age/weight"), 144);
    } else if (teacherGlucagonPage == 3) {
      cv->setTextColor(sevColor(1), UI_BG);
      tfLine(L("Uygulama sonrası", "After giving it"), 48);
      cv->setTextColor(TFT_WHITE, UI_BG);
      tfLine(L("- Yan yatırın, solunum yolu açık", "- Side position, airway open"), 78);
      tfLine(L("- Uyanıp yutabilince meyve suyu", "- When awake and can swallow: juice"), 100);
      tfLine(L("- 10 dk'da uyanmazsa 112", "- Not awake in 10 min: call emergency"), 122);
      tfLine(L("- Saati not edin, aileyi arayın", "- Note the time, call the family"), 144);
    } else {
      // 4. sayfa: cihazdaki glukagon videosu
      cv->setTextColor(sevColor(1), UI_BG);
      tfLine(L("Video: nasıl uygulanır?", "Video: how to give it"), 48);
      if (videoExists()) {
        cv->fillRoundRect(40, 74, 240, 100, 16, 0xA800);
        cv->fillTriangle(146, 92, 146, 132, 178, 112, TFT_WHITE);
        cv->setTextColor(TFT_WHITE, 0xA800);
        cv->setTextDatum(MC_DATUM);
        drawFreeTitle(L("Videoyu oynat", "Play the video"), 160, 155, &FreeSansTR9pt);
      } else {
        cv->setTextColor(TFT_LIGHTGREY, UI_BG);
        cv->setTextDatum(TC_DATUM);
        drawFreeTitle(L("Video bu cihazda yüklü değil.", "The video is not installed."), 160, 100, &FreeSansTR7pt);
        drawFreeTitle(L("Kutudaki talimatı kullanın.", "Use the guide in the box."), 160, 124, &FreeSansTR7pt);
      }
      cv->setTextDatum(TL_DATUM);
    }
    cv->fillRoundRect(30, 200, 120, 36, 6, 0x333333);
    cv->setTextColor(TFT_WHITE, 0x333333);
    drawBackIcon(52, 218, TFT_WHITE);
    cv->setTextDatum(ML_DATUM);
    drawFreeTitle(L("Geri", "Back"), 68, 218, &FreeSansTR7pt);
    cv->fillRoundRect(170, 200, 120, 36, 6, 0x4B72);
    cv->setTextColor(TFT_WHITE, 0x4B72);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(teacherGlucagonPage < 4 ? L("İleri", "Next") : L("Kapat", "Close"), 230, 218, &FreeSansTR7pt);
    cv->setTextDatum(TL_DATUM);
    return;
  }

  cv->setTextColor(TFT_MAGENTA, UI_BG);
  cv->setTextDatum(TC_DATUM);
  if (teacherPreview) { cv->setTextColor(sevColor(1), UI_BG); drawFreeTitle(L("ÖNİZLEME", "PREVIEW"), 160, 6, &FreeSansTR7pt); }
  else if (personCount > 1) drawFreeTitle((String(L("REHBER", "GUIDE")) + ": " + personLabel(dispCtx)).c_str(), 160, 6, &FreeSansTR7pt);   // 4.0
  else drawFreeTitle(L("REHBER", "GUIDE"), 160, 6, &FreeSansTR7pt);
  char gbuf[16];
  { String _gs = formatGlucose(teacherSgv()); strncpy(gbuf, _gs.c_str(), 15); gbuf[15] = 0; }
  cv->setTextColor(getGlucoseColor(teacherSgv()), UI_BG);
  dsN(cv, gbuf, 160, 32, 4);

  if (teacherStep == 1) {
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Bilinci kapalı mı, nöbet geçiriyor", "Unconscious, having a seizure"), 160, 70, &FreeSansTR7pt);
    drawFreeTitle(L("ya da yutamıyor mu?", "or unable to swallow?"), 160, 92, &FreeSansTR7pt);
    tfBtn(20, 130, sevColor(0), TFT_WHITE, L("Evet", "Yes"));
    tfBtn(170, 130, sevColor(2), TFT_BLACK, L("Hayır", "No"));
  } else if (teacherStep == 2) {
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Parmaktan kan şekeri ölçün.", "Do a fingerstick now."), 160, 70, &FreeSansTR7pt);
    drawFreeTitle(L("Sonuç sensöre yakın mı?", "Close to the sensor value?"), 160, 92, &FreeSansTR7pt);
    tfBtn(12, 96, 0x4B72, TFT_WHITE, L("Evet", "Yes"));
    tfBtn(112, 96, 0x4208, TFT_WHITE, L("Hayır", "No"));
    tfBtn(212, 96, 0x2945, TFT_WHITE, L("Ölçemiyorum", "Can't test"));
  } else if (teacherStep == 4) {
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Parmak ölçümü sonucu kaç?", "What is the fingerstick result?"), 160, 72, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle((String("(") + unitLabel() + ")").c_str(), 160, 94, &FreeSansTR6pt);
    String lo = formatGlucose(alarmLow);
    String hi = formatGlucose(alarmHigh);
    tfBtn2(12, 96, sevColor(0), TFT_WHITE, L("Düşük", "Low"), fmtS(L("%s altı", "below %s"), lo));
    tfBtn2(112, 96, sevColor(2), TFT_BLACK, L("Normal", "Normal"), lo + " - " + hi);
    tfBtn2(212, 96, sevColor(3), TFT_BLACK, L("Yüksek", "High"), fmtS(L("%s üstü", "above %s"), hi));
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Emin değilseniz aileyi arayın.", "If unsure, call the family."), 160, 181, &FreeSansTR6pt);
  } else {
    // Aksiyon karti
    cv->setTextDatum(TL_DATUM);
    if (!teacherConscious) {
      cv->setTextColor(sevColor(0), UI_BG);
      tfLine(L("BİLİNÇ KAPALI / YUTAMIYOR:", "UNCONSCIOUS / CANNOT SWALLOW:"), 60);
      cv->setTextColor(TFT_WHITE, UI_BG);
      tfLine(L("- 112'yi arayın, aileye haber verin", "- Call emergency and the family"), 86);
      tfLine(L("- Yan yatırın, solunum yolu açık", "- Side position, airway open"), 106);
      tfLine(L("- Yiyecek veya içecek vermeyin", "- Give no food or drink"), 126);
      if (teacherSensorCat() == 1) {            // glukagon sadece dusuk sekerde
        cv->fillRoundRect(40, 150, 240, 40, 8, sevColor(3));
        cv->setTextColor(TFT_BLACK, sevColor(3));
        cv->setTextDatum(MC_DATUM);
        drawFreeTitle(L("Glukagon nasıl uygulanır?", "How to give glucagon?"), 160, 170, &FreeSansTR7pt);
      } else {
        tfLine(L("- Yanından ayrılmayın", "- Stay with the student"), 146);
      }
    } else if (teacherCat == 1) {
      cv->setTextColor(sevColor(1), UI_BG);
      tfLine(L("DÜŞÜK ŞEKER:", "LOW GLUCOSE:"), 62);
      cv->setTextColor(TFT_WHITE, UI_BG);
      tfLine(L("- Meyve suyu veya küp şeker verin", "- Give juice or sugar cubes"), 84);
      tfLine(L("- 15 dk bekleyin, tekrar ölçün", "- Wait 15 min, recheck"), 104);
      tfLine(L("- Yükselmezse tekrarlayın", "- Repeat if still low"), 124);
      tfLine(L("- Yutamaz hale gelirse 112", "- If cannot swallow: call emergency"), 144);
      tfLine(L("- Yanından ayrılmayın", "- Stay with the student"), 164);
    } else if (teacherCat == 3) {
      cv->setTextColor(sevColor(3), UI_BG);
      tfLine(L("ÇOK YÜKSEK:", "VERY HIGH:"), 62);
      cv->setTextColor(TFT_WHITE, UI_BG);
      tfLine(L("- Su içirin (zorlamayın)", "- Water (do not force)"), 84);
      tfLine(L("- Egzersiz yaptırmayın", "- No exercise"), 104);
      tfLine(L("- Doz kararı vermeyin, aileye /", "- Do not decide a dose; follow"), 124);
      tfLine(L("  okul planına başvurun", "  the family/school plan"), 144);
      tfLine(L("- Kusma, karın ağrısı, hızlı nefes: 112", "- Vomiting, belly pain, fast breath: emergency"), 164);
    } else {
      cv->setTextColor(sevColor(2), UI_BG);
      tfLine(L("Durumu izleyin", "Monitor the student"), 90);
      cv->setTextColor(TFT_WHITE, UI_BG);
      tfLine(L("- Halsizlik varsa yanında kalın", "- Stay close if weak"), 120);
      tfLine(L("- Şüphede aileyi arayın", "- Call the family if unsure"), 142);
    }
  }
  cv->fillRoundRect(30, 200, 120, 36, 6, 0x333333);
  cv->setTextColor(TFT_WHITE, 0x333333);
  drawBackIcon(52, 218, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 68, 218, &FreeSansTR7pt);
  cv->fillRoundRect(170, 200, 120, 36, 6, 0x4B72);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Anasayfa", "Home"), 230, 218, &FreeSansTR7pt);
  cv->setTextDatum(TL_DATUM);
}

void drawTeacherScreen() {
  cv->fillScreen(UI_BG);
  cv->setTextColor(TFT_MAGENTA, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("REHBER MODU", "GUIDE MODE"), 160, 4, &FreeSansTR7pt);

  // Ust: acik/kapali
  uint16_t onBg = teacherModeOn ? SEL_BG : UNSEL_BG;
  uint16_t offBg = !teacherModeOn ? SEL_BG : UNSEL_BG;
  uint16_t onFg = teacherModeOn ? SEL_FG : UNSEL_FG;
  uint16_t offFg = !teacherModeOn ? SEL_FG : UNSEL_FG;
  cv->fillRoundRect(12, 26, 144, 32, 8, onBg);
  cv->setTextColor(onFg, onBg);
  cv->setTextDatum(MC_DATUM);
  drawCheckIcon(30, 42, onFg);
  // Metin kutunun tam ortasina (x=84/236) hizalandi - Gece/Okul Modu
  // ekranlarindaki ayni Acik/Kapali anahtariyla tutarli (onceden 96/248
  // kullaniliyordu, kutu merkeziyle uyumsuzdu).
  drawFreeTitle(L("Açık", "On"), 84, 42, &FreeSansTR7pt);
  if (teacherModeOn) cv->drawRoundRect(12, 26, 144, 32, 8, TFT_WHITE);
  cv->fillRoundRect(164, 26, 144, 32, 8, offBg);
  cv->setTextColor(offFg, offBg);
  drawXIcon(182, 42, offFg);
  drawFreeTitle(L("Kapalı", "Off"), 236, 42, &FreeSansTR7pt);
  if (!teacherModeOn) cv->drawRoundRect(164, 26, 144, 32, 8, TFT_WHITE);

  cv->setTextColor(TFT_DARKGREY, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("Alarmda 'Dikkat' butonu gösterir.", "Shows an Alert button on alarm."), 160, 62, &FreeSansTR6pt);

  char tbuf[48];
  // Dusuk / hipo (sari kart)
  cv->drawRoundRect(10, 74, 300, 46, 8, sevColor(1));
  drawChevronDown(28, 97, sevColor(1));
  cv->setTextColor(sevColor(1), UI_BG);
  cv->setTextDatum(TL_DATUM);
  sprintf(tbuf, L("Hipoglisemi (<%s):", "Hypoglycemia (<%s):"), formatGlucose(alarmLow).c_str());
  dsN(cv, tbuf, 44, 80, 2);
  cv->setTextColor(TFT_WHITE, UI_BG);
  // Bu kart 46px yuksekliginde sabit, iki satir 12px araligina sikica oturuyor;
  // yeni Turkce mini font (FreeSansTR6pt) 14px yukseklikte oldugundan siga sigmaz,
  // kart taskar. Bu yuzden bu iki satir bilincli olarak ASCII font 1'de birakildi.
  dsN(cv, L("Meyve suyu / kup seker ver,", "Give juice or sugar cubes,"), 44, 98, 1);
  dsN(cv, L("15 dk bekle, tekrar olc.", "wait 15 min, recheck."), 44, 110, 1);

  // Kritik / bilinc yok (turuncu kart)
  cv->drawRoundRect(10, 122, 300, 46, 8, sevColor(3));
  drawWarnIcon(28, 145, sevColor(3), TFT_BLACK);
  cv->setTextColor(sevColor(3), UI_BG);
  sprintf(tbuf, L("Siddetli Hipo (<%s):", "Severe hypo (<%s):"), formatGlucose(alarmCritLow).c_str());
  dsN(cv, tbuf, 44, 128, 2);
  cv->setTextColor(TFT_WHITE, UI_BG);
  // Ayni sebep: kart yuksekligi sabit, 12px satir araligi yeni fontun 14px
  // yuksekligine yetmiyor -> tasma riski. Bilincli olarak ASCII font 1'de kaldi.
  dsN(cv, L("Agizdan hicbir sey vermeyin.", "Give nothing by mouth."), 44, 146, 1);
  dsN(cv, L("Glukagon yapin, 112 + aile arayin.", "Give glucagon, call emergency + family."), 44, 158, 1);

  // Cok yuksek (cyan kart)
  cv->drawRoundRect(10, 170, 300, 34, 8, TFT_CYAN);
  drawChevronUp(28, 187, TFT_CYAN);
  cv->setTextColor(TFT_CYAN, UI_BG);
  sprintf(tbuf, L("Hiperglisemi (>%s):", "Hyperglycemia (>%s):"), formatGlucose(alarmVeryHigh).c_str());
  dsN(cv, tbuf, 44, 176, 2);
  cv->setTextColor(TFT_WHITE, UI_BG);
  drawFreeTitle(L("Su içir, egzersiz yaptırma.", "Water, no exercise."), 44, 194, &FreeSansTR6pt);

  // Geri: ekranin altina tasmasin diye 206'da, 30px yukseklikte
  cv->fillRoundRect(60, 206, 200, 28, 10, 0x333333);
  cv->setTextColor(TFT_WHITE, 0x333333);
  drawBackIcon(140, 220, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 156, 220, &FreeSansTR7pt);
}

void drawDigerScreen() {
  cv->fillScreen(UI_BG);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("DİĞER AYARLAR", "MORE SETTINGS"), 160, 6, &FreeSansTR9pt);
  cv->drawFastHLine(10, 42, 300, 0x2104);

  // Bilgi ve Kılavuz artık QR sayfasından açılıyor (bkz. drawQrScreen), bu
  // yuzden burada sadece 4 madde kaldi; 2x2 daha ferah bir izgaraya sadelestirildi.
  const char* names[] = {L("Dil/Birim","Lang/Unit"), L("Reset","Reset"), L("Rotasyon","Rotate"), L("Yenile","Refresh")};
  for (int i = 0; i < 4; i++) {
    int col = i % 2, row = i / 2;
    int x = 30 + col * 140;
    int y = 56 + row * 64;
    cv->fillRoundRect(x, y, 120, 52, 10, 0x4B72);
    cv->drawRoundRect(x, y, 120, 52, 10, 0x4A69);
    int cx = x + 60, iconY = y + 16;
    switch (i) {
      case 0: drawGlobeIcon(cx, iconY, TFT_WHITE); break;
      case 1: drawPowerIcon(cx, iconY, TFT_WHITE); break;
      case 2: drawRotateIcon(cx, iconY, TFT_WHITE); break;
      case 3: drawRefreshIcon(cx, iconY, TFT_WHITE); break;
    }
    cv->setTextColor(TFT_WHITE, 0x4B72);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(names[i], x + 60, y + 38, &FreeSansTR7pt);
  }
  // Reset (WiFi sifirla / cihazi kapat) artik ayri, sifresiz bir ekranda —
  // yanlislikla dokunma korumasi Evet/Hayir onayindan geliyor.
  cv->fillRoundRect(90, 194, 140, 40, 8, 0x333333);
  cv->drawRoundRect(90, 194, 140, 40, 8, 0x555555);
  cv->setTextColor(TFT_WHITE, 0x333333);
  drawBackIcon(112, 214, TFT_WHITE);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 128, 214, &FreeSansTR7pt);
}

// ---- Ayarlar ana ekrani icin kucuk ikonlar (ozel font/bitmap yok, sadece temel sekiller) ----
void drawBrightnessIcon(int cx, int cy, uint16_t c) {
  cv->fillCircle(cx, cy, 4, c);
  cv->drawLine(cx, cy - 9, cx, cy - 6, c);
  cv->drawLine(cx, cy + 6, cx, cy + 9, c);
  cv->drawLine(cx - 9, cy, cx - 6, cy, c);
  cv->drawLine(cx + 6, cy, cx + 9, cy, c);
  cv->drawLine(cx - 6, cy - 6, cx - 4, cy - 4, c);
  cv->drawLine(cx + 4, cy - 4, cx + 6, cy - 6, c);
  cv->drawLine(cx - 6, cy + 6, cx - 4, cy + 4, c);
  cv->drawLine(cx + 4, cy + 4, cx + 6, cy + 6, c);
}
void drawBrightnessLevelIcon(int cx, int cy, uint16_t c, int level) {
  // Ses ikonundaki seviye dalgalarinin ayni mantigi: gunes ikonu sabit,
  // saginda 0/1/2 kucuk "isik dalgasi" (Dusuk/Orta/Yuksek).
  drawBrightnessIcon(cx, cy, c);
  if (level >= 1) { cv->drawLine(cx + 11, cy - 3, cx + 14, cy, c); cv->drawLine(cx + 14, cy, cx + 11, cy + 3, c); }
  if (level >= 2) { cv->drawLine(cx + 15, cy - 4, cx + 18, cy, c); cv->drawLine(cx + 18, cy, cx + 15, cy + 4, c); }
}
void drawSoundIcon(int cx, int cy, uint16_t c) {
  cv->fillRect(cx - 9, cy - 3, 5, 6, c);
  cv->fillTriangle(cx - 4, cy - 3, cx - 4, cy + 3, cx + 3, cy - 7, c);
  cv->fillTriangle(cx - 4, cy + 3, cx + 3, cy - 7, cx + 3, cy + 7, c);
}
void drawBellIcon(int cx, int cy, uint16_t c) {
  cv->fillCircle(cx, cy - 2, 6, c);
  cv->fillRect(cx - 7, cy - 2, 14, 6, c);
  cv->fillCircle(cx, cy + 7, 2, c);
}
void drawTargetIcon(int cx, int cy, uint16_t c) {
  cv->drawCircle(cx, cy, 8, c);
  cv->drawCircle(cx, cy, 5, c);
  cv->fillCircle(cx, cy, 2, c);
}
void drawQrIcon(int cx, int cy, uint16_t c) {
  cv->drawRect(cx - 9, cy - 9, 18, 18, c);
  cv->fillRect(cx - 7, cy - 7, 6, 6, c);
  cv->fillRect(cx + 1, cy + 1, 6, 6, c);
}
void drawMoreIcon(int cx, int cy, uint16_t c) {
  cv->fillCircle(cx - 7, cy, 2, c);
  cv->fillCircle(cx, cy, 2, c);
  cv->fillCircle(cx + 7, cy, 2, c);
}
// ---- Diger Ayarlar / Alarm alt menusu / Ana Sayfa icin ek ikonlar (ayni ilke: sadece temel sekiller) ----
void drawGlobeIcon(int cx, int cy, uint16_t c) {
  cv->drawCircle(cx, cy, 8, c);
  cv->drawLine(cx - 8, cy, cx + 8, cy, c);
  cv->drawLine(cx, cy - 8, cx, cy + 8, c);
  cv->drawLine(cx - 5, cy - 6, cx + 5, cy - 6, c);
  cv->drawLine(cx - 5, cy + 6, cx + 5, cy + 6, c);
}
void drawInfoIconM(int cx, int cy, uint16_t c) {
  cv->drawCircle(cx, cy, 8, c);
  cv->fillCircle(cx, cy - 4, 1, c);
  cv->fillRect(cx - 1, cy - 1, 2, 6, c);
}
void drawLockIcon(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 4, cy - 2, cx - 4, cy - 6, c);
  cv->drawLine(cx + 4, cy - 2, cx + 4, cy - 6, c);
  cv->drawLine(cx - 4, cy - 6, cx + 4, cy - 6, c);
  cv->fillRoundRect(cx - 7, cy - 2, 14, 10, 2, c);
}
void drawRotateIcon(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 7, cy - 3, cx + 4, cy - 3, c);
  cv->fillTriangle(cx + 4, cy - 6, cx + 4, cy, cx + 9, cy - 3, c);
  cv->drawLine(cx + 7, cy + 3, cx - 4, cy + 3, c);
  cv->fillTriangle(cx - 4, cy, cx - 4, cy + 6, cx - 9, cy + 3, c);
}
void drawRefreshIcon(int cx, int cy, uint16_t c) {
  cv->drawCircle(cx, cy, 7, c);
  cv->fillTriangle(cx + 5, cy - 7, cx + 10, cy - 4, cx + 3, cy - 2, c);
}
void drawBookIcon(int cx, int cy, uint16_t c) {
  cv->drawRoundRect(cx - 8, cy - 6, 16, 13, 2, c);
  cv->drawLine(cx, cy - 6, cx, cy + 6, c);
}
void drawPowerIcon(int cx, int cy, uint16_t c) {
  cv->drawCircle(cx, cy + 1, 7, c);
  cv->drawLine(cx, cy - 8, cx, cy - 1, c);
}
void drawBackIcon(int cx, int cy, uint16_t c) {
  cv->fillTriangle(cx + 5, cy - 7, cx + 5, cy + 7, cx - 5, cy, c);
}
void drawFlashIcon(int cx, int cy, uint16_t c) {
  cv->fillTriangle(cx + 3, cy - 9, cx - 4, cy + 1, cx + 2, cy + 1, c);
  cv->fillTriangle(cx - 3, cy + 9, cx + 4, cy - 1, cx - 1, cy - 1, c);
}
void drawChevronRight(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 3, cy - 6, cx + 3, cy, c);
  cv->drawLine(cx + 3, cy, cx - 3, cy + 6, c);
  cv->drawLine(cx - 2, cy - 6, cx + 4, cy, c);
  cv->drawLine(cx + 4, cy, cx - 2, cy + 6, c);
}
void drawClockIcon(int cx, int cy, uint16_t c) {
  cv->drawCircle(cx, cy, 8, c);
  cv->drawLine(cx, cy, cx, cy - 5, c);
  cv->drawLine(cx, cy, cx + 4, cy + 2, c);
}
void drawGridIcon(int cx, int cy, uint16_t c) {
  cv->fillRoundRect(cx - 8, cy - 8, 6, 6, 1, c);
  cv->fillRoundRect(cx + 2, cy - 8, 6, 6, 1, c);
  cv->fillRoundRect(cx - 8, cy + 2, 6, 6, 1, c);
  cv->fillRoundRect(cx + 2, cy + 2, 6, 6, 1, c);
}
void drawCheckIcon(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 5, cy, cx - 1, cy + 4, c);
  cv->drawLine(cx - 1, cy + 4, cx + 5, cy - 5, c);
}
void drawXIcon(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 4, cy - 4, cx + 4, cy + 4, c);
  cv->drawLine(cx - 4, cy + 4, cx + 4, cy - 4, c);
}
void drawWarnIcon(int cx, int cy, uint16_t c, uint16_t bg) {
  cv->fillTriangle(cx, cy - 8, cx - 8, cy + 7, cx + 8, cy + 7, c);
  cv->fillRect(cx - 1, cy - 3, 2, 6, bg);
  cv->fillCircle(cx, cy + 5, 1, bg);
}
// ---- Hedefler / Ses-Parlaklik ic butonlari ve Zamanlayici icin ek ikonlar ----
void drawChevronUp(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 5, cy + 3, cx, cy - 3, c);
  cv->drawLine(cx, cy - 3, cx + 5, cy + 3, c);
}
void drawChevronDown(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 5, cy - 3, cx, cy + 3, c);
  cv->drawLine(cx, cy + 3, cx + 5, cy - 3, c);
}
void drawDoubleChevronUp(int cx, int cy, uint16_t c) {
  drawChevronUp(cx, cy - 4, c);
  drawChevronUp(cx, cy + 4, c);
}
void drawDoubleChevronDown(int cx, int cy, uint16_t c) {
  drawChevronDown(cx, cy - 4, c);
  drawChevronDown(cx, cy + 4, c);
}
void drawNoDataIcon(int cx, int cy, uint16_t c) {
  cv->drawCircle(cx, cy, 8, c);
  cv->drawLine(cx - 6, cy - 6, cx + 6, cy + 6, c);
}
void drawMuteIcon(int cx, int cy, uint16_t c) {
  cv->fillRect(cx - 9, cy - 3, 5, 6, c);
  cv->fillTriangle(cx - 4, cy - 3, cx - 4, cy + 3, cx + 1, cy - 6, c);
  cv->fillTriangle(cx - 4, cy + 3, cx + 1, cy - 6, cx + 1, cy + 6, c);
  cv->drawLine(cx + 3, cy - 5, cx + 9, cy + 5, c);
  cv->drawLine(cx + 3, cy + 5, cx + 9, cy - 5, c);
}
void drawHourglassIcon(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 7, cy - 9, cx + 7, cy - 9, c);
  cv->drawLine(cx - 7, cy + 9, cx + 7, cy + 9, c);
  cv->drawLine(cx - 7, cy - 9, cx + 2, cy, c);
  cv->drawLine(cx + 7, cy - 9, cx - 2, cy, c);
  cv->drawLine(cx - 7, cy + 9, cx + 2, cy, c);
  cv->drawLine(cx + 7, cy + 9, cx - 2, cy, c);
  cv->fillCircle(cx, cy, 2, c);
}
void drawSparkleIcon(int cx, int cy, uint16_t c) {
  cv->drawLine(cx, cy - 9, cx, cy + 9, c);
  cv->drawLine(cx - 9, cy, cx + 9, cy, c);
  cv->drawLine(cx - 5, cy - 5, cx + 5, cy + 5, c);
  cv->drawLine(cx - 5, cy + 5, cx + 5, cy - 5, c);
}
void drawForkIcon(int cx, int cy, uint16_t c) {
  cv->drawLine(cx - 5, cy - 9, cx - 5, cy + 9, c);
  cv->drawLine(cx - 8, cy - 9, cx - 8, cy - 2, c);
  cv->drawLine(cx - 2, cy - 9, cx - 2, cy - 2, c);
  cv->drawLine(cx - 8, cy - 2, cx - 2, cy - 2, c);
  cv->drawCircle(cx + 6, cy - 6, 3, c);
  cv->drawLine(cx + 6, cy - 3, cx + 6, cy + 9, c);
}
// Ses seviyesine gore hoparlor + 0/1/2 dalga isareti (Sessiz/Dusuk icin 0, Orta icin 1, Yuksek icin 2)
void drawSoundLevelIcon(int cx, int cy, uint16_t c, int waves) {
  drawSoundIcon(cx, cy, c);
  if (waves >= 1) { cv->drawLine(cx + 6, cy - 3, cx + 9, cy, c); cv->drawLine(cx + 9, cy, cx + 6, cy + 3, c); }
  if (waves >= 2) { cv->drawLine(cx + 10, cy - 4, cx + 13, cy, c); cv->drawLine(cx + 13, cy, cx + 10, cy + 4, c); }
}
void drawMoonIcon(int cx, int cy, uint16_t c, uint16_t bg) {
  cv->fillCircle(cx, cy, 7, c);
  cv->fillCircle(cx + 4, cy - 3, 7, bg);
}

// Baslik yazilarinda kullanilan alternatif (GFXFF) yazi tipi. Serbest fontu
// aciyor, metni yaziyor, sonra HEMEN eski numarali font sistemine geri
// donduruyor (setTextFont(1)) — boylece bu ekrandan sonra cizilecek hicbir
// seye sizmiyor. Renk/hizalama (setTextColor/setTextDatum) cagirandan once
// ayarlanmis olmali, bu fonksiyon onlara dokunmuyor. Varsayilan font
// FreeSansTR9pt (ASCII); Turkce karakter gereken tek bir deneme icin
// (Bilgi basligi) ozel FreeSansTR9pt fontu parametre olarak veriliyor.
void drawFreeTitle(const char* text, int x, int y, const GFXfont* font) {
  // TFT_eSPI serbest fontlari arka plan boyamadan (seffaf) ciziyordu;
  // LovyanGFX ayni gorunumu versin diye arka plan rengi gecici kapatiliyor.
  uint32_t fg = cv->getTextStyle().fore_rgb888;
  uint32_t bg = cv->getTextStyle().back_rgb888;
  cv->setFreeFont(font);
  cv->setTextColor(fg);
  cv->drawString(text, x, y);
  cv->setTextColor(fg, bg);
  cv->setTextFont(1);
}

// Pil simgesi sadece arayuz sayfasinda (2. sayfa), ust menu kutusu kapaliyken
// ve alarm yanip sonmesi yokken gosterilir - diger ekranlarin duzeni bozulmasin.
bool batteryIndicatorVisible() {
  if (!inaPresent || batteryPercent < 0) return false;
  if (showErrorScreen || uiPreviewActive || showGuide || ageWarnActive) return false;
  if (isSettingsScreen() || isTimerScreen() || isStatsScreen()) return false;
  if (currentScreen != 1) return false;
  if (uiPageMode >= 5 || uiPageMode == 3) return false;   // yeni temalar: kendi durum cubugu var
  if (uiChromeVisible || uiMenuOpen) return false;
  if (isAlarmActive() && visualAlarmOn && (long)(millis() - visualBurstEnd) < 0) return false;
  return true;
}


// =====================================================================
//  2.2.0 - KAYDIRMALI MENULER, METIN SAYFALARI, (i) YARDIMLARI, SAAT GIRISI
//  Listeler: Ayarlar (0), Alarmlar (1), Modlar (3, subScreen 19).
//  Metin sayfalari (2): Bilgi, Kilavuz, Rehber modu ve (i) yardimlari -
//  buyuk yazi, parmakla kaydirilir. Alt ekranlarin kendisi (Ses,
//  Parlaklik, Hedefler, Gece/Okul ...) ayni kaldi.
// =====================================================================
#define LIST_Y  40
#define LIST_H  200
#define ROW_P   50
#define ROW_H   44
#define UI_LIST_MAX 64

enum ListIcon { IC_BELL = 1, IC_TARGET, IC_SOUND, IC_SUN, IC_GRID, IC_GLOBE, IC_EYE, IC_ROTATE,
                IC_QR, IC_INFO, IC_BOOK, IC_REFRESH, IC_POWER, IC_DDOWN, IC_DUP, IC_FLASH,
                IC_CLOCK, IC_MOON, IC_SPARKLE, IC_MUTE, IC_FORK, IC_SUM };

// Metin sayfasi / yardim kimlikleri
enum TextPageId { TP_NONE = 0, TP_INFO, TP_GUIDE, TP_REHBER, TP_H_FASTFALL, TP_H_FASTRISE,
                  TP_H_SUPPRESS, TP_H_VISUAL, TP_H_SNOOZE, TP_H_COLOR, TP_H_NIGHT, TP_H_SCHOOL,
                  TP_H_SLEEP, TP_H_MIC, TP_KARB, TP_MEAL };

struct UiListItem { uint8_t id; uint8_t icon; uint16_t iconBg; String label; String value; int8_t toggle; uint8_t help; int16_t infoX; };
UiListItem uiItems[UI_LIST_MAX];
int uiItemCount = 0;
int settingsScroll = 0;
int alarmListScroll = 0;
int modesScroll = 0;
int sleepScroll = 0;
int listPressedIdx = -1;

void listAdd(uint8_t id, uint8_t icon, uint16_t bg, const String& label, const String& value, int8_t tog, uint8_t help = TP_NONE) {
  if (uiItemCount >= UI_LIST_MAX) return;
  UiListItem& it = uiItems[uiItemCount++];
  it.id = id; it.icon = icon; it.iconBg = bg; it.label = label; it.value = value;
  it.toggle = tog; it.help = help; it.infoX = -100;
}

String onOffLabel(bool on) { return on ? L("Açık", "On") : L("Kapalı", "Off"); }

// Ayarlar: her yere TEK yerden ulasilir (2.2.0 sadelestirmesi)
void buildSettingsItems() {
  uiItemCount = 0;
  listAdd(0,  IC_BELL,    0xE207, L("Alarmlar", "Alarms"), "", -1);
  listAdd(1,  IC_TARGET,  0x1D0C, L("Hedefler", "Targets"), formatGlucose(alarmLow) + " - " + formatGlucose(alarmHigh), -1);
  listAdd(2,  IC_SUN,     0xF542, L("Ekran/Dil ayarları", "Display/Language"), levelName3(brightnessLevel) + " / " + langLabel(), -1);
  listAdd(8,  IC_QR,      0xB9F4, L("QR / Web", "QR / Web"), L("bilgi, kılavuz", "info, guide"), -1);
  { int pl[P_N]; int pn = pagerList(pl) - 1;
    listAdd(14, IC_GRID, 0x3C1C, L("Sayfaları düzenle", "Edit pages"), String(pn) + L(" sayfa", " pages"), -1); }
  listAdd(12, IC_POWER,   0xA945, L("Sıfırla / Kapat", "Reset / Power"), "", -1);
}

void buildAlarmItems() {
  uiItemCount = 0;
  String snz = (snoozeMinutes <= 0) ? onOffLabel(false)
             : String(snoozeMinutes) + L(" dk", " min");
  listAdd(20, IC_DDOWN, 0xE207, L("Hızlı düşüş", "Fast fall"), "", alarmFastFallOn ? 1 : 0, TP_H_FASTFALL);
  listAdd(21, IC_DUP,   0xF542, L("Hızlı yükseliş", "Fast rise"), "", alarmFastRiseOn ? 1 : 0, TP_H_FASTRISE);
  listAdd(22, IC_SOUND, 0x3C1C, L("Ses bastırma", "Suppress sound"), "", recoverySoundSuppressOn ? 1 : 0, TP_H_SUPPRESS);
  listAdd(23, IC_FLASH, 0xE3C3, L("Görsel alarm", "Visual alarm"), onOffLabel(visualAlarmOn), -1, TP_H_VISUAL);
  listAdd(24, IC_CLOCK, 0x5B75, L("Ertele", "Snooze"), snz, -1, TP_H_SNOOZE);
}

void buildModesItems() {
  uiItemCount = 0;
  listAdd(26, IC_MOON,    0x4235, L("Gece modu", "Night mode"), onOffLabel(nightModeOn), -1, TP_H_NIGHT);
  listAdd(27, IC_BOOK,    0x1D52, L("Okul modu", "School mode"), onOffLabel(schoolModeOn), -1, TP_H_SCHOOL);
  listAdd(28, IC_SPARKLE, 0x8ADB, L("Rehber modu", "Guide mode"), "", teacherModeOn ? 1 : 0, TP_REHBER);
  listAdd(29, IC_MUTE,    0x5B75, L("Rahatsız etme modu", "Do not disturb"), onOffLabel(sleepModeOn), -1, TP_H_SLEEP);
}

String snapSensLabel() {
  if (snapSens == 0) return L("Düşük", "Low");
  if (snapSens == 2) return L("Yüksek", "High");
  return L("Orta", "Medium");
}

void buildSleepItems() {
  uiItemCount = 0;
  listAdd(30, IC_MUTE,  0x5B75, L("Rahatsız etme modu", "Do not disturb"), "", sleepModeOn ? 1 : 0, TP_H_SLEEP);
  listAdd(31, IC_SOUND, 0x3C1C, L("Şıklatma ile aç", "Snap to wake"), "", micSnapOn ? 1 : 0, TP_H_MIC);
  listAdd(32, IC_TARGET,0x1D0C, L("Hassasiyet", "Sensitivity"), snapSensLabel(), -1);
  listAdd(33, IC_SPARKLE,0x8ADB, L("Şıklatma testi", "Snap test"), "", -1);
}

void listDrawIcon(int icon, int cx, int cy, uint16_t bg) {
  cv->fillCircle(cx, cy, 15, bg);
  uint16_t w = TFT_WHITE;
  switch (icon) {
    case IC_BELL:    drawBellIcon(cx, cy, w); break;
    case IC_TARGET:  drawTargetIcon(cx, cy, w); break;
    case IC_SOUND:   drawSoundLevelIcon(cx + 1, cy, w, 2); break;
    case IC_SUN:     drawBrightnessIcon(cx, cy, w); break;
    case IC_GRID:    drawGridIcon(cx, cy, w); break;
    case IC_GLOBE:   drawGlobeIcon(cx, cy, w); break;
    case IC_EYE:     cv->drawEllipse(cx, cy, 9, 5, w); cv->fillCircle(cx, cy, 3, w); break;
    case IC_ROTATE:  drawRotateIcon(cx, cy, w); break;
    case IC_QR:      drawQrIcon(cx, cy, w); break;
    case IC_INFO:    drawInfoIconM(cx, cy, w); break;
    case IC_BOOK:    drawBookIcon(cx, cy, w); break;
    case IC_REFRESH: drawRefreshIcon(cx, cy, w); break;
    case IC_POWER:   drawPowerIcon(cx, cy, w); break;
    case IC_DDOWN:   drawDoubleChevronDown(cx, cy, w); break;
    case IC_DUP:     drawDoubleChevronUp(cx, cy, w); break;
    case IC_FLASH:   drawFlashIcon(cx, cy, w); break;
    case IC_CLOCK:   drawClockIcon(cx, cy, w); break;
    case IC_MOON:    drawMoonIcon(cx - 1, cy + 1, w, bg); break;
    case IC_SPARKLE: drawSparkleIcon(cx, cy, w); break;
    case IC_MUTE:    drawMuteIcon(cx, cy, w); break;
    case IC_FORK:    drawForkIcon(cx, cy, w); break;
    case IC_SUM:     cv->fillRect(cx - 8, cy - 2, 17, 4, w); cv->fillRect(cx - 2, cy - 8, 4, 17, w); break;
  }
}

// kucuk (i) bilgi dugmesi
void drawInfoBadge(lgfx::LovyanGFX* g, int cx, int cy) {
  g->drawCircle(cx, cy, 10, UI_ACCENT);
  g->drawCircle(cx, cy, 9, UI_ACCENT);
  g->fillCircle(cx, cy - 4, 2, UI_ACCENT);
  g->fillRect(cx - 1, cy - 1, 3, 7, UI_ACCENT);
}

void drawListHeader(const char* title, bool nextArrow) {
  cv->fillRect(0, 0, 320, LIST_Y, UI_BG);
  for (int t = 0; t < 3; t++) {            // geri oku
    cv->drawLine(18 + t, 12, 10 + t, 20, UI_ACCENT);
    cv->drawLine(10 + t, 20, 18 + t, 28, UI_ACCENT);
  }
  cv->setTextColor(UI_ACCENT, UI_BG);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 26, 20, &FreeSansTR7pt);
  if (nextArrow) {                          // sonraki sayfa oku
    for (int t = 0; t < 3; t++) {
      cv->drawLine(296 - t, 12, 304 - t, 20, UI_ACCENT);
      cv->drawLine(304 - t, 20, 296 - t, 28, UI_ACCENT);
    }
  }
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(title, 172, 20, &FreeSansTR9pt);
  cv->drawFastHLine(0, LIST_Y - 2, 320, UI_LINE);
}

// Liste/metin govdesi icin cizim hedefi (PSRAM sprite; yoksa dogrudan ekran)
lgfx::LovyanGFX* listTarget(int& oy) {
  if (!listSprOk) {
    listSpr.setPsram(true);
    listSpr.setColorDepth(16);
    listSprOk = (listSpr.createSprite(320, LIST_H) != nullptr);
  }
  oy = listSprOk ? 0 : LIST_Y;
  if (!listSprOk) cv->setClipRect(0, LIST_Y, 320, LIST_H);
  return listSprOk ? (lgfx::LovyanGFX*)&listSpr : (lgfx::LovyanGFX*)&tft;
}
void listFlush() {
  if (!listSprOk) { cv->clearClipRect(); return; }
  if (frameActive) { listSpr.pushSprite(&homeSpr, 0, LIST_Y); return; }   // tam ekran tamponuna
  if (bgFrameOn()) composeBuf((uint16_t*)listSpr.getBuffer(), 320, LIST_H, LIST_Y, 240, currentBg());
  listSpr.pushSprite(0, LIST_Y);
}

void drawScrollbar(lgfx::LovyanGFX* g, int oy, int scroll, int maxS, int contentH) {
  if (maxS <= 0) return;
  int trackY = oy + 4, trackH = LIST_H - 8;
  int thumbH = trackH * LIST_H / contentH;
  if (thumbH < 24) thumbH = 24;
  int thumbY = trackY + (trackH - thumbH) * scroll / maxS;
  g->fillRoundRect(310, trackY, 4, trackH, 2, UI_LINE);
  g->fillRoundRect(310, thumbY, 4, thumbH, 2, UI_ACCENT);
}

int& listScrollRef(int kind) {
  if (kind == 5) return formScroll;
  if (kind == 6) return karbScroll;
  if (kind == 7) return themeScroll;
  if (kind == 1) return alarmListScroll;
  if (kind == 3) return modesScroll;
  if (kind == 4) return sleepScroll;
  return settingsScroll;
}

void drawListBody(int kind) {
  bool tmpKey = bgFrameOn();
  if (tmpKey) uiBg = HOME_KEY;
  int& scroll = listScrollRef(kind);
  int contentH = uiItemCount * ROW_P + 6;
  int maxS = contentH > LIST_H ? contentH - LIST_H : 0;
  if (scroll < 0) scroll = 0;
  if (scroll > maxS) scroll = maxS;
  int oy;
  lgfx::LovyanGFX* g = listTarget(oy);
  lgfx::LovyanGFX* savedCv = cv;
  cv = g;
  g->fillRect(0, oy, 320, LIST_H, UI_BG);
  for (int i = 0; i < uiItemCount; i++) {
    int y = oy + 4 + i * ROW_P - scroll;
    UiListItem& it = uiItems[i];
    it.infoX = -100;
    if (y + ROW_H < oy || y > oy + LIST_H) continue;
    g->fillRoundRect(8, y, 296, ROW_H, 10, (i == listPressedIdx) ? UI_CARD_ON : UI_CARD);
    listDrawIcon(it.icon, 32, y + ROW_H / 2, it.iconBg);
    g->setFont(&FreeSansTR9pt);
    g->setTextColor(TFT_WHITE);
    g->setTextDatum(ML_DATUM);
    g->drawString(it.label.c_str(), 56, y + ROW_H / 2 + 1);
    if (it.help) {
      int ix = 56 + g->textWidth(it.label.c_str()) + 18;
      drawInfoBadge(g, ix, y + ROW_H / 2);
      it.infoX = ix;
    }
    if (it.toggle >= 0) {
      bool on = it.toggle > 0;
      int tx = 246, ty = y + ROW_H / 2 - 12;
      g->fillRoundRect(tx, ty, 48, 24, 12, on ? UI_TOG_ON : UI_TOG_OFF);
      g->fillCircle(on ? tx + 36 : tx + 12, ty + 12, 9, TFT_WHITE);
    } else {
      int cx = 290, cy = y + ROW_H / 2;
      for (int t = 0; t < 2; t++) {
        g->drawLine(cx - 3 + t, cy - 6, cx + 3 + t, cy, UI_SUBTEXT);
        g->drawLine(cx + 3 + t, cy, cx - 3 + t, cy + 6, UI_SUBTEXT);
      }
      if (it.value.length()) {
        g->setFont(&FreeSansTR7pt);
        g->setTextColor(UI_SUBTEXT);
        g->setTextDatum(MR_DATUM);
        g->drawString(it.value.c_str(), 278, cy + 1);
      }
    }
  }
  drawScrollbar(g, oy, scroll, maxS, contentH);
  g->setTextDatum(TL_DATUM);
  cv = savedCv;
  listFlush();
  if (tmpKey) uiBg = 0;
}

void drawSettingsList() { buildSettingsItems(); drawListHeader(L("AYARLAR", "SETTINGS"), false); drawListBody(0); }
void drawAlarmList()    { buildAlarmItems();    drawListHeader(L("ALARMLAR", "ALARMS"), false);  drawListBody(1); }
void drawModesList()    { buildModesItems();    drawListHeader(L("MODLAR", "MODES"), false);     drawListBody(3); }
void drawSleepList()    { buildSleepItems();    drawListHeader(L("RAHATSIZ ETME", "DO NOT DISTURB"), false); drawListBody(4); }

// ---------------------------------------------------------------- METIN SAYFALARI
#define TXT_MAX 200
struct TxtLine { uint8_t st; uint16_t col; int16_t y; String t; };
TxtLine txtL[TXT_MAX];
int txtN = 0, txtH = 0, txtScroll = 0;
int textPageId = TP_NONE;
String txtTitle;

// stiller: 0 baslik, 1 govde, 2 not (kucuk gri), 3 madde, 4 bosluk, 5 madde devami
const lgfx::IFont* txtFont(uint8_t st) {
  (void)st;   // 2.2.0: tum metinler buyuk yazi (notlar sadece gri renkte)
  return (const lgfx::IFont*)&FreeSansTR9pt;
}
int txtLineH(uint8_t st) { if (st == 6) return 48; return st == 0 ? 26 : 22; }

void txtBegin(const String& title) { txtN = 0; txtH = 4; txtScroll = 0; txtTitle = title; }

void txtPush(uint8_t st, uint16_t col, const String& t) {
  if (txtN >= TXT_MAX) return;
  txtL[txtN].st = st; txtL[txtN].col = col; txtL[txtN].y = txtH; txtL[txtN].t = t;
  txtN++;
  txtH += txtLineH(st);
}

// Paragrafi ekran genisligine gore kelime kelime satirlara boler
void txtPara(uint8_t st, uint16_t col, const String& text) {
  if (st == 4) { txtH += 10; return; }
  if (st == 0 && txtN > 0) txtH += 8;
  cv->setFont(txtFont(st));
  int maxW = (st == 3) ? 262 : 286;
  String rest = text;
  bool first = true;
  while (rest.length() > 0) {
    int lastFit = -1, pos = 0;
    while (true) {
      int sp = rest.indexOf(' ', pos);
      String cand = (sp < 0) ? rest : rest.substring(0, sp);
      if (cv->textWidth(cand.c_str()) <= maxW) {
        lastFit = (sp < 0) ? rest.length() : sp;
        if (sp < 0) break;
        pos = sp + 1;
      } else break;
    }
    if (lastFit <= 0) lastFit = (rest.indexOf(' ') > 0) ? rest.indexOf(' ') : rest.length();
    String line = rest.substring(0, lastFit);
    rest = rest.substring(lastFit);
    rest.trim();
    txtPush((st == 3 && !first) ? 5 : st, col, line);
    first = false;
  }
  cv->setTextFont(1);
}

void txtH1(const String& t)   { txtPara(0, TFT_CYAN, t); }
void txtBody(const String& t) { txtPara(1, TFT_WHITE, t); }
void txtNote(const String& t) { txtPara(2, UI_SUBTEXT, t); }
void txtItem(const String& t) { txtPara(3, TFT_WHITE, t); }
void txtButton(const String& t, uint16_t act) { txtH += 8; txtPush(6, act, t); }   // 3.4.6: col = eylem no

void buildTextPage(int id) {
  String gl = formatGlucose(alarmLow), gc = formatGlucose(alarmCritLow), gv = formatGlucose(alarmVeryHigh);
  String un = unitLabel();
  switch (id) {
    case TP_INFO: {
      txtBegin(L("BİLGİ", "INFO"));
      txtH1(L("Bağlantı", "Connection"));
      if (WiFi.status() == WL_CONNECTED) {
        txtBody(String("WiFi: ") + WiFi.SSID());
        txtBody(String("IP: ") + WiFi.localIP().toString());
      } else txtPara(1, sevColor(0), L("WiFi bağlı değil", "WiFi not connected"));
      txtBody(String(L("Kaynak: ", "Source: ")) + sourceLabel());
      txtH1(L("Cihaz", "Device"));
      txtBody(String(L("Ad: ", "Name: ")) + deviceName());
      txtBody(String("ID: ") + thisChipId);
      txtBody(String(L("Sürüm: ", "Version: ")) + FIRMWARE_VERSION);
      txtPara(1, sevColor(2), SIGNATURE_TEXT());
      {
        prefs.begin("metematik", true);
        uint32_t cc = prefs.getUInt("crashcnt", 0);
        prefs.end();
        txtBody(String(L("Otomatik yeniden başlatma: ", "Automatic restarts: ")) + String(cc));
      }
      if (inaPresent) {
        String bt = (batteryPercent < 0) ? String(L("Pil takılı değil", "No battery"))
                  : batteryPctText(L("Pil: %%%d", "Battery: %d%%")) +
                    "  " + String(batteryMv / 1000.0f, 2) + "V" +
                    (batteryCharging ? String(L("  şarj ", "  charging ")) + String(-batteryMa) + " mA"
                                     : String("  ") + String(batteryMa) + " mA");
        txtBody(bt);
      }
      txtH1(L("Telefondan ayar (web)", "Phone settings (web)"));
      txtBody(String("http://") + deviceName() + ".local");
      txtBody(String(L("Şifre: ", "Password: ")) + webPass);
      txtNote(L("Tarayıcı 30 gün hatırlanır. Şifreyi web sayfasından değiştirebilirsin; Cihaz Sıfırla ilk (fabrika) şifreye döndürür; Bilgi ekranında yazar.",
                "The browser is remembered for 30 days. You can change the password on the web page; Device Reset restores the original (factory) password, shown on the Info screen."));
      txtH1(L("İletişim", "Contact"));
      txtBody(CONTACT_EMAIL);
      txtBody(YOUTUBE_HANDLE);
      txtH1(L("Kaynaklar", "Sources"));
      txtBody(L("Veri: Nightscout, Dexcom, LibreLinkUp, Medtrum", "Data: Nightscout, Dexcom, LibreLinkUp, Medtrum"));
      txtNote(L("Bazıları üçüncü taraf API uçları kullanır.", "Some use third-party API endpoints."));
      txtH1(L("Lisanslar", "Licenses"));
      txtNote(L("Yazı tipi: Noto Sans - SIL Open Font License 1.1, Copyright 2022 The Noto Project Authors",
                "Font: Noto Sans - SIL Open Font License 1.1, Copyright 2022 The Noto Project Authors"));
      txtNote("LovyanGFX - FreeBSD License, Copyright (c) lovyan03");
      txtNote("WiFiManager - MIT License, Copyright (c) 2015 tzapu");
      txtNote("ArduinoJson - MIT License, Copyright (c) Benoit Blanchon");
      txtNote("QRCode - MIT License, Copyright (c) 2017 Richard Moore");
      txtNote(L("ESP32 Arduino çekirdeği - LGPL 2.1 / Apache 2.0, Espressif Systems",
                "ESP32 Arduino core - LGPL 2.1 / Apache 2.0, Espressif Systems"));
      txtNote("MeteMatik - GNU GPL v3.0 (github)");   // 4.0.3
      txtNote(L("Lisans metinlerinin tamamı kullanım kılavuzundadır.", "Full license texts are in the user manual."));
      txtH1(L("Önemli", "Important"));
      txtPara(1, sevColor(3), L("Tıbbi cihaz değildir, bilgi amaçlıdır. Tedavi kararları için resmi CGM uygulamanızı esas alın.",
                                "Not a medical device, informational only. Rely on your official CGM app for treatment decisions."));
      txtItem(L("Asıl CGM alarmlarınızı kapatmayın.", "Do not turn off your CGM alarms."));   // 3.6.1
      txtNote(L("MeteMatik; Dexcom, Abbott, Medtrum ve Nightscout ile bağlantılı değildir. Markalar sahiplerine aittir.",
                "MeteMatik is not affiliated with Dexcom, Abbott, Medtrum or Nightscout. Brands belong to their owners."));
      txtNote(L("Açık kaynaklı, ücretsiz bir projedir; hiçbir garanti verilmez.", "A free, open-source project; provided without any warranty."));
      txtButton(L("Önizlemeler", "Previews"), 1);          // 3.4.6
      break;
    }
    case TP_GUIDE: {
      txtBegin(L("KILAVUZ", "GUIDE"));
      txtH1(L("Önemli uyarı", "Important"));
      txtPara(1, sevColor(3), L("İnsülin veya tedavi kararı için KULLANMAYIN.", "Do NOT use for insulin or treatment decisions."));
      txtItem(L("Tıbbi cihaz değildir, sadece bilgilendirme amaçlıdır.", "Not a medical device; informational only."));
      txtItem(L("Resmi CGM uygulamanızı esas alın. Risk kullanıcıya aittir.", "Rely on your official CGM app. Risk is the user's."));
      txtItem(L("Asıl CGM alarmlarınızı kapatmayın.", "Do not turn off your CGM alarms."));
      txtH1(L("Delta", "Delta"));     // 3.6.1
      txtItem(L("Delta: son 5 dakikadaki değişim.", "Delta: the change over the last 5 minutes."));
      txtItem(L("Yanında * varsa veride kesinti olmuştur; değişim 5 dakikaya çevrilerek tahmin edilmiştir.",
                "A * means there was a gap in the data; the change was estimated per 5 minutes."));
      txtItem(L("-- : 15 dakikadan uzun kesinti, değişim bilinmiyor.", "-- : gap longer than 15 minutes, change unknown."));
      txtH1(L("Gezinme", "Navigation"));
      txtItem(L("Ana ekran: seçtiğin tema. Tema uygulamasından değiştirilir.", "Home: the theme you chose. Change it in the Theme app."));
      txtItem(L("Ana ekranda parmağını sola kaydır: uygulamalar açılır.", "Swipe left on the home screen: apps open."));
      txtItem(L("Sağa kaydır ya da BOOT tuşuna kısa bas: bir geri.", "Swipe right or short-press BOOT: back."));
      txtItem(L("Uygulamalar: Tema, Grafik, İstatistik, Sensör, Zamanlayıcı, Karb-Not, Modlar, Ayarlar", "Apps: Theme, Graph, Stats, Sensor, Timer, Carbs, Modes, Settings"));
      txtH1(L("Ayarlar", "Settings"));
      txtBody(L("Liste parmakla yukarı-aşağı kaydırılır, dokununca açılır. Ayar sayfasında değiştir, sonra sağ üstteki Kaydet'e bas; kaydetmeden Geri'ye basarsan sorar. (i) işaretine dokunursan o özelliğin ne işe yaradığı yazar.",
                "Scroll the list with your finger, tap to open. Change a setting, then press Save at the top right; pressing Back without saving asks first. Tap (i) to see what a feature does."));
      txtItem(L("Alarmlar: hızlı düşüş/yükseliş, ses bastırma, görsel alarm, ertele", "Alarms: fast fall/rise, sound suppress, visual alarm, snooze"));
      txtItem(L("Hedefler: kritik düşük / düşük / yüksek / çok yüksek eşikleri ve sesleri", "Targets: critical low / low / high / very high limits and sounds"));
      txtItem(L("Modlar: Gece, Okul, Rehber, Rahatsız etme", "Modes: Night, School, Guide"));
      txtH1(L("Modlar", "Modes"));
      txtItem(L("Gece / Okul: seçtiğin saatlerde ses ve parlaklık otomatik değişir. Saate dokunarak istediğin saati yazabilirsin.",
                "Night / School: sound and brightness change automatically in the chosen hours. Tap a time to type it."));
      txtItem(L("Rehber: alarmda Dikkat butonu çıkar; yanındaki kişiye ne yapacağını adım adım gösterir.",
                "Guide: an Alert button appears on alarm and shows the person nearby what to do, step by step."));
      txtH1(L("Telefondan ayar ve güncelleme", "Phone settings and updates"));
      txtBody(L("Aynı WiFi'deki telefonda Ayarlar > QR / Web ekranındaki QR'ı okut veya tarayıcıda şu adresi aç:",
                "On a phone on the same WiFi, scan the QR in Settings > QR / Web or open this address:"));
      txtBody(String("http://") + deviceName() + ".local");
      txtBody(L("Yazılım güncellemesi: aynı adreste /update sayfasından .bin dosyası yüklenir.",
                "Software update: upload the .bin file on the /update page at the same address."));
      txtH1(L("WiFi kurulumu", "WiFi setup"));
      txtBody(String(L("Kurulum QR'ını okut veya telefonun WiFi listesinden şu ağa bağlan: ", "Scan the setup QR or join this network from the phone's WiFi list: ")) + deviceName());
      txtBody(L("Telefonun kendi hotspot'unu kullanacaksan:", "Using your phone's own hotspot:"));
      txtItem(L("1. Hotspot'u kapat", "1. Turn the hotspot off"));
      txtItem(String(L("2. ", "2. ")) + deviceName() + L(" ağına bağlanıp bilgileri gir", " - join it and fill in the form"));
      txtItem(L("3. Hotspot'u tekrar aç", "3. Turn the hotspot back on"));
      txtNote(L("BOOT tuşuna 2 saniye basmak WiFi / kaynak kurulumunu tekrar açar.", "Holding BOOT for 2 seconds reopens WiFi / source setup."));
      break;
    }
    case TP_REHBER: {
      txtBegin(L("REHBER MODU", "GUIDE MODE"));
      txtBody(L("Açıkken, şeker tehlikeli aralığa girdiğinde ana ekranda Dikkat butonu çıkar. Butona basan kişiye (öğretmen, bakıcı, arkadaş) ne yapması gerektiği adım adım gösterilir.",
                "When on, an Alert button appears on the main screen if glucose enters a dangerous range. It shows the person who taps it (teacher, carer, friend) what to do, step by step."));
      txtNote(L("Açıp kapatmak için Modlar listesindeki anahtarı kullan.", "Use the switch in the Modes list to turn it on or off."));
      txtPara(0, sevColor(1), String(L("Hipoglisemi (", "Hypoglycemia (")) + "<" + gl + " " + un + ")");
      txtItem(L("Meyve suyu veya küp şeker ver.", "Give juice or sugar cubes."));
      txtItem(L("15 dakika bekle, tekrar ölç.", "Wait 15 minutes, check again."));
      txtPara(0, sevColor(0), String(L("Şiddetli hipo (", "Severe hypo (")) + "<" + gc + " " + un + ")");
      txtItem(L("Bilinç yoksa ağızdan HİÇBİR ŞEY verme.", "If unconscious, give NOTHING by mouth."));
      txtItem(L("Glukagon yap, 112'yi ve aileyi ara.", "Give glucagon, call emergency services and the family."));
      txtPara(0, sevColor(3), String(L("Hiperglisemi (", "Hyperglycemia (")) + ">" + gv + " " + un + ")");
      txtItem(L("Su içir, egzersiz yaptırma.", "Give water, no exercise."));
      txtItem(L("Aileye haber ver.", "Let the family know."));
      txtNote(L("Eşikler Hedefler ayarından gelir.", "Limits come from the Targets setting."));
      txtNote(L("Bu adımlar bir diyabet hemşiresi tarafından gözden geçirildi; doktorunuzun talimatlarının yerini tutmaz.",
                "These steps were reviewed by a diabetes nurse; they do not replace your doctor's instructions."));   // 3.6.1
      break;
    }
    case TP_H_FASTFALL:
      txtBegin(L("HIZLI DÜŞÜŞ", "FAST FALL"));
      txtBody(L("Şeker çok hızlı düşerken (CGM'de çift aşağı ok) uyarır; düşük eşiğine inmeden önce haber verir.",
                "Warns while glucose falls very fast (double down arrow on the CGM), before it reaches the low limit."));
      txtBody(L("Kendine özgü kısa 3'lü bip çalar. Eşik alarmları her zaman önceliklidir.",
                "Plays its own short triple beep. Limit alarms always take priority."));
      txtNote(L("Sadece Nightscout ve Dexcom'da çalışır; LibreLinkUp çift ok bilgisi vermez. Sadece 10 dakikadan yeni veride tetiklenir.",
                "Works with Nightscout and Dexcom only; LibreLinkUp does not report double arrows. Only fires on data newer than 10 minutes."));
      break;
    case TP_H_FASTRISE:
      txtBegin(L("HIZLI YÜKSELİŞ", "FAST RISE"));
      txtBody(L("Şeker çok hızlı yükselirken (CGM'de çift yukarı ok) uyarır; yüksek eşiğine çıkmadan önce haber verir.",
                "Warns while glucose rises very fast (double up arrow on the CGM), before it reaches the high limit."));
      txtBody(L("Yemeklerden sonra sık çalabileceği için varsayılan olarak kapalıdır.", "Off by default, as it can go off often after meals."));
      txtNote(L("Sadece Nightscout ve Dexcom'da çalışır.", "Works with Nightscout and Dexcom only."));
      break;
    case TP_H_SUPPRESS:
      txtBegin(L("SES BASTIRMA", "SOUND SUPPRESS"));
      txtBody(L("Şeker eşik dışında olsa da normale doğru belirgin şekilde dönüyorsa (yüksekte en az 5 mg/dL düşüş; düşükte yukarı ok ve en az 5 mg/dL artış) alarm SESİ otomatik susar.",
                "If glucose is out of range but clearly heading back to normal (high: a drop of at least 5 mg/dL; low: an up arrow and a rise of at least 5 mg/dL), the alarm SOUND mutes automatically."));
      txtBody(L("Görsel alarm (renk / yanıp sönme) aynen devam eder.", "The visual alarm (color / blinking) continues as usual."));
      txtPara(1, sevColor(0), L("Kritik düşükte ASLA geçerli değildir, o alarm her zaman çalar.", "Never applies to critical low - that alarm always sounds."));
      break;
    case TP_H_VISUAL:
      txtBegin(L("GÖRSEL ALARM", "VISUAL ALARM"));
      txtBody(L("Alarm sırasında ekran seçtiğin renkte yanıp söner. Ses kapalıyken, gece veya okul modunda bile uyarı görülür.",
                "During an alarm the screen blinks in the color you choose. The alert is visible even when sound is off or in night/school mode."));
      txtNote(L("Rengi ve açık/kapalı durumunu Görsel alarm sayfasından seçebilirsin.", "Choose the color and on/off on the Visual alarm page."));
      break;
    case TP_H_SNOOZE:
      txtBegin(L("ERTELE", "SNOOZE"));
      txtBody(L("Alarm varken ana ekranda Ertele butonu çıkar. Basınca alarm (ses ve yanıp sönme) seçtiğin süre boyunca durur: 15, 30 veya 60 dakika.",
                "During an alarm a Snooze button appears on the main screen. Tapping it stops the alarm (sound and blinking) for the chosen time: 15, 30 or 60 minutes."));
      txtBody(L("Süre bitince şeker hâlâ eşik dışındaysa alarm yeniden başlar.", "When the time is up, the alarm starts again if glucose is still out of range."));
      txtNote(L("Cihaz sessizdeyse buton gösterilmez. Kapalı seçilirse buton hiç çıkmaz.", "The button is hidden when the device is silent. Choose Off to never show it."));
      break;
    case TP_H_COLOR:
      txtBegin(L("RENK DOSTU", "COLOR-SAFE"));
      txtBody(L("Renk körlüğü olanlar için: kırmızı / sarı / yeşil / turuncu yerine birbirinden kolay ayırt edilen tonlar kullanılır.",
                "For color-blind users: easy-to-tell-apart shades replace red / yellow / green / orange."));
      txtItem(L("Kritik: vermilyon (koyu turuncu)", "Critical: vermilion (deep orange)"));
      txtItem(L("Düşük: gök mavisi", "Low: sky blue"));
      txtItem(L("Normal: mavimsi yeşil", "Normal: bluish green"));
      txtItem(L("Yüksek: amber", "High: amber"));
      txtNote(L("Tüm sayfalara ve grafiğe uygulanır.", "Applies to every page and the graph."));
      break;
    case TP_H_NIGHT:
      txtBegin(L("GECE MODU", "NIGHT MODE"));
      txtBody(L("Belirlediğin saatler arasında ekran parlaklığı ve ses seviyesi otomatik olarak gece değerlerine iner. Saat bitince normale döner.",
                "Between the hours you set, screen brightness and sound drop to the night values automatically, then return to normal."));
      txtBody(L("Gece sesi Sessiz seçilmediyse alarmlar çalmaya devam eder.", "Alarms still sound unless the night sound is set to Silent."));
      txtNote(L("Saate dokunarak istediğin saati yazabilirsin. Gece ve okul saatleri çakışırsa gece modu geçerli olur.", "Tap a time to type it. If night and school hours overlap, night mode wins."));
      break;
    case TP_H_SLEEP:
      txtBegin(L("RAHATSIZ ETME", "DO NOT DISTURB"));
      txtBody(L("Açıkken, şeker hedef aralıktaysa (düşük ile yüksek eşik arası) ekrana 15 saniye dokunulmayınca ekran kapanır.",
                "When on, the screen turns off after 15 seconds without touch while glucose is in range (between the low and high limits)."));
      txtBody(L("Ekranı açmak için ekrana dokun ya da parmaklarını iki kez şıklat. Ana ekran 15 saniye görünür, sonra tekrar kapanır.",
                "To see it, touch the screen or snap your fingers twice. The main screen shows for 15 seconds, then turns off again."));
      txtPara(1, sevColor(3), L("Şeker kritik düşük, düşük, yüksek veya çok yüksek olursa, hızlı düşüş/yükseliş varsa ya da veri gelmiyorsa ekran kendiliğinden açılır ve açık kalır.",
                                "If glucose is critical low, low, high or very high, falls/rises fast, or no data arrives, the screen turns on by itself and stays on."));
      txtNote(L("Alarm sesleri rahatsız etme modunda da aynen çalar.", "Alarm sounds work as usual in sleep mode."));
      break;
    case TP_H_MIC:
      txtBegin(L("ŞIKLATMA İLE AÇ", "SNAP TO WAKE"));
      txtBody(L("Ekran uykudayken mikrofon sadece kısa ve ani sesleri (parmak şıklatma) dinler. İki şıklatma arka arkaya gelirse ekran açılır.",
                "While the screen sleeps, the microphone only listens for short, sharp sounds (finger snaps). Two snaps in a row turn the screen on."));
      txtPara(1, sevColor(2), L("Ses kaydedilmez ve hiçbir yere gönderilmez; sadece o anki ses şiddetine bakılır. Ekran açıkken mikrofon tamamen kapalıdır.",
                                "Nothing is recorded or sent anywhere; only the momentary loudness is checked. The microphone is fully off while the screen is on."));
      txtNote(L("Algılamıyorsa Hassasiyet'i artır, kendiliğinden açılıyorsa azalt. Şıklatma testi ile deneyebilirsin.",
                "If it misses snaps, raise the Sensitivity; if it wakes by itself, lower it. Try it with Snap test."));
      break;
    case TP_MEAL: {
      txtBegin(L("MENÜ", "MEAL"));
      if (mealN == 0) txtBody(L("Menü boş.", "The meal is empty."));
      for (int i = 0; i < mealN; i++)
        txtBody(mealRecs[i].name + ":  " + karbNum(mealRecs[i].amt) + " " + karbUnitName(mealRecs[i].type, false) +
                "  =  " + karbNum(mealRecs[i].carb) + " g");
      if (karbMenuCount > mealN) txtNote(String(karbMenuCount - mealN) + L(" kalem daha (liste dolu)", " more items (list full)"));
      txtH1(String(L("Toplam: ", "Total: ")) + karbNum(karbMenuTotal) + L(" g karb", " g carbs"));
      break;
    }
    case TP_KARB:
      txtBegin(L("KARB-NOT", "CARB NOTES"));
      txtBody(L("Yiyecek listesi telefondan hazırlanır. Aynı WiFi'deki telefonda şu adresi açın:", "The food list is made on a phone. On a phone on the same WiFi open:"));
      txtBody(String("http://") + deviceName() + ".local/karb");
      if (WiFi.status() == WL_CONNECTED) txtBody(String("http://") + WiFi.localIP().toString() + "/karb");
      txtItem(L("Yiyecek adını, birimini (100 g, adet, porsiyon, kase, dilim, bardak, paket) ve karbonhidratını yazıp Kaydet'e basın.", "Enter the food name, unit (100 g, piece, portion, bowl, slice, glass, pack) and carbs, then Save."));
      txtItem(L("Cihazda yiyeceği seçip miktarı yazınca karbonhidrat hesaplanır. Sağ üstteki Ekle ile öğün toplamı tutulur; Menü toplamına dokununca içeriği görünür, Menüyü sıfırla ile temizlenir.",
                "On the device pick a food and type the amount to get the carbs. Add to meal keeps a meal total; tap the total row to reset it."));
      txtNote(L("Değerleri ürün etiketinden kontrol edin. Bu bir hesap yardımcısıdır.", "Check the values on the product label. This is a calculation helper."));
      break;
    case TP_H_SCHOOL:
      txtBegin(L("OKUL MODU", "SCHOOL MODE"));
      txtBody(L("Okul saatlerinde ses ve parlaklık otomatik olarak seçtiğin okul değerlerine geçer; ders sırasında cihaz daha sessiz ve sönük olur.",
                "During school hours sound and brightness switch to your school values automatically, so the device is quieter and dimmer in class."));
      txtNote(L("Saate dokunarak istediğin saati yazabilirsin.", "Tap a time to type it."));
      break;
  }
  if (txtH < LIST_H) txtH = LIST_H;
}

void openTextPage(int id) {
  textPageId = id;
  buildTextPage(id);
  textPageActive = true;
  drawCurrentScreen();
}

void drawTextBody() {
  bool tmpKey = bgFrameOn();
  if (tmpKey) uiBg = HOME_KEY;
  int contentH = txtH + 8;
  int maxS = contentH > LIST_H ? contentH - LIST_H : 0;
  if (txtScroll < 0) txtScroll = 0;
  if (txtScroll > maxS) txtScroll = maxS;
  int oy;
  lgfx::LovyanGFX* g = listTarget(oy);
  g->fillRect(0, oy, 320, LIST_H, UI_BG);
  g->setTextDatum(TL_DATUM);
  for (int i = 0; i < txtN; i++) {
    const TxtLine& l = txtL[i];
    int y = oy + 4 + l.y - txtScroll;
    int h = txtLineH(l.st);
    if (y + h < oy || y > oy + LIST_H) continue;
    if (l.st == 6) {                                   // 3.4.6: dugme
      g->fillRoundRect(8, y + 2, 304, 40, 10, UI_CARD);
      g->fillCircle(32, y + 22, 13, 0x93C7);
      g->fillTriangle(28, y + 15, 28, y + 29, 39, y + 22, TFT_WHITE);
      g->setFont((const lgfx::IFont*)&FreeSansTR9pt);
      g->setTextColor(TFT_WHITE);
      g->setTextDatum(ML_DATUM);
      g->drawString(l.t.c_str(), 54, y + 22);
      g->fillTriangle(290, y + 15, 290, y + 29, 298, y + 22, UI_SUBTEXT);
      g->setTextDatum(TL_DATUM);
      continue;
    }
    g->setFont(txtFont(l.st));
    g->setTextColor(l.col);
    int x = 12;
    if (l.st == 3) { g->fillCircle(18, y + 9, 3, UI_ACCENT); x = 30; }
    else if (l.st == 5) x = 30;
    g->drawString(l.t.c_str(), x, y + 2);
    if (l.st == 0) g->drawFastHLine(12, y + h - 4, 296, UI_LINE);
  }
  drawScrollbar(g, oy, txtScroll, maxS, contentH);
  listFlush();
  if (tmpKey) uiBg = 0;
}

void drawTextPage() {
  cv->fillScreen(UI_BG);
  drawListHeader(txtTitle.c_str(), false);
  drawTextBody();
}

// ---------------------------------------------------------------- SAAT GIRISI (tus takimi)
int* tpHour = nullptr;
int* tpMin = nullptr;
String tpTitle;
char tpDig[10] = {0};
int tpLen = 0;
// 2.4.0: ayni tus takimi sayi girisi icin de kullaniliyor (hedef esikleri)
int tpMode = 0;            // 0 = saat, 1 = sayi, 4 = dil secimi (3.6.0)
int* tpNum = nullptr;
int tpMinV = 0, tpMaxV = 0;
String tpUnit;
unsigned long tpErrUntil = 0;

void openTimePicker(const char* title, int* h, int* m) {
  tpMode = 0;
  tpTitle = title; tpHour = h; tpMin = m; tpLen = 0; memset(tpDig, 0, sizeof(tpDig));
  timePickerActive = true;
}

void openNumberPicker(const char* title, int* v, int mn, int mx, const char* unit) {
  tpMode = 1;
  tpTitle = title; tpNum = v; tpMinV = mn; tpMaxV = mx; tpUnit = unit;
  tpLen = 0; memset(tpDig, 0, sizeof(tpDig)); tpErrUntil = 0;
  timePickerActive = true;
}

static const char* TP_KEYS[12] = {"1", "2", "3", "<", "4", "5", "6", "0", "7", "8", "9", "OK"};

// 3.6.0: dil secimi - 3x3 izgara, dil adlari kendi dillerinde
void openLangPicker() {
  tpMode = 4;
  tpTitle = (langIdx == 1) ? String("Language") : String(L("Dil", "Language")) + " / Language";
  timePickerActive = true;
}

void drawLangPicker() {
  for (int k = 0; k < LANG_COUNT; k++) {
    int c = k % 3, r = k / 3;
    int x = 8 + c * 102, y = 48 + r * 62;
    bool sel = (k == langIdx);
    uint16_t bg = sel ? SEL_BG : UNSEL_BG;
    cv->fillRoundRect(x, y, 98, 56, 8, bg);
    cv->drawRoundRect(x, y, 98, 56, 8, sel ? TFT_WHITE : 0x4A69);
    cv->setTextColor(sel ? SEL_FG : UNSEL_FG, bg);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(LANG_NAMES[k], x + 49, y + 28, &FreeSansTR7pt);
  }
}

void langPickerTap(int x, int y) {
  if (y < 48) { if (y < LIST_Y && x < 70) timePickerActive = false; return; }   // iptal
  int c = (x - 8) / 102, r = (y - 48) / 62;
  if (c < 0 || c > 2 || r < 0 || r > 2) return;
  int k = r * 3 + c;
  if (k >= LANG_COUNT) return;
  langIdx = k;                       // 3.6.1: kalici kayit form Kaydet ile (saveSettings)
  timePickerActive = false;
}

void drawTimePicker() {
  cv->fillScreen(UI_BG);
  drawListHeader(tpTitle.c_str(), false);
  if (tpMode == 4) { drawLangPicker(); return; }
  char disp[12];
  uint16_t col = TFT_WHITE;
  if (tpMode == 3) {
    char dd[28];
    lifeDisplay(dd);
    cv->setTextColor(tpLen ? (uint16_t)TFT_WHITE : UI_SUBTEXT, UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(dd, 160, 62, &FreeSansTR9pt);
    bool err = (long)(millis() - tpErrUntil) < 0;
    cv->setTextColor(err ? sevColor(0) : UI_SUBTEXT, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Gün (2 hane) + saat (2 hane)", "Days (2 digits) + hours (2 digits)"), 160, 80, &FreeSansTR7pt);
  } else if (tpMode == 2) {
    char dd[28];
    dtpDisplay(dd);
    cv->setTextColor(tpLen ? (uint16_t)TFT_WHITE : UI_SUBTEXT, UI_BG);
    cv->setTextDatum(MC_DATUM);
    dsN(cv, dd, 160, 60, 4);
    bool err = (long)(millis() - tpErrUntil) < 0;
    cv->setTextColor(err ? sevColor(0) : UI_SUBTEXT, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(L("Gün, ay, saat, dakika: 8 rakam", "Day, month, hour, minute: 8 digits"), 160, 80, &FreeSansTR7pt);
  } else if (tpMode == 1) {
    if (tpLen == 0) { sprintf(disp, "%d", *tpNum); col = UI_SUBTEXT; }
    else { strncpy(disp, tpDig, sizeof(disp)); disp[tpLen] = 0; }
    cv->setTextColor(col, UI_BG);
    cv->setTextDatum(MC_DATUM);
    cv->setTextSize(1.5f);
    dsN(cv, disp, 160, 64, 4);
    cv->setTextSize(1);
    char rng[40];
    sprintf(rng, "%s %d - %d %s", L("Aralık:", "Range:"), tpMinV, tpMaxV, tpUnit.c_str());
    bool err = (long)(millis() - tpErrUntil) < 0;
    cv->setTextColor(err ? sevColor(0) : UI_SUBTEXT, UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(rng, 160, 86, &FreeSansTR7pt);
  } else {
    if (tpLen == 0) { sprintf(disp, "%02d:%02d", *tpHour, *tpMin); col = UI_SUBTEXT; }
    else {
      char d[4];
      for (int i = 0; i < 4; i++) d[i] = (i < tpLen) ? tpDig[i] : '_';
      sprintf(disp, "%c%c:%c%c", d[0], d[1], d[2], d[3]);
    }
    cv->setTextColor(col, UI_BG);
    cv->setTextDatum(MC_DATUM);
    cv->setTextSize(1.5f);
    dsN(cv, disp, 160, 70, 4);
    cv->setTextSize(1);
  }
  for (int k = 0; k < 12; k++) {
    int c = k % 4, r = k / 4;
    int x = 10 + c * 76, y = 104 + r * 45;
    uint16_t bg = UI_CARD;
    if (k == 3) bg = 0x6124;                 // sil
    if (k == 11) bg = (tpMode == 3 ? (tpLen == 0 || tpLen == 2 || tpLen == 4) : tpMode == 2 ? (tpLen == 0 || tpLen == 8) : (tpMode == 1 || tpLen == 0 || tpLen == 1 || tpLen == 2 || tpLen == 4)) ? 0x0B5E : 0x3186;  // tamam
    cv->fillRoundRect(x, y, 70, 40, 8, bg);
    cv->setTextColor(TFT_WHITE, bg);
    cv->setTextDatum(MC_DATUM);
    if (k == 3) drawFreeTitle(L("Sil", "Del"), x + 35, y + 21, &FreeSansTR9pt);
    else if (k == 11) drawFreeTitle(L("Tamam", "OK"), x + 35, y + 21, &FreeSansTR9pt);
    else dsN(cv, TP_KEYS[k], x + 35, y + 21, 4);
  }
}

void timePickerTap(int x, int y) {
  if (tpMode == 4) { langPickerTap(x, y); return; }
  if (y < LIST_Y) { if (x < 70) timePickerActive = false; return; }   // iptal
  if (y < 104) return;
  int c = (x - 10) / 76, r = (y - 104) / 45;
  if (c < 0 || c > 3 || r < 0 || r > 2) return;
  int k = r * 4 + c;
  if (tpMode == 2) { dtpTap(k); return; }
  if (tpMode == 3) { lifeTap(k); return; }
  if (k == 3) { if (tpLen > 0) tpDig[--tpLen] = 0; return; }
  if (tpMode == 1) {                          // ---- sayi girisi
    if (k == 11) {
      if (tpLen == 0) { timePickerActive = false; return; }
      int v = atoi(tpDig);
      if (v < tpMinV || v > tpMaxV) { tpErrUntil = millis() + 2000; tpLen = 0; memset(tpDig, 0, sizeof(tpDig)); return; }
      *tpNum = v;
      timePickerActive = false;
      return;
    }
    if (tpLen >= 3) return;
    if (tpLen == 0 && TP_KEYS[k][0] == '0') return;   // basta sifir yok
    tpDig[tpLen++] = TP_KEYS[k][0];
    tpDig[tpLen] = 0;
    return;
  }
  if (k == 11) {
    if (tpLen == 0) { timePickerActive = false; return; }
    int h, m = 0;
    if (tpLen == 1) h = tpDig[0] - '0';
    else if (tpLen == 2) h = (tpDig[0] - '0') * 10 + (tpDig[1] - '0');
    else if (tpLen == 4) { h = (tpDig[0] - '0') * 10 + (tpDig[1] - '0'); m = (tpDig[2] - '0') * 10 + (tpDig[3] - '0'); }
    else return;                              // 3 hane: eksik, bekle
    if (h > 23 || m > 59) return;
    *tpHour = h; *tpMin = m;
    timePickerActive = false;
    return;
  }
  if (tpLen >= 4) return;
  char d = TP_KEYS[k][0];
  int v = d - '0';
  // gecerli saat/dakika disinda rakam kabul etme
  if (tpLen == 0 && v > 2) { tpDig[0] = '0'; tpDig[1] = d; tpLen = 2; return; }   // "7" -> 07
  if (tpLen == 1 && tpDig[0] == '2' && v > 3) return;
  if (tpLen == 2 && v > 5) return;
  tpDig[tpLen++] = d;
}

// ---------------------------------------------------------------- GORSEL ALARM SAYFASI
void drawVisualAlarmPage() {
  drawListHeader(L("GÖRSEL ALARM", "VISUAL ALARM"), false);
  // ac/kapa satiri
  cv->fillRoundRect(8, 46, 304, 44, 10, UI_CARD);
  drawFlashIcon(32, 68, visualAlarmOn ? getAlarmColor() : UI_SUBTEXT);
  cv->setTextColor(TFT_WHITE, UI_CARD);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Yanıp sönme", "Blinking"), 56, 69, &FreeSansTR9pt);
  int tx = 250, ty = 56;
  cv->fillRoundRect(tx, ty, 48, 24, 12, visualAlarmOn ? UI_TOG_ON : UI_TOG_OFF);
  cv->fillCircle(visualAlarmOn ? tx + 36 : tx + 12, ty + 12, 9, TFT_WHITE);

  cv->setTextColor(UI_SUBTEXT, UI_BG);
  cv->setTextDatum(TL_DATUM);
  drawFreeTitle(L("Renk seç", "Choose color"), 12, 98, &FreeSansTR7pt);
  for (int i = 0; i < 6; i++) {
    int col = i % 3, row = i / 3;
    int x = 12 + col * 100, y = 118 + row * 58;
    bool sel = visualAlarmOn && (alarmColorIdx == i);
    uint16_t fill = visualAlarmOn ? colorVals[i] : 0x4208;
    cv->fillRoundRect(x, y, 94, 50, 10, fill);
    if (sel) {
      cv->drawRoundRect(x - 2, y - 2, 98, 54, 12, TFT_WHITE);
      cv->drawRoundRect(x - 3, y - 3, 100, 56, 13, TFT_WHITE);
    }
    cv->setTextColor(visualAlarmOn ? TFT_BLACK : UI_SUBTEXT, fill);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(colorName(i), x + 47, y + 25, &FreeSansTR7pt);
  }
}

// =====================================================================
//  2.3.0 - UYKU MODU + PARMAK SIKLATMA (dahili PDM mikrofon, GPIO9/10)
//  Mikrofon SADECE ekran uykudayken (ve Siklatma testi ekraninda) acilir.
//  Ses kaydedilmez/gonderilmez: 10 ms'lik bloklarin tepe seviyesine bakilir.
// =====================================================================
I2SClass micI2s;

bool micTestScreenOn() { return isSettingsScreen() && subScreen == 21 && !screenAsleep; }

void micTask(void*) {
  static int16_t buf[160];                 // 10 ms @ 16 kHz
  float hpX = 0, hpY = 0;
  int bg = 300;
  bool inT = false;
  int tBlocks = 0;
  unsigned long lastSnap = 0, lastEnd = 0;
  // 2.4.0: kullanici testi - eski "Yuksek" (1000) yakindan iyi algiliyordu,
  // Orta/Dusuk zordu. Yeni olcek: Dusuk = eski Yuksek, Orta/Yuksek daha hassas.
  static const int kMul10[3] = {40, 30, 25};  // arka plan x 4.0 / 3.0 / 2.5
  static const int kMin[3] = {1000, 600, 380};
  for (;;) {
    if (!micWanted) {
      if (micRunning) { micI2s.end(); micRunning = false; }
      micPeak = 0;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!micRunning) {
      pinMode(AUDIO_SEL_PIN, OUTPUT);
      digitalWrite(AUDIO_SEL_PIN, HIGH);   // mikrofon yolu
      micI2s.setPinsPdmRx(MIC_CLK_PIN, MIC_DATA_PIN);
      if (!micI2s.begin(I2S_MODE_PDM_RX, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      micRunning = true;
      bg = 300; inT = false; lastSnap = 0; hpX = hpY = 0;
      unsigned long t0 = millis();          // acilis parazitini at
      while (millis() - t0 < 250) micI2s.readBytes((char*)buf, sizeof(buf));
    }
    size_t n = micI2s.readBytes((char*)buf, sizeof(buf));
    if (n < sizeof(buf)) { vTaskDelay(1); continue; }

    int peak = 0;
    for (int i = 0; i < 160; i++) {       // DC'yi atan yuksek geciren filtre
      float x = buf[i];
      float y = x - hpX + 0.995f * hpY;
      hpX = x; hpY = y;
      int a = (int)fabsf(y);
      if (a > peak) peak = a;
    }
    unsigned long now = millis();
    int sens = snapSens;
    if (sens < 0 || sens > 2) sens = 1;
    int thr = bg * kMul10[sens] / 10;
    if (thr < kMin[sens]) thr = kMin[sens];
    micPeak = peak; micBg = bg; micThr = thr;
    bool toneBusy = (now - lastToneMs) < 400;   // kendi bip sesimizi yok say

    if (!inT) {
      if (!toneBusy && peak > thr && now - lastEnd > 60) { inT = true; tBlocks = 1; }
      else { bg = (bg * 31 + peak) / 32; if (bg < 50) bg = 50; }
    } else {
      tBlocks++;
      if (peak < thr / 2) {
        inT = false; lastEnd = now;
        if (tBlocks <= 12) {                     // <= ~120 ms: kisa ve keskin = siklatma
          snapSingles = snapSingles + 1;
          if (lastSnap && now - lastSnap >= 120 && now - lastSnap <= 900) { snapFlag = true; lastSnap = 0; }
          else lastSnap = now;
        }
      } else if (tBlocks > 30) { inT = false; lastEnd = now; }   // uzun ses (konusma vb.): siklatma degil
    }
    if (lastSnap && now - lastSnap > 900) lastSnap = 0;
  }
}

void startMicTask() {
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 1, nullptr, 0);
}

// Seker hedef araliktaysa ve alarm gerektiren bir durum yoksa true
bool sleepGlucoseNormal() {
  if (!dataValid) return false;
  if (staleAlarmMin > 0 && readingAgeSec > (unsigned long)staleAlarmMin * 60UL) return false;
  if (readingAgeSec > 20UL * 60UL) return false;          // eski veri: guvenlik icin uyuma
  if (sgv < alarmLow || sgv > alarmHigh) return false;    // kritik dusuk/dusuk/yuksek/cok yuksek
  if (fastFallActive() || fastRiseActive()) return false;
  return true;
}

void sleepEnter() {
  formAbandon();
  teacherStep = 0; teacherGlucagonPage = 0;      // seker normal: rehber akisi biter
  teacherPreview = 0; previewVideo = false;
  if (currentScreen == SCR_VIDEO) { videoStop(); currentScreen = homeScreenIdx(); }
  screenAsleep = true;
  textPageActive = false; timePickerActive = false;
  showGuide = false; uiMenuOpen = false; graphMenuOpen = false;
  ledcWrite(BACKLIGHT_PIN, 0);
}

// timed = true: dokunus/siklatma ile 15 sn goster; false: seker aralik disi, acik kal
void sleepWake(bool timed) {
  bool was = screenAsleep;
  screenAsleep = false;
  if (timed) sleepWakeUntil = millis() + SLEEP_SHOW_MS;
  lastUserTouchMs = millis();
  if (was) {
    formAbandon(); textPageActive = false; timePickerActive = false;
    if (currentScreen == SCR_VIDEO) videoStop();
    currentScreen = homeScreenIdx(); subScreen = 0;
    applyRotation();
    drawCurrentScreen();
    applyBrightness();
  }
}

void serviceSleep() {
  unsigned long now = millis();
  bool normal = sleepGlucoseNormal();
  if (screenAsleep) {
    if (!sleepModeOn || !normal) sleepWake(false);
    else if (snapFlag) { snapFlag = false; sleepWake(true); }
  } else {
    if (!micTestScreenOn()) snapFlag = false;
    if (sleepModeOn && normal && !micTestScreenOn() && !(currentScreen == SCR_VIDEO && videoPlaying) &&
        (long)(now - sleepWakeUntil) >= 0 && now - lastUserTouchMs > SLEEP_SHOW_MS) {
      sleepEnter();
    }
  }
  micWanted = (screenAsleep && sleepModeOn && micSnapOn) || micTestScreenOn();
}

// ---------------------------------------------------------------- Siklatma testi ekrani
unsigned long micTestLastDraw = 0;

void drawMicTestDynamic() {
  const int bx = 20, bw = 280;
  auto sc = [&](int v) -> int {
    if (v < 0) v = 0;
    if (v > 20000) v = 20000;
    return (int)(sqrtf(v / 20000.0f) * bw);
  };
  int pk = micPeak, th = micThr;
  int p = sc(pk), t = sc(th);
  cv->fillRect(0, 82, 320, 38, UI_BG);
  cv->fillRoundRect(bx, 88, bw, 26, 6, UI_CARD);
  uint16_t col = (pk > th) ? sevColor(2) : UI_ACCENT;
  if (p > 4) cv->fillRoundRect(bx, 88, p, 26, 6, col);
  cv->fillRect(bx + t - 1, 84, 3, 34, sevColor(3));        // esik cizgisi

  char b[48];
  cv->fillRect(0, 124, 320, 22, UI_BG);
  sprintf(b, "%s %d   %s %d", L("Ses:", "Level:"), pk, L("Eşik:", "Threshold:"), th);
  cv->setTextColor(UI_SUBTEXT, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(b, 160, 126, &FreeSansTR7pt);

  cv->fillRect(0, 150, 320, 32, UI_BG);
  sprintf(b, "%s %d", L("Şıklatma:", "Snaps:"), (int)snapSingles);
  cv->setTextColor(TFT_WHITE, UI_BG);
  drawFreeTitle(b, 160, 154, &FreeSansTR9pt);

  cv->fillRect(0, 186, 320, 42, UI_BG);
  if ((long)(millis() - snapShownUntil) < 0) {
    cv->fillRoundRect(24, 188, 272, 38, 10, sevColor(2));
    cv->setTextColor(TFT_BLACK, sevColor(2));
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("ÇİFT ŞIKLATMA ALGILANDI", "DOUBLE SNAP DETECTED"), 160, 207, &FreeSansTR7pt);
  } else {
    cv->setTextColor(micRunning ? UI_SUBTEXT : sevColor(3), UI_BG);
    cv->setTextDatum(TC_DATUM);
    String hint = micRunning ? String(L("Hassasiyet: ", "Sensitivity: ")) + snapSensLabel() + L("  -  dokun: sıfırla", "  -  tap: reset")
                             : String(L("Mikrofon başlatılıyor...", "Starting microphone..."));
    drawFreeTitle(hint.c_str(), 160, 196, &FreeSansTR7pt);
  }
}

void drawMicTestScreen() {
  cv->fillScreen(UI_BG);
  drawListHeader(L("ŞIKLATMA TESTİ", "SNAP TEST"), false);
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("Parmaklarını iki kez şıklat", "Snap your fingers twice"), 160, 52, &FreeSansTR9pt);
  drawMicTestDynamic();
}

void serviceMicTestScreen() {
  if (!micTestScreenOn()) return;
  if (snapFlag) { snapFlag = false; snapShownUntil = millis() + 1500; }
  if (millis() - micTestLastDraw > 90) { micTestLastDraw = millis(); drawMicTestDynamic(); }
}

// =====================================================================
//  2.5.0/2.6.0 - EKRAN TAMPONU (ARKA PLAN TUM SAYFALARDA), TEMALAR,
//  UYGULAMALAR SAYFASI, TEMA SECIMI
// =====================================================================
bool homeSprOk = false, homeSprFail = false;
int sprW = 0, sprH = 0;
uint16_t* bgBuf = nullptr;
int bgBufFor = -1, bgBufW = 0, bgBufH = 0;

int themeUiMode(int th) {
  static const int m[THEME_N] = {0, 0, 1, 2, 4, 5, 6, 3, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
  if (th < 0 || th >= THEME_N) th = 0;
  return m[th];
}
// 3.0.0: ekranda gosterilen tema (2./3. sayfada o sayfanin temasi)
int activeTheme() { return (pageThemeOverride >= 0 && pageThemeOverride < THEME_N) ? pageThemeOverride : homeTheme; }
int homeScreenIdx() { return (teacherStep > 0 || activeTheme() == 0) ? 0 : 1; }   // Rehber akisi ekran 0'da
bool isHomeScreen() { return currentScreen == 0 || currentScreen == 1; }

const char* themeName(int t) {
  switch (t) {
    case 1: return L("Klasik", "Classic");
    case 2: return L("Büyük", "Big");
    case 3: return L("Takvim", "Calendar");
    case 4: return L("Foto", "Photo");
    case 5: return L("Halka", "Ring");
    case 6: return L("Kart", "Card");
    case 7: return L("Dikey", "Portrait");
    case 8: return L("Saat", "Clock");
    case 9: return L("Dalga", "Wave");
    case 10: return L("Minimal", "Minimal");
    case 11: return L("Termometre", "Thermometer");
    case 12: return L("Panel", "Dashboard");
    case 13: return L("Nabız", "Pulse");
    case 14: return L("Yıldızlar", "Stars");
    case 15: return L("Balon", "Balloon");
    case 16: return L("Uzay", "Space");
    case 17: return L("Akvaryum", "Aquarium");
    case 18: return L("Gülen yüz", "Smiley");
    default: return L("Ana", "Main");
  }
}
const char* bgName(int b) {
  switch (b) {
    case 1: return L("Gece mavisi", "Midnight");
    case 2: return L("Antrasit", "Graphite");
    case 3: return L("Orman", "Forest");
    case 4: return L("Mürdüm", "Plum");
    case 5: return L("Karbon", "Carbon");
    case 6: return L("Nokta", "Dots");
    case 7: return L("Degrade", "Gradient");
    case 8: return L("Çizgi", "Lines");
    case 9: return L("Krem", "Cream");
    case 10: return L("Buz mavisi", "Ice blue");
    case 11: return L("Pembe", "Pink");
    case 12: return L("Nane", "Mint");
    case 13: return L("Kareli", "Grid");
    case 14: return L("Lavanta", "Lavender");
    default: return L("Siyah", "Black");
  }
}

static inline uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

// Tum arka planlar koyu: seker renkleri (kirmizi/turuncu/sari/yesil/camgobegi) okunakli kalir.
uint16_t bgPixelH(int b, int x, int y, int h) {
  switch (b) {
    case 1: return rgb565(8, 16, 44);
    case 2: return rgb565(24, 26, 30);
    case 3: return rgb565(6, 30, 18);
    case 4: return rgb565(36, 10, 34);
    case 5: {                                    // karbon dokuma
      bool a = ((x / 4 + y / 4) & 1);
      int v = a ? 16 + (x % 4) * 3 : 8 + (y % 4) * 3;
      return rgb565(v, v, v + 3);
    }
    case 6: {                                    // nokta
      int mx = x % 16, my = y % 16;
      if ((mx == 7 || mx == 8) && (my == 7 || my == 8)) return rgb565(44, 52, 72);
      return rgb565(6, 8, 14);
    }
    case 7: {                                    // lacivertten siyaha degrade
      int t = 255 - (constrain(y, 0, h - 1) * 255) / (h - 1);
      return rgb565(12 * t / 255, 34 * t / 255, 78 * t / 255);
    }
    case 8:                                      // capraz ince cizgi
      return ((x + y) % 14 == 0) ? rgb565(34, 40, 54) : rgb565(8, 8, 12);
    case 9:  return rgb565(250, 243, 226);          // acik renkler (sadece ana ekran, uygun temalarda)
    case 10: return rgb565(222, 238, 250);
    case 11: return rgb565(252, 228, 236);
    case 12: return rgb565(222, 246, 232);
    case 13: {                                   // kareli: ince izgara
      int mx = x % 20, my = y % 20;
      if (mx == 0 || my == 0) return rgb565(30, 38, 54);
      return rgb565(10, 12, 20);
    }
    case 14: return rgb565(236, 230, 250);
    default: return 0x0000;
  }
}
uint16_t bgPixel(int b, int x, int y) { return bgPixelH(b, x, y, 240); }

// Ekran boyutunda (donuse gore 320x240 veya 240x320) PSRAM tamponu
bool frameSpriteFor(int w, int h) {
  if (homeSprOk && sprW == w && sprH == h) return true;
  if (homeSprFail) return false;
  if (homeSprOk) { homeSpr.deleteSprite(); homeSprOk = false; }
  homeSpr.setPsram(true);
  homeSpr.setColorDepth(16);
  homeSprOk = (homeSpr.createSprite(w, h) != nullptr);
  if (homeSprOk) { sprW = w; sprH = h; } else homeSprFail = true;
  return homeSprOk;
}

bool bgIsLight(int b) { return (b >= 9 && b <= 12) || b == 14; }
// acik arka plani destekleyen temalar (metin rengi arka plana gore degisir)
bool themeLightOK(int th) { return (th >= 5 && th <= 13); }
// kendi sahnesini cizen temalar (arka plan secimi kullanilmaz)
bool themeOwnScene(int th) { return th == 4 || th >= 15; }
int themeCategory(int th) { if (th == 8 || th == 9 || th == 13 || th >= 14) return 1; return 0; }
// 3.0.0: listeden kaldirilan temalar (Dikey, Minimal, Termometre)
bool themeHidden(int th) { return th == 7 || th == 10 || th == 11; }
bool themeAnimatedId(int th) { return th == 9 || th == 13 || th == 14 || th == 15 || th == 16 || th == 17 || th == 8; }
bool themeAnimated() { return themeAnimatedId(activeTheme()); }
unsigned long themeAnimPeriod() { return activeTheme() == 8 ? 1000UL : 90UL; }

// Bu ekranda gecerli arka plan: acik renkler sadece uygun temanin ana ekraninda
int currentBg() {
  bool prev = (currentScreen == SCR_THEME && themePreview);
  int b = prev ? bgSel : homeBg;
  int th = prev ? thSel : activeTheme();
  if (bgIsLight(b)) {
    bool homeLike = (isHomeScreen() || prev) && !formId && !textPageActive && !timePickerActive && !ageWarnActive && teacherStep == 0;
    if (!(homeLike && themeLightOK(th))) {
      int d = prev ? thDarkSel : homeBgDark;        // uygulanamayan yerde son secilen koyu arka plan
      return bgIsLight(d) ? 0 : d;
    }
  }
  return b;
}

// Tampondaki anahtar renkli (bos) pikselleri secili arka planla degistir.
// p: w genisliginde, 'rows' satir; yoff: bu satirlarin ekrandaki baslangic satiri.
void composeBuf(uint16_t* p, int w, int rows, int yoff, int fullH, int b) {
  if (!p) return;
  const uint16_t key = (uint16_t)((HOME_KEY >> 8) | ((HOME_KEY & 0xFF) << 8));   // sprite bayt sirasi
  const int n = w * rows;
  if (b <= 4) {
    uint16_t c = bgPixel(b, 0, 0);
    uint16_t cs = (uint16_t)((c >> 8) | ((c & 0xFF) << 8));
    for (int i = 0; i < n; i++) if (p[i] == key) p[i] = cs;
    return;
  }
  if (!bgBuf) bgBuf = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
  if (!bgBuf) { for (int i = 0; i < n; i++) if (p[i] == key) p[i] = 0; return; }
  if (bgBufFor != b || bgBufW != w || bgBufH != fullH) {
    for (int y = 0; y < fullH; y++)
      for (int x = 0; x < w; x++) {
        uint16_t c = bgPixelH(b, x, y, fullH);
        bgBuf[y * w + x] = (uint16_t)((c >> 8) | ((c & 0xFF) << 8));
      }
    bgBufFor = b; bgBufW = w; bgBufH = fullH;
  }
  const uint16_t* src = bgBuf + yoff * w;
  for (int i = 0; i < n; i++) if (p[i] == key) p[i] = src[i];
}

// Liste / form / metin govdesi (listSpr) cizilirken arka plan acik mi
bool bgFrameOn() { return homeSprOk && !frameActive && !screenDirect(); }

// ---------------------------------------------------------------- ust durum cubugu
void drawWifiGlyph(lgfx::LovyanGFX* g, int x, int y) {
  bool on = (WiFi.status() == WL_CONNECTED);
  int bars = 0;
  if (on) {
    int rssi = WiFi.RSSI();
    bars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
  }
  for (int b = 0; b < 4; b++) {
    int h = 3 + b * 3;
    g->fillRect(x + b * 4, y + 12 - h, 3, h, (b < bars) ? TXT() : (lightBg ? (uint16_t)0xBDF7 : (uint16_t)0x4208));
  }
  if (!on || netNoInternet) {                    // 3.4.8: internet yoksa da carpi
    uint16_t xc = sevColor(0);
    g->drawLine(x, y + 1, x + 14, y + 12, xc); g->drawLine(x + 1, y + 1, x + 15, y + 12, xc);
    g->drawLine(x + 14, y + 1, x, y + 12, xc); g->drawLine(x + 15, y + 1, x + 1, y + 12, xc);
  }
}

void drawBatteryGlyph(lgfx::LovyanGFX* g, int x, int y) {
  if (!inaPresent || batteryPercent < 0) return;
  uint16_t c = batteryCharging ? (uint16_t)TFT_CYAN
             : (batteryPercent <= 20 ? sevColor(0) : (batteryPercent <= 30 ? sevColor(3) : TXT()));
  g->drawRoundRect(x, y, 22, 12, 3, 0xA514);
  g->fillRect(x + 22, y + 4, 2, 4, 0xA514);
  int w = 18 * batteryPercent / 100;
  if (w > 0) g->fillRect(x + 2, y + 2, w, 8, c);
  if (batteryCharging) {                                    // 3.4.13: sarjda simsek hep sari, ustte (koyu kenarli)
    int cx = x + 11;
    const int ox[4] = {-1, 1, 0, 0}, oy[4] = {0, 0, -1, 1};
    for (int k = 0; k < 4; k++) {
      g->fillTriangle(cx + 3 + ox[k], y + 1 + oy[k], cx - 4 + ox[k], y + 7 + oy[k], cx + 1 + ox[k], y + 7 + oy[k], TFT_BLACK);
      g->fillTriangle(cx - 3 + ox[k], y + 11 + oy[k], cx + 4 + ox[k], y + 5 + oy[k], cx - 1 + ox[k], y + 5 + oy[k], TFT_BLACK);
    }
    g->fillTriangle(cx + 3, y + 1, cx - 4, y + 7, cx + 1, y + 7, TFT_YELLOW);
    g->fillTriangle(cx - 3, y + 11, cx + 4, y + 5, cx - 1, y + 5, TFT_YELLOW);
  }
  g->setFont(&lgfx::fonts::Font0);
  g->setTextColor(lightBg ? (uint16_t)0x4208 : (uint16_t)0xD69A);
  g->setTextDatum(MR_DATUM);
  g->drawString((String(batteryPercent) + "%").c_str(), x - 3, y + 6);
}

String clockStr() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) return "--:--";
  time_t local = tv.tv_sec + 3 * 3600;
  struct tm* tm = gmtime(&local);
  char b[8]; sprintf(b, "%02d:%02d", tm->tm_hour, tm->tm_min);
  return String(b);
}

// y = 0..20 arasi; showGlucose: ortada kucuk seker + ok
bool statusBarDrawn = false;
// Kucuk "ses kapali" simgesi (hoparlor + carpi), sol ust kose (x, y), ~16x12
void drawMuteGlyph(lgfx::LovyanGFX* g, int x, int y, uint16_t c) {
  g->fillRect(x, y + 4, 3, 5, c);
  g->fillTriangle(x + 3, y + 4, x + 7, y, x + 7, y + 12, c);
  g->fillRect(x + 3, y + 4, 4, 5, c);
  g->drawLine(x + 10, y + 3, x + 15, y + 9, c); g->drawLine(x + 11, y + 3, x + 16, y + 9, c);
  g->drawLine(x + 15, y + 3, x + 10, y + 9, c); g->drawLine(x + 16, y + 3, x + 11, y + 9, c);
}
uint16_t muteGlyphColor() { return (effectiveSoundLevel() == 0) ? (uint16_t)0xFD20 : (uint16_t)0xFE60; }

void drawStatusBar(bool showGlucose) {
  statusBarDrawn = true;
  lgfx::LovyanGFX* g = cv;
  int W = g->width();
  g->setFont(&lgfx::fonts::Font2);
  g->setTextColor(TXT());
  g->setTextDatum(ML_DATUM);
  g->drawString(clockStr().c_str(), 8, 11);
  if (soundWarnText().length()) drawMuteGlyph(g, 8 + g->textWidth(clockStr().c_str()) + 8, 5, muteGlyphColor());
  if (showGlucose && dataValid) {
    uint16_t c = GC(sgv);
    String s = formatGlucose(sgv);
    int w = g->textWidth(s.c_str());
    int x0 = W / 2 - (w + 16) / 2;
    g->setTextColor(c);
    g->drawString(s.c_str(), x0, 11);
    drawTrendArrow(x0 + w + 9, 11, direction, c, 10);
  }
  bool batShown = inaPresent && batteryPercent >= 0;
  drawWifiGlyph(g, batShown ? W - 82 : W - 22, 4);      // pil yoksa sag ust kose
  if (inaPresent && batteryPercent >= 0) drawBatteryGlyph(g, W - 30, 5);
  g->setTextDatum(TL_DATUM);
  g->setFont(&lgfx::fonts::Font0);
}

// ageText() eski ASCII yazi tipleri icin "once" yazar; Turkce yazi tipiyle "önce"
String ageTextTR() {
  String t = ageText();
  if (langIdx == 0) t.replace("once", "önce");
  return t;
}

uint16_t dimColor(uint16_t c, int pct) {
  int r = ((c >> 11) & 0x1F) * pct / 100, gg = ((c >> 5) & 0x3F) * pct / 100, b = (c & 0x1F) * pct / 100;
  return (uint16_t)((r << 11) | (gg << 5) | b);
}


// ---- acik / koyu arka plana gore renkler (2.7.0)
uint16_t blendC(uint16_t a, uint16_t b, int pctB) {
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31, br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (br - ar) * pctB / 100, g = ag + (bg - ag) * pctB / 100, bl = ab + (bb - ab) * pctB / 100;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}
uint16_t TXT()   { return lightBg ? (uint16_t)0x2104 : (uint16_t)TFT_WHITE; }
uint16_t SUBT()  { return lightBg ? (uint16_t)0x6B4D : (uint16_t)0xA514; }
uint16_t CARDC() { return lightBg ? (uint16_t)0xFFFF : (uint16_t)0x18E4; }
uint16_t FACEC() { return lightBg ? (uint16_t)0xFFFF : (uint16_t)0x10A2; }
uint16_t GC(int v) { uint16_t c = getGlucoseColor(v); return lightBg ? dimColor(c, 68) : c; }
uint16_t TRACKC(uint16_t c) { return lightBg ? blendC(c, 0xFFFF, 72) : dimColor(c, 38); }

// ---------------------------------------------------------------- HALKA temasi
void drawUiRing() {
  cv->fillScreen(UI_BG);
  bool alarmOn = isAlarmActive();
  bool visBlink = alarmOn && visualAlarmOn && ((long)(millis() - visualBurstEnd) < 0);
  if (visBlink && millis() - lastBlink > 400) { alarmBlinkState = !alarmBlinkState; lastBlink = millis(); }
  bool flash = visBlink && alarmBlinkState;
  if (flash) cv->fillScreen(getAlarmColor());
  drawStatusBar(false);

  const int cx = 160, cy = 134, r1 = 106, r0 = 86;
  // Arka iz: esik bolgelerinin koyu tonlari (40..300 mg/dL, 270 derece)
  auto angOf = [](int v) -> float { return 135.0f + (constrain(v, 40, 300) - 40) * 270.0f / 260.0f; };
  int segStart = 40;
  uint16_t segCol = GC(40);
  for (int v = 42; v <= 300; v += 2) {
    uint16_t c = GC(v);
    if (c != segCol || v == 300) {
      cv->fillArc(cx, cy, r0, r1, angOf(segStart), angOf(v), TRACKC(segCol));
      segStart = v; segCol = c;
    }
  }
  if (dataValid) {
    int v = constrain(sgv, 40, 300);
    // Dolu kisim: bolge renkleriyle
    segStart = 40; segCol = GC(40);
    for (int vv = 42; vv <= v; vv += 2) {
      uint16_t c = GC(vv);
      if (c != segCol || vv + 2 > v) {
        cv->fillArc(cx, cy, r0, r1, angOf(segStart), angOf(vv), segCol);
        segStart = vv; segCol = c;
      }
    }
    float rad = angOf(v) * DEG_TO_RAD;
    int mx = cx + (int)(cosf(rad) * (r0 + r1) / 2), my = cy + (int)(sinf(rad) * (r0 + r1) / 2);
    uint16_t gc = GC(sgv);
    cv->fillCircle(mx, my, 15, TXT());
    cv->fillCircle(mx, my, 10, gc);

    uint16_t tc = flash ? (uint16_t)TFT_BLACK : gc;
    String gs = formatGlucose(sgv);
    cv->setTextColor(tc);
    cv->setTextDatum(MC_DATUM);
    cv->setFont(&lgfx::fonts::Font7);
    cv->setTextSize(unitIdx == 1 ? 1.25f : 1.42f);
    cv->drawString(gs.c_str(), cx, cy - 14);
    cv->setTextSize(1);
    cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
    cv->setFont(&FreeSansTR7pt);
    cv->drawString(unitLabel().c_str(), cx, cy + 30);
    // delta + ok
    String ds = formatDelta(delta);
    cv->setFont(&lgfx::fonts::Font4);
    int dw = cv->textWidth(ds.c_str());
    int gx = cx - (dw + 10 + 26) / 2;
    cv->setTextColor(tc);
    cv->setTextDatum(ML_DATUM);
    cv->drawString(ds.c_str(), gx, cy + 58);
    drawTrendArrow(gx + dw + 10 + 13, cy + 58, direction, tc, 26);
  } else {
    cv->setTextColor(sevColor(1));
    cv->setTextDatum(MC_DATUM);
    cv->setFont(&FreeSansTR9pt);
    cv->drawString(L("Veri bekleniyor", "Waiting for data"), cx, cy);
  }
  // alt boslukta son olcum suresi
  bool stale = dataValid && staleAlarmMin > 0 && readingAgeSec > (unsigned long)staleAlarmMin * 60UL;
  cv->setTextColor(flash ? (uint16_t)TFT_BLACK : (stale ? sevColor(0) : SUBT()));
  cv->setTextDatum(MC_DATUM);
  cv->setFont(&FreeSansTR7pt);
  cv->drawString(ageTextTR().c_str(), cx, 230);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- KART temasi
// ---------------------------------------------------------------- KART temasi
void drawMiniHistory(int sx, int gy, int gw, int gh, int hours) {
  uint32_t nowSec = (uint32_t)time(nullptr);
  uint32_t cut = (nowSec > (uint32_t)hours * 3600UL) ? nowSec - (uint32_t)hours * 3600UL : 0;
  static int idx[HIST_MAX];
  int cnt = 0;
  for (int i = 0; i < historyCount; i++)
    if (historyTsSec[i] == 0 || historyTsSec[i] >= cut) { if (cnt < HIST_MAX) idx[cnt++] = i; }
  auto yOf = [&](int v) -> int { return constrain(gy + gh - ((constrain(v, 40, 300) - 40) * gh) / 260, gy, gy + gh); };
  int yHi = yOf(alarmHigh), yLo = yOf(alarmLow);
  if (yLo > yHi) cv->fillRect(sx, yHi, gw, yLo - yHi, (lightBg ? (uint16_t)0xCF79 : (uint16_t)0x0200));
  cv->drawFastHLine(sx, yHi, gw, (lightBg ? (uint16_t)0x9EF3 : (uint16_t)0x0320));
  cv->drawFastHLine(sx, yLo, gw, (lightBg ? (uint16_t)0x9EF3 : (uint16_t)0x0320));
  if (cnt > 1) {
    for (int i = 0; i < cnt - 1; i++) {
      int v1 = history[idx[i]], v2 = history[idx[i + 1]];
      int x1 = sx + (i * gw) / (cnt - 1), x2 = sx + ((i + 1) * gw) / (cnt - 1);
      uint16_t c = GC((v1 + v2) / 2);
      cv->drawLine(x1, yOf(v1), x2, yOf(v2), c);
      cv->drawLine(x1, yOf(v1) + 1, x2, yOf(v2) + 1, c);
    }
    int lx = sx + gw, ly = yOf(history[idx[cnt - 1]]);
    cv->fillCircle(lx, ly, 3, GC(history[idx[cnt - 1]]));
  }
}

void drawUiCardTheme() {
  cv->fillScreen(UI_BG);
  bool alarmOn = isAlarmActive();
  bool visBlink = alarmOn && visualAlarmOn && ((long)(millis() - visualBurstEnd) < 0);
  if (visBlink && millis() - lastBlink > 400) { alarmBlinkState = !alarmBlinkState; lastBlink = millis(); }
  bool flash = visBlink && alarmBlinkState;
  drawStatusBar(false);

  const uint16_t CARD = CARDC();
  uint16_t gc = dataValid ? GC(sgv) : (uint16_t)0x8410;
  uint16_t cardFill = flash ? getAlarmColor() : CARD;
  cv->fillRoundRect(8, 26, 304, 124, 16, cardFill);
  cv->fillRoundRect(13, 40, 6, 96, 3, gc);
  if (dataValid) {
    uint16_t tc = flash ? (uint16_t)TFT_BLACK : gc;
    String gs = formatGlucose(sgv);
    cv->setFont(&lgfx::fonts::Font7);
    cv->setTextSize(unitIdx == 1 ? 1.6f : 1.8f);
    cv->setTextColor(tc);
    cv->setTextDatum(ML_DATUM);
    cv->drawString(gs.c_str(), 26, 84);
    cv->setTextSize(1);
    cv->setFont(&FreeSansTR6pt);
    cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
    cv->setTextDatum(BL_DATUM);
    cv->drawString((unitLabel() + "   " + ageTextTR()).c_str(), 28, 144);
    drawTrendArrow(268, 62, direction, tc, 30);
    String ds = formatDelta(delta);
    cv->setFont(&lgfx::fonts::Font4);
    cv->setTextColor(tc);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(ds.c_str(), 268, 112);
  } else {
    cv->setFont(&FreeSansTR9pt);
    cv->setTextColor(sevColor(1));
    cv->setTextDatum(MC_DATUM);
    cv->drawString(L("Veri bekleniyor", "Waiting for data"), 160, 88);
  }
  // 3 saatlik mini grafik karti
  cv->fillRoundRect(8, 156, 304, 80, 14, CARD);
  cv->setFont(&FreeSansTR6pt);
  cv->setTextColor(SUBT());
  cv->setTextDatum(TL_DATUM);
  cv->drawString(L("Son 3 saat", "Last 3 hours"), 18, 160);
  // son veri gostergesi (5 dakika noktasi)
  int bars = (int)(readingAgeSec / 60UL);
  if (bars > 5) bars = 5;
  for (int i = 0; i < 5; i++) {
    uint16_t c = ageBarColor(readingAgeSec, i < bars, lightBg ? (uint16_t)0xC618 : (uint16_t)0x3186);
    cv->fillRoundRect(236 + i * 14, 162, 10, 8, 2, c);
  }
  drawMiniHistory(18, 176, 280, 52, 3);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- ana ekran ust katman
// Ana (0) disindaki temalarda alarm varken Ertele / Dikkat ust koselerde
// ---------------------------------------------------------------- ortak: yanip sonme durumu
bool themeFlash() {
  bool alarmOn = isAlarmActive();
  bool visBlink = alarmOn && visualAlarmOn && ((long)(millis() - visualBurstEnd) < 0);
  if (visBlink && millis() - lastBlink > 400) { alarmBlinkState = !alarmBlinkState; lastBlink = millis(); }
  return visBlink && alarmBlinkState;
}

// ---------------------------------------------------------------- DIKEY tema (240x320)
void drawUiVertical() {
  const int W = 240;
  cv->fillScreen(UI_BG);
  bool flash = themeFlash();
  if (flash) cv->fillScreen(getAlarmColor());
  drawStatusBar(false);
  if (dataValid) {
    uint16_t gc = GC(sgv);
    uint16_t tc = flash ? (uint16_t)TFT_BLACK : gc;
    String gs = formatGlucose(sgv);
    cv->setFont(&lgfx::fonts::Font7);
    cv->setTextSize(unitIdx == 1 ? 1.35f : 1.6f);
    cv->setTextColor(tc);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(gs.c_str(), W / 2, 100);
    cv->setTextSize(1);
    cv->setFont(&FreeSansTR7pt);
    cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
    cv->drawString(unitLabel().c_str(), W / 2, 152);
    String ds = formatDelta(delta);
    cv->setFont(&lgfx::fonts::Font4);
    int dw = cv->textWidth(ds.c_str());
    int gx = W / 2 - (dw + 12 + 30) / 2;
    cv->setTextColor(tc);
    cv->setTextDatum(ML_DATUM);
    cv->drawString(ds.c_str(), gx, 186);
    drawTrendArrow(gx + dw + 12 + 15, 186, direction, tc, 30);
  } else {
    cv->setFont(&FreeSansTR9pt);
    cv->setTextColor(sevColor(1));
    cv->setTextDatum(MC_DATUM);
    cv->drawString(L("Veri bekleniyor", "Waiting for data"), W / 2, 120);
  }
  cv->fillRoundRect(8, 214, W - 16, 74, 14, CARDC());
  cv->setFont(&FreeSansTR6pt);
  cv->setTextColor(SUBT());
  cv->setTextDatum(TL_DATUM);
  cv->drawString(L("Son 3 saat", "Last 3 hours"), 18, 218);
  drawMiniHistory(18, 234, W - 36, 46, 3);
  bool stale = dataValid && staleAlarmMin > 0 && readingAgeSec > (unsigned long)staleAlarmMin * 60UL;
  cv->setFont(&FreeSansTR7pt);
  cv->setTextColor(flash ? (uint16_t)TFT_BLACK : (stale ? sevColor(0) : SUBT()));
  cv->setTextDatum(MC_DATUM);
  cv->drawString(ageTextTR().c_str(), W / 2, 304);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- SAAT (analog) temasi
void drawUiClock() {
  cv->fillScreen(UI_BG);
  bool flash = themeFlash();
  if (flash) cv->fillScreen(getAlarmColor());
  const int cx = 108, cy = 124, R = 102;
  cv->fillCircle(cx, cy, R, flash ? (uint16_t)TFT_BLACK : FACEC());
  cv->drawCircle(cx, cy, R, 0x6B4D);
  cv->drawCircle(cx, cy, R - 1, 0x6B4D);
  for (int i = 0; i < 60; i++) {
    float a = (i * 6 - 90) * DEG_TO_RAD;
    int r1 = (i % 5 == 0) ? R - 14 : R - 7;
    int x0 = cx + (int)(cosf(a) * (R - 4)), y0 = cy + (int)(sinf(a) * (R - 4));
    int x1 = cx + (int)(cosf(a) * r1), y1 = cy + (int)(sinf(a) * r1);
    if (i % 5 == 0) cv->drawWideLine(x0, y0, x1, y1, 2, TXT());
    else cv->drawLine(x0, y0, x1, y1, 0x8410);
  }
  cv->setFont(&FreeSansTR9pt);
  cv->setTextColor(SUBT());
  cv->setTextDatum(MC_DATUM);
  cv->drawString("12", cx, cy - R + 28);
  cv->drawString("3", cx + R - 26, cy + 1);
  cv->drawString("6", cx, cy + R - 26);
  cv->drawString("9", cx - R + 26, cy + 1);
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (tv.tv_sec > 1600000000) {
    time_t local = tv.tv_sec + 3 * 3600;
    struct tm* tm = gmtime(&local);
    float hA = ((tm->tm_hour % 12) * 30 + tm->tm_min * 0.5f - 90) * DEG_TO_RAD;
    float mA = (tm->tm_min * 6 + tm->tm_sec * 0.1f - 90) * DEG_TO_RAD;
    float sA = (tm->tm_sec * 6 - 90) * DEG_TO_RAD;
    cv->drawWideLine(cx, cy, cx + (int)(cosf(hA) * 52), cy + (int)(sinf(hA) * 52), 5, TXT());
    cv->drawWideLine(cx, cy, cx + (int)(cosf(mA) * 78), cy + (int)(sinf(mA) * 78), 3, TXT());
    uint16_t sc = dataValid ? GC(sgv) : (uint16_t)0xF800;
    cv->drawLine(cx - (int)(cosf(sA) * 16), cy - (int)(sinf(sA) * 16), cx + (int)(cosf(sA) * 86), cy + (int)(sinf(sA) * 86), sc);
    cv->fillCircle(cx, cy, 6, sc);
    cv->fillCircle(cx, cy, 2, TFT_BLACK);
    // tarih (sag ust)
    char db[20];
    snprintf(db, sizeof(db), "%d %s", tm->tm_mday, monthShortName(tm->tm_mon + 1));
    cv->setFont(&FreeSansTR7pt);
    cv->setTextColor(SUBT());
    cv->setTextDatum(MC_DATUM);
    cv->drawString(db, 264, 30);
  }
  if (inaPresent && batteryPercent >= 0) drawBatteryGlyph(cv, 290, 5);
  // sag panel: seker
  if (dataValid) {
    uint16_t gc = GC(sgv);
    uint16_t tc = flash ? (uint16_t)TFT_BLACK : gc;
    cv->fillRoundRect(216, 50, 98, 150, 14, flash ? getAlarmColor() : (uint16_t)CARDC());
    cv->fillRoundRect(222, 58, 86, 5, 2, gc);
    String gs = formatGlucose(sgv);
    cv->setFont(&lgfx::fonts::Font7);
    cv->setTextSize(unitIdx == 1 ? 0.75f : 0.85f);
    cv->setTextColor(tc);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(gs.c_str(), 265, 100);
    cv->setTextSize(1);
    cv->setFont(&FreeSansTR6pt);
    cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
    cv->drawString(unitLabel().c_str(), 265, 130);
    drawTrendArrow(245, 160, direction, tc, 22);
    cv->setFont(&lgfx::fonts::Font4);
    cv->setTextColor(tc);
    cv->drawString(formatDelta(delta).c_str(), 284, 160);
    cv->setFont(&FreeSansTR6pt);
    cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
    cv->drawString(ageTextTR().c_str(), 265, 188);
  } else {
    cv->setFont(&FreeSansTR7pt);
    cv->setTextColor(sevColor(1));
    cv->setTextDatum(MC_DATUM);
    cv->drawString(L("Veri yok", "No data"), 265, 124);
  }
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- DALGA (animasyonlu) temasi
void drawUiWave() {
  cv->fillScreen(UI_BG);
  bool flash = themeFlash();
  if (flash) cv->fillScreen(getAlarmColor());
  uint16_t gc = dataValid ? GC(sgv) : (uint16_t)0x4208;
  float ph = millis() / 14.0f;
  uint16_t c1 = TRACKC(gc), c2 = lightBg ? blendC(gc, 0xFFFF, 45) : dimColor(gc, 55);
  for (int x = 0; x < 320; x += 2) {
    int y1 = 178 + (int)(9 * sinf((x + ph) / 30.0f) + 5 * sinf((x * 1.6f - ph * 1.3f) / 21.0f));
    int y2 = 194 + (int)(7 * sinf((x - ph * 0.8f) / 24.0f) + 3 * sinf((x + ph * 2.0f) / 13.0f));
    cv->fillRect(x, y1, 2, 240 - y1, c1);
    cv->fillRect(x, y2, 2, 240 - y2, c2);
  }
  // yukselen kabarciklar
  for (int i = 0; i < 6; i++) {
    int period = 3000 + i * 700;
    int t = (millis() + i * 911) % period;
    int bx = (i * 53 + 20) % 300 + 10 + (int)(4 * sinf((millis() + i * 300) / 400.0f));
    int by = 236 - t * 60 / period;
    cv->drawCircle(bx, by, 2 + (i % 3), dimColor(gc, 85));
  }
  drawStatusBar(false);
  if (dataValid) {
    uint16_t tc = flash ? (uint16_t)TFT_BLACK : gc;
    String gs = formatGlucose(sgv);
    cv->setFont(&lgfx::fonts::Font7);
    cv->setTextSize(unitIdx == 1 ? 1.4f : 1.7f);
    cv->setTextColor(tc);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(gs.c_str(), 128, 92);
    cv->setTextSize(1);
    drawTrendArrow(276, 72, direction, tc, 30);
    cv->setFont(&lgfx::fonts::Font4);
    cv->setTextColor(tc);
    cv->drawString(formatDelta(delta).c_str(), 276, 120);
    cv->setFont(&FreeSansTR7pt);
    cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
    cv->drawString(unitLabel().c_str(), 128, 150);
  } else {
    cv->setFont(&FreeSansTR9pt);
    cv->setTextColor(sevColor(1));
    cv->setTextDatum(MC_DATUM);
    cv->drawString(L("Veri bekleniyor", "Waiting for data"), 160, 100);
  }
  cv->setFont(&FreeSansTR7pt);
  cv->setTextColor(TXT());
  cv->setTextDatum(MC_DATUM);
  cv->drawString(ageTextTR().c_str(), 160, 226);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}


// =====================================================================
//  2.7.0 - YENI TEMALAR: Minimal, Termometre, Panel, Nabiz (anim), Yildizlar (anim)
//  Cocuk: Balon, Uzay, Akvaryum (anim), Gulen yuz
// =====================================================================
// ortak: deger + ok + degisim satiri (ortalanmis)
void drawValueBlock(int cx, int cy, float scale, uint16_t tc, bool withUnit) {
  String gs = formatGlucose(sgv);
  cv->setFont(&lgfx::fonts::Font7);
  cv->setTextSize(unitIdx == 1 ? scale * 0.85f : scale);
  cv->setTextColor(tc);
  cv->setTextDatum(MC_DATUM);
  cv->drawString(gs.c_str(), cx, cy);
  cv->setTextSize(1);
  if (withUnit) {
    cv->setFont(&FreeSansTR6pt);
    cv->setTextColor(SUBT());
    cv->drawString(unitLabel().c_str(), cx, cy + (int)(28 * scale) + 6);
  }
}
void drawDeltaRow(int cx, int cy, uint16_t tc, int arrow) {
  String ds = formatDelta(delta);
  cv->setFont(&lgfx::fonts::Font4);
  int dw = cv->textWidth(ds.c_str());
  int gx = cx - (dw + 10 + arrow) / 2;
  cv->setTextColor(tc);
  cv->setTextDatum(ML_DATUM);
  cv->drawString(ds.c_str(), gx, cy);
  drawTrendArrow(gx + dw + 10 + arrow / 2, cy, direction, tc, arrow);
}
int kidY(int v) { return 196 - (constrain(v, 40, 300) - 40) * 160 / 260; }
String kidMsg(const char* subj) {
  // Sadece hedefteyken ovgu; dusuk/yuksekte yonlendirme yok (cocuk yanlis anlamasin)
  if (!dataValid || homeDataStale() || sgv < alarmLow || sgv > alarmHigh) return "";
  return String(subj) + L(" hedefte, harika!", " is on target, great!");
}
void kidCard(uint16_t cardC, uint16_t txtC, bool darkText) {
  cv->fillRoundRect(206, 8, 108, 78, 14, cardC);
  if (!dataValid) return;
  uint16_t gc = darkText ? dimColor(getGlucoseColor(sgv), 70) : getGlucoseColor(sgv);
  String gs = formatGlucose(sgv);
  cv->setFont(&lgfx::fonts::Font7);
  cv->setTextSize(unitIdx == 1 ? 0.7f : 0.85f);
  cv->setTextColor(gc);
  cv->setTextDatum(MC_DATUM);
  cv->drawString(gs.c_str(), 260, 36);
  cv->setTextSize(1);
  drawTrendArrow(236, 70, direction, gc, 16);
  cv->setFont(&lgfx::fonts::Font2);
  cv->drawString(formatDelta(delta).c_str(), 282, 70);
  (void)txtC;
}
void kidMessage(const char* subj, uint16_t col, int y) {
  cv->setFont(&FreeSansTR7pt);
  cv->setTextColor(col);
  cv->setTextDatum(MC_DATUM);
  cv->drawString(kidMsg(subj).c_str(), 160, y);
}
static uint32_t hashU(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }

// ---------------------------------------------------------------- MINIMAL
void drawUiMinimal() {
  cv->fillScreen(UI_BG);
  bool flash = themeFlash();
  if (flash) cv->fillScreen(getAlarmColor());
  cv->setFont(&FreeSansTR7pt);
  cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
  cv->setTextDatum(MC_DATUM);
  cv->drawString(clockStr().c_str(), 160, 12);
  if (dataValid) {
    uint16_t tc = flash ? (uint16_t)TFT_BLACK : GC(sgv);
    drawValueBlock(160, 102, 2.25f, tc, false);
    drawDeltaRow(160, 186, tc, 26);
  } else {
    cv->setFont(&FreeSansTR9pt); cv->setTextColor(sevColor(1)); cv->drawString(L("Veri bekleniyor", "Waiting for data"), 160, 110);
  }
  cv->setFont(&FreeSansTR6pt);
  cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
  cv->setTextDatum(MC_DATUM);
  cv->drawString((unitLabel() + "  -  " + ageTextTR()).c_str(), 160, 226);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- TERMOMETRE
void drawUiThermo() {
  cv->fillScreen(UI_BG);
  bool flash = themeFlash();
  if (flash) cv->fillScreen(getAlarmColor());
  drawStatusBar(false);
  const int tx = 34, tw = 30, top = 34, bot = 190;
  auto yOf = [&](int v) { return bot - (constrain(v, 40, 300) - 40) * (bot - top) / 260; };
  int yHi = yOf(alarmHigh), yLo = yOf(alarmLow);
  cv->fillRect(tx - 12, yHi, tw + 24, yLo - yHi, lightBg ? (uint16_t)0xCF79 : (uint16_t)0x0200);
  cv->fillRoundRect(tx, top - 6, tw, bot - top + 20, 15, CARDC());
  cv->drawRoundRect(tx, top - 6, tw, bot - top + 20, 15, SUBT());
  uint16_t gc = dataValid ? GC(sgv) : SUBT();
  int bx = tx + tw / 2, by = bot + 26;
  if (dataValid) cv->fillRoundRect(tx + 7, yOf(sgv), tw - 14, by - yOf(sgv), 8, gc);
  cv->fillCircle(bx, by, 22, CARDC());
  cv->drawCircle(bx, by, 22, SUBT());
  cv->fillCircle(bx, by, 16, gc);
  cv->setFont(&FreeSansTR6pt);
  cv->setTextColor(SUBT());
  cv->setTextDatum(ML_DATUM);
  cv->drawString(formatGlucose(alarmHigh).c_str(), tx + tw + 8, yHi);
  cv->drawString(formatGlucose(alarmLow).c_str(), tx + tw + 8, yLo);
  if (dataValid) {
    uint16_t tc = flash ? (uint16_t)TFT_BLACK : gc;
    drawValueBlock(200, 92, 1.55f, tc, true);
    drawDeltaRow(200, 168, tc, 24);
  } else {
    cv->setFont(&FreeSansTR9pt); cv->setTextColor(sevColor(1)); cv->setTextDatum(MC_DATUM); cv->drawString(L("Veri yok", "No data"), 200, 110);
  }
  cv->setFont(&FreeSansTR7pt);
  cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
  cv->setTextDatum(MC_DATUM);
  cv->drawString(ageTextTR().c_str(), 200, 212);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- PANEL
void drawUiPanel() {
  cv->fillScreen(UI_BG);
  bool flash = themeFlash();
  drawStatusBar(false);
  uint16_t card = flash ? getAlarmColor() : CARDC();
  uint16_t gc = dataValid ? GC(sgv) : SUBT();
  uint16_t tc = flash ? (uint16_t)TFT_BLACK : gc;
  const char* lab[4] = {L("Şeker", "Glucose"), L("Değişim", "Change"), L("Hedefte (24 sa)", "In range (24 h)"), L("Son 3 saat", "Last 3 hours")};
  int cxs[4] = {8, 162, 8, 162}, cys[4] = {24, 24, 128, 128};
  for (int i = 0; i < 4; i++) {
    cv->fillRoundRect(cxs[i], cys[i], 150, 100, 12, i < 2 ? card : CARDC());
    if (lightBg) cv->drawRoundRect(cxs[i], cys[i], 150, 100, 12, 0xC618);
    cv->setFont(&FreeSansTR6pt);
    cv->setTextColor(i < 2 && flash ? (uint16_t)TFT_BLACK : SUBT());
    cv->setTextDatum(TL_DATUM);
    cv->drawString(lab[i], cxs[i] + 10, cys[i] + 6);
  }
  if (dataValid) {
    drawValueBlock(83, 74, 1.0f, tc, false);
    cv->setFont(&FreeSansTR6pt); cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT()); cv->setTextDatum(MC_DATUM);
    cv->drawString(unitLabel().c_str(), 83, 112);
    drawDeltaRow(237, 70, tc, 22);
    cv->setFont(&FreeSansTR6pt); cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT()); cv->setTextDatum(MC_DATUM);
    cv->drawString(ageTextTR().c_str(), 237, 108);
  }
  int minV = 999, maxV = 0, avgV = 0, p1 = 0, p2 = 0, used = 0; uint32_t t0 = 0, t1 = 0;
  calcLast24hStats(minV, maxV, avgV, p1, p2, used, t0, t1);
  cv->setTextDatum(MC_DATUM);
  if (used > 0 && !statsEnough(used, t0, t1)) {
    cv->setFont(&FreeSansTR7pt); cv->setTextColor(SUBT()); cv->drawString("-", 83, 170);
    cv->setFont(&FreeSansTR6pt);
    cv->drawString(L("veri az", "not enough data"), 83, 200);
  } else if (used > 0) {
    char b[12]; sprintf(b, "%%%d", p1);
    cv->setFont(&lgfx::fonts::Font4);
    cv->setTextSize(1.4f);
    cv->setTextColor(p1 >= 70 ? GC(120) : GC(alarmHigh + 1));
    cv->drawString(b, 83, 176);
    cv->setTextSize(1);
    cv->setFont(&FreeSansTR6pt);
    cv->setTextColor(SUBT());
    String mm = String(L("en az ", "min ")) + formatGlucose(minV) + L("  en çok ", "  max ") + formatGlucose(maxV);
    cv->drawString(mm.c_str(), 83, 212);
  } else {
    cv->setFont(&FreeSansTR7pt); cv->setTextColor(SUBT()); cv->drawString("-", 83, 176);
  }
  drawMiniHistory(172, 152, 128, 66, 3);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- NABIZ (animasyonlu)
void drawUiPulse() {
  cv->fillScreen(UI_BG);
  bool flash = themeFlash();
  if (flash) cv->fillScreen(getAlarmColor());
  drawStatusBar(false);
  const int cx = 160, cy = 128;
  uint16_t gc = dataValid ? GC(sgv) : SUBT();
  float t = (millis() % 1800) / 1800.0f;
  for (int k = 0; k < 3; k++) {
    float f = fmodf(t + k / 3.0f, 1.0f);
    int r = 72 + (int)(f * 60);
    uint16_t c = lightBg ? blendC(gc, 0xFFFF, 35 + (int)(f * 60)) : dimColor(gc, (int)((1 - f) * 75));
    for (int w = 0; w < 3; w++) cv->drawCircle(cx, cy, r + w, c);
  }
  int pr = 70 + (int)(3 * sinf(t * 2 * PI));
  cv->fillCircle(cx, cy, pr, CARDC());
  for (int w = 0; w < 3; w++) cv->drawCircle(cx, cy, pr - w, gc);
  if (dataValid) {
    uint16_t tc = flash ? (uint16_t)TFT_BLACK : gc;
    drawValueBlock(cx, cy - 14, 1.2f, tc, false);
    drawDeltaRow(cx, cy + 36, tc, 20);
  } else {
    cv->setFont(&FreeSansTR7pt); cv->setTextColor(sevColor(1)); cv->setTextDatum(MC_DATUM); cv->drawString(L("Veri yok", "No data"), cx, cy);
  }
  cv->setFont(&FreeSansTR7pt);
  cv->setTextColor(flash ? (uint16_t)TFT_BLACK : SUBT());
  cv->setTextDatum(MC_DATUM);
  cv->drawString(ageTextTR().c_str(), cx, 228);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- YILDIZLAR (animasyonlu)
void drawUiStars() {
  cv->fillScreen(UI_BG);
  bool flash = themeFlash();
  if (flash) cv->fillScreen(getAlarmColor());
  unsigned long ms = millis();
  for (int i = 0; i < 60; i++) {
    uint32_t h = hashU(i * 2654435761UL + 7);
    int sp = 1 + (h % 3);
    int x = (int)(((h >> 8) % 320) - (ms * sp / 70) % 320);
    if (x < 0) x += 320;
    int y = (h >> 16) % 240;
    int br = 110 + (int)(100 * sinf(ms / 350.0f + i));
    uint16_t c = rgb565(br, br, min(255, br + 30));
    if (sp == 3) cv->fillRect(x, y, 2, 2, c); else cv->drawPixel(x, y, c);
  }
  drawStatusBar(false);
  if (dataValid) {
    uint16_t tc = flash ? (uint16_t)TFT_BLACK : getGlucoseColor(sgv);
    drawValueBlock(160, 98, 1.7f, tc, true);
    drawDeltaRow(160, 184, tc, 26);
  } else {
    cv->setFont(&FreeSansTR9pt); cv->setTextColor(sevColor(1)); cv->setTextDatum(MC_DATUM); cv->drawString(L("Veri bekleniyor", "Waiting for data"), 160, 110);
  }
  cv->setFont(&FreeSansTR7pt);
  cv->setTextColor(0xA514);
  cv->setTextDatum(MC_DATUM);
  cv->drawString(ageTextTR().c_str(), 160, 226);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- COCUK: BALON
void drawUiBalloon() {
  unsigned long ms = millis();
  for (int y = 0; y < 240; y += 4) cv->fillRect(0, y, 320, 4, blendC(rgb565(110, 185, 245), rgb565(225, 242, 255), y * 100 / 240));
  int yHi = kidY(alarmHigh), yLo = kidY(alarmLow);
  cv->fillRect(0, yHi, 320, yLo - yHi, blendC(rgb565(170, 215, 250), rgb565(120, 220, 120), 35));
  cv->setFont(&FreeSansTR6pt); cv->setTextColor(rgb565(30, 110, 40)); cv->setTextDatum(TL_DATUM);
  cv->drawString(L("hedef", "target"), 6, yHi + 3);
  for (int i = 0; i < 3; i++) {
    int x = (int)((i * 150 + (ms / 45) * (i + 1) / 2) % 420) - 50, y = 44 + i * 34;
    cv->fillCircle(x, y, 12, 0xFFFF); cv->fillCircle(x + 14, y - 6, 14, 0xFFFF); cv->fillCircle(x + 30, y, 11, 0xFFFF); cv->fillRect(x, y, 30, 11, 0xFFFF);
  }
  cv->fillRect(0, 206, 320, 34, rgb565(96, 190, 90));
  for (int i = 0; i < 12; i++) cv->fillCircle(12 + i * 27, 214 + (i % 3) * 6, 2, (i % 2) ? rgb565(255, 230, 80) : rgb565(255, 140, 170));
  if (dataValid) {
    int bx = 100 + (int)(10 * sinf(ms / 900.0f)), by = kidY(sgv) - 10 + (int)(4 * sinf(ms / 500.0f));
    uint16_t c = getGlucoseColor(sgv);
    cv->drawLine(bx - 18, by + 16, bx - 7, by + 40, 0x4208); cv->drawLine(bx + 18, by + 16, bx + 7, by + 40, 0x4208);
    cv->fillCircle(bx, by, 27, c);
    cv->fillTriangle(bx - 20, by + 16, bx + 20, by + 16, bx, by + 32, c);
    cv->fillEllipse(bx - 9, by - 10, 6, 9, blendC(c, 0xFFFF, 60));
    cv->fillRoundRect(bx - 8, by + 38, 16, 11, 3, rgb565(150, 90, 40));
  }
  kidCard(0xFFFF, 0x2104, true);
  cv->setFont(&FreeSansTR6pt); cv->setTextColor(0x4208); cv->setTextDatum(TL_DATUM); cv->drawString(clockStr().c_str(), 6, 4);
  kidMessage(L("Balon", "The balloon"), 0xFFFF, 226);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- COCUK: UZAY
void drawUiSpace() {
  unsigned long ms = millis();
  cv->fillScreen(rgb565(8, 10, 34));
  for (int i = 0; i < 45; i++) {
    uint32_t h = hashU(i * 97 + 3);
    int br = 120 + (int)(110 * sinf(ms / 260.0f + i * 1.7f));
    cv->drawPixel(h % 320, (h >> 10) % 200, rgb565(br, br, 255));
  }
  int yHi = kidY(alarmHigh), yLo = kidY(alarmLow);
  cv->fillRect(0, yHi, 320, yLo - yHi, rgb565(14, 44, 56));
  cv->setFont(&FreeSansTR6pt); cv->setTextColor(rgb565(90, 200, 170)); cv->setTextDatum(TL_DATUM);
  cv->drawString(L("hedef", "target"), 6, yHi + 3);
  cv->fillCircle(10, 262, 80, rgb565(120, 70, 170));
  cv->fillCircle(-10, 240, 16, rgb565(150, 100, 200));
  cv->drawEllipse(10, 262, 110, 24, rgb565(200, 170, 240));
  if (dataValid) {
    int x = 150 + (int)(6 * sinf(ms / 700.0f)), y = kidY(sgv) + (int)(3 * sinf(ms / 400.0f));
    int fl = 10 + (int)((ms / 70) % 3) * 5;
    cv->fillTriangle(x - 7, y + 20, x + 7, y + 20, x, y + 20 + fl, rgb565(255, 160, 40));
    cv->fillTriangle(x - 4, y + 20, x + 4, y + 20, x, y + 14 + fl, rgb565(255, 240, 120));
    cv->fillRoundRect(x - 10, y - 22, 20, 44, 8, 0xFFFF);
    cv->fillTriangle(x - 10, y - 14, x + 10, y - 14, x, y - 36, rgb565(230, 50, 60));
    cv->fillTriangle(x - 10, y + 8, x - 20, y + 24, x - 10, y + 20, rgb565(230, 50, 60));
    cv->fillTriangle(x + 10, y + 8, x + 20, y + 24, x + 10, y + 20, rgb565(230, 50, 60));
    cv->fillCircle(x, y - 4, 6, rgb565(70, 150, 240));
    cv->drawCircle(x, y - 4, 6, getGlucoseColor(sgv));
  }
  kidCard(0x18E4, 0xFFFF, false);
  cv->setFont(&FreeSansTR6pt); cv->setTextColor(0xA514); cv->setTextDatum(TL_DATUM); cv->drawString(clockStr().c_str(), 6, 4);
  kidMessage(L("Roket", "The rocket"), 0xFFFF, 228);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- COCUK: AKVARYUM
void drawUiAquarium() {
  unsigned long ms = millis();
  for (int y = 0; y < 240; y += 4) cv->fillRect(0, y, 320, 4, blendC(rgb565(60, 170, 225), rgb565(12, 70, 140), y * 100 / 240));
  int yHi = kidY(alarmHigh), yLo = kidY(alarmLow);
  cv->fillRect(0, yHi, 320, yLo - yHi, rgb565(70, 175, 190));
  cv->setFont(&FreeSansTR6pt); cv->setTextColor(0xFFFF); cv->setTextDatum(TL_DATUM);
  cv->drawString(L("hedef", "target"), 6, yHi + 3);
  cv->fillRect(0, 212, 320, 28, rgb565(224, 196, 128));
  for (int p = 0; p < 4; p++) {
    int bx = 20 + p * 84;
    for (int sgm = 0; sgm < 9; sgm++) {
      int ox = (int)(5 * sinf(ms / 600.0f + sgm * 0.6f + p));
      cv->fillCircle(bx + ox, 212 - sgm * 9, 4, rgb565(40, 160 + sgm * 6, 70));
    }
  }
  for (int i = 0; i < 7; i++) {
    int period = 2500 + i * 500, t = (ms + i * 700) % period;
    int x = 30 + (i * 43) % 280, y = 210 - t * 200 / period;
    cv->drawCircle(x + (int)(3 * sinf((ms + i * 200) / 300.0f)), y, 2 + i % 3, rgb565(200, 235, 255));
  }
  if (dataValid) {
    int t = (ms / 30) % 440;
    bool right = t < 220;
    int x = right ? 30 + t : 470 - t;
    int y = kidY(sgv) + (int)(4 * sinf(ms / 450.0f));
    uint16_t c = getGlucoseColor(sgv);
    int d = right ? 1 : -1;
    cv->fillTriangle(x - d * 18, y, x - d * 34, y - 12, x - d * 34, y + 12, dimColor(c, 80));
    cv->fillEllipse(x, y, 22, 13, c);
    cv->fillTriangle(x - d * 2, y - 12, x - d * 12, y - 20, x - d * 14, y - 9, dimColor(c, 80));
    cv->fillCircle(x + d * 11, y - 3, 4, 0xFFFF);
    cv->fillCircle(x + d * 12, y - 3, 2, 0x0000);
  }
  kidCard(0xFFFF, 0x2104, true);
  cv->setFont(&FreeSansTR6pt); cv->setTextColor(0xFFFF); cv->setTextDatum(TL_DATUM); cv->drawString(clockStr().c_str(), 6, 4);
  kidMessage(L("Balık", "The fish"), rgb565(60, 40, 10), 227);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- COCUK: GULEN YUZ
void drawUiSmiley() {
  cv->fillScreen(rgb565(236, 244, 255));
  bool flash = themeFlash();
  if (flash) cv->fillScreen(getAlarmColor());
  const int cx = 98, cy = 122, R = 84;
  uint16_t face = rgb565(180, 180, 180);
  int mood = 0;                                   // 0 mutlu, 1 endiseli, 2 cok endiseli
  if (dataValid) {
    if (sgv < alarmCritLow) { face = rgb565(250, 110, 70); mood = 2; }
    else if (sgv < alarmLow) { face = rgb565(250, 205, 60); mood = 1; }
    else if (sgv > alarmVeryHigh) { face = rgb565(245, 90, 80); mood = 2; }
    else if (sgv > alarmHigh) { face = rgb565(250, 160, 60); mood = 1; }
    else face = rgb565(130, 210, 90);
  }
  cv->fillCircle(cx, cy, R, face);
  cv->drawCircle(cx, cy, R, dimColor(face, 60));
  cv->drawCircle(cx, cy, R - 1, dimColor(face, 60));
  cv->fillEllipse(cx - 28, cy - 22, 9, 13, 0x2104);
  cv->fillEllipse(cx + 28, cy - 22, 9, 13, 0x2104);
  cv->fillCircle(cx - 25, cy - 27, 3, 0xFFFF);
  cv->fillCircle(cx + 31, cy - 27, 3, 0xFFFF);
  if (!dataValid) cv->fillRoundRect(cx - 26, cy + 30, 52, 6, 3, 0x2104);
  else if (mood == 0) cv->fillArc(cx, cy + 6, 40, 46, 25, 155, 0x2104);
  else if (mood == 1) cv->fillArc(cx, cy + 70, 34, 40, 235, 305, 0x2104);
  else { cv->fillArc(cx, cy + 74, 36, 43, 220, 320, 0x2104); cv->fillEllipse(cx + 44, cy - 6, 5, 8, rgb565(90, 170, 250)); }
  if (mood == 0 && dataValid) { cv->fillCircle(cx - 50, cy + 10, 9, blendC(face, rgb565(255, 120, 150), 55)); cv->fillCircle(cx + 50, cy + 10, 9, blendC(face, rgb565(255, 120, 150), 55)); }
  cv->setFont(&FreeSansTR6pt); cv->setTextColor(0x6B4D); cv->setTextDatum(TL_DATUM); cv->drawString(clockStr().c_str(), 6, 4);
  if (dataValid) {
    uint16_t gc = flash ? (uint16_t)TFT_BLACK : dimColor(getGlucoseColor(sgv), 70);
    String gs = formatGlucose(sgv);
    cv->setFont(&lgfx::fonts::Font7);
    cv->setTextSize(unitIdx == 1 ? 0.85f : 1.0f);
    cv->setTextColor(gc);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(gs.c_str(), 252, 62);
    cv->setTextSize(1);
    drawTrendArrow(230, 110, direction, gc, 20);
    cv->setFont(&lgfx::fonts::Font4);
    cv->drawString(formatDelta(delta).c_str(), 276, 110);
  }
  // sadece hedefteyken ovgu; diger durumlarda yazi yok
  const char* l1 = ""; const char* l2 = "";
  if (dataValid && !homeDataStale() && sgv >= alarmLow && sgv <= alarmHigh) { l1 = L("Harika!", "Great!"); l2 = L("Çok iyi, hedeftesin", "Well done, on target"); }
  cv->setFont(&FreeSansTR9pt); cv->setTextColor(0x2104); cv->setTextDatum(MC_DATUM);
  cv->drawString(l1, 252, 152);
  cv->setFont(&FreeSansTR7pt); cv->setTextColor(0x4208);
  cv->drawString(l2, 252, 176);
  cv->setFont(&FreeSansTR6pt); cv->setTextColor(0x6B4D);
  cv->drawString(ageTextTR().c_str(), 252, 222);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// ---------------------------------------------------------------- hoparlor simgesi (acik / sessiz)
void drawSpeakerIcon(int cx, int cy, uint16_t c, bool on) {
  cv->fillRect(cx - 10, cy - 4, 5, 9, c);
  cv->fillTriangle(cx - 6, cy - 4, cx + 1, cy - 10, cx + 1, cy + 10, c);
  cv->fillTriangle(cx - 6, cy - 4, cx - 6, cy + 4, cx + 1, cy + 10, c);
  if (on) {
    cv->drawArc(cx + 2, cy, 6, 7, -50, 50, c);
    cv->drawArc(cx + 2, cy, 11, 12, -50, 50, c);
  } else {
    cv->drawWideLine(cx + 5, cy - 5, cx + 12, cy + 5, 1, c);
    cv->drawWideLine(cx + 5, cy + 5, cx + 12, cy - 5, 1, c);
  }
}

// ---------------------------------------------------------------- ana ekran ust katman
// Alt koselerde Dikkat (sol) ve Ertele (sag); ana temadakiyle ayni yer ve gorunum.
int homeBtnW() { return (cv->width() < 300) ? 108 : 100; }
bool homeDikkatHit(int x, int y) {
  int W = cv->width(), H = cv->height(), bw = (W < 300) ? 108 : 100;
  return x < 8 + bw + 6 && y > H - 58 && W > 0;
}
bool homeSnoozeHit(int x, int y) {
  int W = cv->width(), H = cv->height(), bw = (W < 300) ? 108 : 100;
  return x > W - 8 - bw - 6 && y > H - 58;
}

// Kucuk sure gostergesi (butonlar varken dakika bari yerine): 5 kisa cubuk + "2 dk once"
void drawCompactAge(int cx, int y) {
  bool bars = useMinuteBars();
  if (bars) {
    int n = (int)(readingAgeSec / 60UL); if (n > 5) n = 5;
    int bw = 14, gap = 3, x0 = cx - (5 * bw + 4 * gap) / 2;
    for (int i = 0; i < 5; i++) {
      uint16_t c = ageBarColor(readingAgeSec, i < n, TRACKC(0x8410));
      cv->fillRoundRect(x0 + i * (bw + gap), y, bw, 10, 3, c);
    }
  }
  String t = ageText();
  if (langIdx == 0) t.replace("once", "önce");
  cv->setTextColor(SUBT());
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(t.c_str(), cx, bars ? y + 24 : y + 12, &FreeSansTR6pt);
}

void drawHomeOverlays(int th) {
  int W = cv->width(), H = cv->height();
  if (th != 0 && dataValid && teacherStep == 0 && (shouldShowDikkat() || shouldShowSnoozeButton())) {
    int bw = homeBtnW(), by = H - 52;
    bool portrait = W < 300;
    // butonlar temanin alt kismini (dakika bari, grafik) kapatmasin: alt serit temizlenir,
    // sure bilgisi butonlarin arasina (dikeyde ustune) kucuk olarak yazilir
    int top = portrait ? by - 34 : by - 16;
    cv->fillRect(0, top, W, H - top, UI_BG);
    cv->drawFastHLine(12, top, W - 24, lightBg ? (uint16_t)0xBDF7 : (uint16_t)0x2945);
    if (portrait) drawCompactAge(W / 2, top + 6);
    else drawCompactAge(W / 2, by + 8);
    if (shouldShowDikkat()) {
      cv->fillRoundRect(8, by, bw, 44, 8, 0x780F);
      drawWarnIcon(8 + bw / 2, by + 12, TFT_WHITE, 0x780F);
      cv->setFont(&FreeSansTR7pt);
      cv->setTextColor(TFT_WHITE);
      cv->setTextDatum(MC_DATUM);
      cv->drawString(L("Dikkat", "Alert"), 8 + bw / 2, by + 32);
    }
    if (shouldShowSnoozeButton()) {
      uint16_t eb = 0x4C7C;
      int x = W - 8 - bw;
      cv->fillRoundRect(x, by, bw, 44, 8, eb);
      cv->setFont(&FreeSansTR7pt);
      cv->setTextColor(TFT_WHITE);
      cv->setTextDatum(MC_DATUM);
      cv->drawString(L("Ertele", "Snooze"), x + bw / 2, by + 13);
      char sl[12];
      snprintf(sl, sizeof(sl), L("%d dk", "%d min"), snoozeShownMins());
      cv->setFont(&FreeSansTR6pt);
      cv->drawString(sl, x + bw / 2, by + 32);
    }
  }
  // 3.4.1: kendi ust cubugu olmayan temalarda (veriyle cakismayanlar) saat / WiFi / pil
  if (!statusBarDrawn && teacherStep == 0 && (th == 0 || th == 1 || th == 2 || th == 3 || th == 8 || th == 18)) {
    bool batOn = inaPresent && batteryPercent >= 0;
    bool oldBat = (th == 1 || th == 2 || th == 3);         // bu temalarda eski pil gostergesi var
    int lx = 8;
    if (th == 2 || th == 0) {                              // saati olmayan tema: sol ustte saat
      cv->setFont(&lgfx::fonts::Font2); cv->setTextColor(TXT()); cv->setTextDatum(ML_DATUM);
      cv->drawString(clockStr().c_str(), 8, 11);
      lx = 8 + cv->textWidth(clockStr().c_str()) + 8;
    } else if (th == 18) {
      cv->setFont(&FreeSansTR6pt);
      lx = 6 + cv->textWidth(clockStr().c_str()) + 8;      // temanin kendi kucuk saatinin yani
    }
    bool lbSave = lightBg;
    if (th == 18) lightBg = true;                          // gulen yuz: acik zemin
    if (soundWarnText().length()) drawMuteGlyph(cv, lx, 5, muteGlyphColor());
    drawWifiGlyph(cv, !batOn ? W - 22 : (oldBat ? W - 88 : W - 82), 4);
    if ((th == 18 || th == 0) && batOn) drawBatteryGlyph(cv, W - 30, 5);
    lightBg = lbSave;
    statusBarDrawn = true;
    cv->setTextDatum(TL_DATUM);
  }
  // WiFi yok: ustte hap, dokununca hata ekrani (Kurulum / Yenile)
  if (homeWifiPillOn()) {
    String pt = homeNetPillText();
    const GFXfont* pf = &FreeSansTR7pt;
    cv->setFont((const lgfx::IFont*)pf);
    int pw = cv->textWidth(pt.c_str()) + 24;
    if (pw > 160) { pf = &FreeSansTR6pt; cv->setFont((const lgfx::IFont*)pf); pw = cv->textWidth(pt.c_str()) + 20; }   // sag ustteki simgelere degmesin
    cv->setTextFont(1);
    if (pw < 150) pw = 150;
    int px = W / 2 - pw / 2, py = (W < 300) ? 30 : 4;
    cv->fillRoundRect(px, py, pw, 26, 13, 0xB000);
    cv->setTextColor(TFT_WHITE);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(pt.c_str(), W / 2, py + 13, pf);
  }
  // alarm sesi calmayacaksa: ust cubugu olan temalarda saatin yaninda (drawStatusBar),
  // olmayanlarda sag ustte kucuk sessiz simgesi. Metin Alarmlar / Hedefler sayfasinda.
  if (!statusBarDrawn && soundWarnText().length()) {
    bool batOn = inaPresent && batteryPercent >= 0;
    int gx = (th == 0 || th == 2 || th == 3 || !batOn) ? W - 22 : W - 88;   // pil simgesi olan temalarda onun solunda
    if (lowBatDim && inaPresent && (th == 0 || th == 2 || th == 3)) gx = W - 54;
    drawMuteGlyph(cv, gx, 6, muteGlyphColor());
  }
  // dusuk pil simgesi (pil gostergesi olmayan temalarda)
  if (lowBatDim && inaPresent && batteryPercent >= 0 && (th == 0 || th == 2 || th == 3)) drawBatteryGlyph(cv, W - 30, 5);
  if ((long)(millis() - homeHintUntil) < 0) {
    cv->fillRoundRect(W / 2 - 120, H - 44, 240, 30, 15, 0x2945);
    cv->setFont(&FreeSansTR7pt);
    cv->setTextColor(TFT_WHITE);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(L("<<  Kaydır: sayfalar / uygulamalar  >>", "<<  Swipe: pages / apps  >>"), W / 2, H - 29);
  }
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

// Secili temayi (arka plan tamponu drawCurrentScreen'de) ciz
// Veri, "veri yok" alarm suresinden eski mi (deger hala ekranda ama guncel degil)
bool homeDataStale() {
  return dataValid && staleAlarmMin > 0 && readingAgeSec > (unsigned long)staleAlarmMin * 60UL;
}

// Eski veri etiketi: kirmizi "Veri eski: 25 dk once" (deger zaten gri cizildi)
void drawStaleLabel() {
  int W = cv->width();
  String t = ageText();
  if (langIdx == 0) t.replace("once", "önce");
  t = String(L("Veri eski: ", "Old data: ")) + t;
  cv->setFont(&FreeSansTR7pt);
  int tw = cv->textWidth(t.c_str());
  int pw = tw + 24, px = W / 2 - pw / 2;
  int py = (W < 300) ? 30 : 4;
  if (homeWifiPillOn()) py += 30;                     // WiFi yok hapinin altina
  cv->fillRoundRect(px, py, pw, 24, 12, sevColor(0));
  cv->setTextColor(TFT_WHITE);
  cv->setTextDatum(MC_DATUM);
  cv->drawString(t.c_str(), W / 2, py + 12);
  cv->setTextDatum(TL_DATUM);
}

void drawHomeContent(int th) {
  statusBarDrawn = false;
  homeStaleGrey = homeDataStale() && teacherStep == 0;
  int savedMode = uiPageMode;
  uiPageMode = themeUiMode(th);
  lightBg = bgIsLight(currentBg()) && frameActive;
  bool ana = (th == 0);
  if (ana) drawScreen0(); else drawScreen1();
  if (!ana && batteryIndicatorVisible()) drawBatteryIndicator();
  homeStaleGrey = false;
  if (!(ana && teacherStep > 0)) drawHomeOverlays(th);
  if (homeDataStale() && teacherStep == 0) drawStaleLabel();
  uiPageMode = savedMode;
  lightBg = false;
}
void drawHome() {
  if (curPage != P_TH2 && curPage != P_TH3) { pageThemeOverride = -1; curPage = P_HOME; }
  uiPageMode = themeUiMode(activeTheme());
  drawHomeContent(activeTheme());
}

// ---------------------------------------------------------------- UYGULAMALAR
#define APP_N 8
static const uint16_t APP_COL[APP_N] = {0x8ADB, 0x1D52, 0xF542, 0x3C1C, 0x5B75, 0x1D0C, 0x4235, 0x52AA};

const char* appName(int i) {
  switch (i) {
    case 0: return L("Tema", "Theme");
    case 1: return L("Grafik", "Graph");
    case 2: return L("İstatistik", "Stats");
    case 3: return L("Sensör", "Sensor");
    case 4: return L("Zamanlayıcı", "Timer");
    case 5: return L("Karb-Not", "Carbs");
    case 6: return L("Modlar", "Modes");
    default: return L("Ayarlar", "Settings");
  }
}

void drawAppGlyph(int id, int cx, int cy, uint16_t bg) {
  lgfx::LovyanGFX* g = cv;
  const uint16_t W = TFT_WHITE;
  switch (id) {
    case 0:   // palet
      g->fillCircle(cx, cy, 16, W);
      g->fillCircle(cx + 8, cy + 8, 5, bg);
      g->fillCircle(cx - 7, cy - 6, 4, 0xF800);
      g->fillCircle(cx + 3, cy - 9, 4, 0xFFE0);
      g->fillCircle(cx + 10, cy - 1, 4, 0x07E0);
      g->fillCircle(cx - 9, cy + 5, 4, 0x041F);
      break;
    case 1:   // cizgi grafik
      g->drawFastVLine(cx - 16, cy - 15, 31, W); g->drawFastHLine(cx - 16, cy + 15, 33, W);
      for (int t = -1; t <= 1; t++) {
        g->drawLine(cx - 13, cy + 6 + t, cx - 4, cy - 4 + t, W);
        g->drawLine(cx - 4, cy - 4 + t, cx + 3, cy + 3 + t, W);
        g->drawLine(cx + 3, cy + 3 + t, cx + 15, cy - 11 + t, W);
      }
      break;
    case 2:   // cubuk grafik
      g->fillRoundRect(cx - 15, cy + 1, 8, 15, 2, W);
      g->fillRoundRect(cx - 4, cy - 14, 8, 30, 2, W);
      g->fillRoundRect(cx + 7, cy - 5, 8, 21, 2, W);
      break;
    case 3:   // sensor diski
      g->fillCircle(cx, cy, 17, W);
      g->fillCircle(cx, cy, 13, bg);
      g->fillCircle(cx, cy, 6, W);
      break;
    case 4:   // kronometre
      g->fillCircle(cx, cy + 3, 15, W);
      g->fillCircle(cx, cy + 3, 12, bg);
      g->fillRect(cx - 4, cy - 17, 8, 4, W);
      g->fillRect(cx - 1, cy - 14, 3, 4, W);
      g->drawLine(cx, cy + 3, cx + 6, cy - 4, W); g->drawLine(cx + 1, cy + 3, cx + 7, cy - 4, W);
      g->fillCircle(cx, cy + 3, 2, W);
      break;
    case 5:   // catal + bicak
      for (int t = 0; t < 3; t++) g->fillRect(cx - 14 + t * 4, cy - 16, 2, 11, W);
      g->fillRoundRect(cx - 14, cy - 7, 10, 5, 2, W);
      g->fillRect(cx - 10, cy - 4, 3, 20, W);
      g->fillRoundRect(cx + 5, cy - 16, 7, 18, 3, W);
      g->fillRect(cx + 6, cy, 4, 16, W);
      break;
    case 6:   // ay (modlar)
      g->fillCircle(cx - 2, cy + 1, 16, W);
      g->fillCircle(cx + 7, cy - 6, 14, bg);
      g->fillCircle(cx + 10, cy + 9, 2, W);
      g->fillCircle(cx + 14, cy - 12, 1, W);
      break;
    default:  // disli
      for (int k = 0; k < 8; k++) {
        float a = k * 45 * DEG_TO_RAD;
        g->fillCircle(cx + (int)(cosf(a) * 14), cy + (int)(sinf(a) * 14), 4, W);
      }
      g->fillCircle(cx, cy, 12, W);
      g->fillCircle(cx, cy, 5, bg);
      break;
  }
}

void drawAppsContent() {
  cv->fillScreen(UI_BG);
  drawStatusBar(true);
  for (int i = 0; i < APP_N; i++) {
    int c = i % 4, r = i / 4;
    int rowTop = 28 + r * 100;
    int x = c * 80 + 13, y = rowTop + 4;
    cv->fillRoundRect(x, y, 54, 54, 15, APP_COL[i]);
    drawAppGlyph(i, x + 27, y + 27, APP_COL[i]);
    cv->setFont(&FreeSansTR7pt);
    if (cv->textWidth(appName(i)) > 70) cv->setFont(&FreeSansTR6pt);
    cv->setTextColor(TFT_WHITE);
    cv->setTextDatum(TC_DATUM);
    cv->drawString(appName(i), c * 80 + 40, rowTop + 64);
  }
  cv->setFont(&FreeSansTR6pt);
  cv->setTextColor(0x8C71);
  cv->setTextDatum(BC_DATUM);
  cv->drawString(L("<<  Ana ekran için sola kaydır", "<<  Swipe left for home"), 160, 238);
  cv->setTextDatum(TL_DATUM);
  cv->setFont(&lgfx::fonts::Font0);
}

void drawAppsPage() { drawAppsContent(); }

void openApp(int i) {
  curPage = -1; pageThemeOverride = -1;
  formId = 0; formDepth = 0; textPageActive = false; timePickerActive = false;
  subScreen = 0; graphDetailOpen = false; graphMenuOpen = false; showGuide = false;
  switch (i) {
    case 0: thSel = homeTheme; bgSel = homeBg; thDarkSel = homeBgDark; themePreview = false; themeScroll = 0; thPickSlot = 0; currentScreen = SCR_THEME; break;
    case 1: currentScreen = SCR_GRAPH; break;
    case 2: currentScreen = SCR_STATS; break;
    case 3: currentScreen = SCR_SENSOR; break;
    case 4: currentScreen = SCR_TIMER; break;
    case 5: karbView = 0; karbSel = -1; currentScreen = SCR_KARB; break;
    case 6: currentScreen = SCR_APPS; curPage = P_APPS; formOpen(F_MODES); return;
    default: currentScreen = SCR_SETTINGS; settingsScroll = 0; break;
  }
  applyRotation();
  drawCurrentScreen();
}

void appsTap(int x, int y) {
  int r = (y >= 28 && y < 122) ? 0 : (y >= 128 && y < 222) ? 1 : -1;
  if (r < 0) return;
  int c = x / 80;
  if (c < 0 || c > 3) return;
  openApp(r * 4 + c);
}

// ---------------------------------------------------------------- geri / ana ekran
void goHome() {
  formId = 0; formDepth = 0; formDlg = 0; textPageActive = false; timePickerActive = false;
  subScreen = 0; showGuide = false; graphDetailOpen = false; graphMenuOpen = false;
  uiPreviewActive = false; themePreview = false;
  if (videoPlaying) videoStop();
  curPage = P_HOME; pageThemeOverride = -1;
  currentScreen = homeScreenIdx();
  uiPageMode = themeUiMode(homeTheme);
  applyRotation();
  drawCurrentScreen();
}

void goApps() {
  formId = 0; formDepth = 0; formDlg = 0; textPageActive = false; timePickerActive = false;
  subScreen = 0; showGuide = false; graphDetailOpen = false; graphMenuOpen = false;
  themePreview = false;
  curPage = P_APPS; pageThemeOverride = -1;
  currentScreen = SCR_APPS;
  applyRotation();
  drawCurrentScreen();
}

// ---------------------------------------------------------------- baslik (Geri + baslik + istege bagli dugme)
void drawFormHeaderBtn(const char* title, const char* btn, bool hot) {
  cv->fillRect(0, 0, 320, LIST_Y, UI_BG);
  for (int t = 0; t < 3; t++) {
    cv->drawLine(18 + t, 12, 10 + t, 20, UI_ACCENT);
    cv->drawLine(10 + t, 20, 18 + t, 28, UI_ACCENT);
  }
  cv->setTextColor(UI_ACCENT, UI_BG);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 26, 20, &FreeSansTR7pt);
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(title, btn ? 158 : 170, 20, &FreeSansTR9pt);
  if (btn) {
    uint16_t sb = hot ? (uint16_t)0x0B5E : (uint16_t)0x2945;
    cv->fillRoundRect(244, 6, 70, 28, 14, sb);
    cv->setTextColor(TFT_WHITE, sb);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(btn, 279, 20, &FreeSansTR7pt);
  }
  cv->drawFastHLine(0, LIST_Y - 2, 320, UI_LINE);
  cv->setTextDatum(TL_DATUM);
}
void drawFormHeader(const char* title, bool save) { drawFormHeaderBtn(title, save ? L("Kaydet", "Save") : nullptr, formDirty); }

// sayfa basinda kucuk geri oku (Grafik, Istatistik, Zamanlayici)
void drawBackChevron(int cx, int cy) {
  for (int t = 0; t < 3; t++) {
    cv->drawLine(cx + 4 + t, cy - 8, cx - 4 + t, cy, UI_ACCENT);
    cv->drawLine(cx - 4 + t, cy, cx + 4 + t, cy + 8, UI_ACCENT);
  }
  uint32_t fg = cv->getTextStyle().fore_rgb888;
  cv->setTextColor(UI_ACCENT);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), cx + 12, cy, &FreeSansTR7pt);
  cv->setTextColor(fg);
  cv->setTextDatum(TL_DATUM);
}
// ---------------------------------------------------------------- TEMA SECIMI: tek kaydirmali sayfa, bolum bolum (3.0.0)
#define TH_TILE_W 72
#define TH_TILE_H 58
#define TH_ROW    66
#define TH_VIEW   162           // listSpr icinde icerik yuksekligi (alt cubuk ustu)
#define TH_BAR_Y  164           // listSpr icinde alt cubuk (ekranda 204)
#define TH_SEC1   26            // Temalar karolari
#define TH_SEC2   180           // Animasyon karolari
#define TH_SEC3   312           // Arka plan basligi
int thKeepSel = 0, thKeepBg = 0;     // secim modundan donunce ana tema secimi korunsun
static const int TH_MAIN[8] = {0, 1, 2, 3, 4, 5, 6, 12};
static const int TH_ANIM[8] = {8, 9, 13, 14, 15, 16, 17, 18};
static const int BG_DARK[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 13};
static const int BG_LIGHT[5] = {9, 10, 11, 12, 14};

int themeContentH() { return thPickSlot ? 312 : 566; }
int swatchCx(int k) { return 34 + (k % 5) * 63; }
int swatchCyDark(int k) { return TH_SEC3 + 62 + (k / 5) * 62; }
int swatchCyLight() { return TH_SEC3 + 214; }

void drawThemeTile(lgfx::LovyanGFX* g, int t, int x, int y, bool sel) {
  const int w = TH_TILE_W, h = TH_TILE_H;
  g->fillRoundRect(x, y, w, h, 10, 0x10A2);
  int cx = x + w / 2;
  uint16_t G = sevColor(2);
  g->setTextDatum(MC_DATUM);
  g->setTextSize(1);
  switch (t) {
    case 0:
      g->setFont(&lgfx::fonts::Font4); g->setTextColor(G); g->drawString("120", cx - 6, y + 17);
      g->fillTriangle(cx + 20, y + 12, cx + 30, y + 17, cx + 20, y + 22, G);
      for (int i = 0; i < 5; i++) g->fillRect(x + 12 + i * 10, y + 32, 8, 4, i < 3 ? G : 0x3186);
      break;
    case 1:
      g->setFont(&lgfx::fonts::Font2); g->setTextColor(TFT_WHITE); g->drawString("12:30", cx, y + 9);
      g->setTextColor(G); g->drawString("120", cx, y + 24);
      g->drawLine(x + 10, y + 38, x + 30, y + 34, G); g->drawLine(x + 30, y + 34, x + 62, y + 36, G);
      break;
    case 2:
      g->setFont(&lgfx::fonts::Font2); g->setTextColor(G); g->setTextSize(2); g->drawString("120", cx, y + 20); g->setTextSize(1);
      break;
    case 3:
      for (int r = 0; r < 3; r++) for (int c = 0; c < 6; c++)
        g->fillRect(x + 10 + c * 9, y + 8 + r * 9, 6, 6, (r == 1 && c == 3) ? (uint16_t)0x4C9F : (uint16_t)0x6B4D);
      break;
    case 4:
      g->fillRect(x + 16, y + 6, 40, 30, 0xD600);
      g->fillRect(x + 18, y + 8, 36, 26, 0x5D1F);
      g->fillTriangle(x + 20, y + 33, x + 32, y + 18, x + 44, y + 33, 0x2444);
      g->fillCircle(x + 46, y + 14, 3, TFT_YELLOW);
      break;
    case 5:
      g->fillArc(cx, y + 22, 12, 17, 135, 405, 0x3186);
      g->fillArc(cx, y + 22, 12, 17, 135, 300, G);
      g->setFont(&lgfx::fonts::Font0); g->setTextColor(G); g->drawString("120", cx, y + 22);
      break;
    case 6:
      g->fillRoundRect(x + 8, y + 6, 56, 18, 4, 0x18E4);
      g->fillRect(x + 11, y + 9, 3, 12, G);
      g->setFont(&lgfx::fonts::Font2); g->setTextColor(G); g->drawString("120", x + 34, y + 15);
      g->drawLine(x + 10, y + 36, x + 30, y + 30, G); g->drawLine(x + 30, y + 30, x + 62, y + 34, G);
      break;
    case 7:
      g->drawRoundRect(cx - 12, y + 3, 24, 38, 4, 0x8410);
      g->setFont(&lgfx::fonts::Font2); g->setTextColor(G); g->drawString("120", cx, y + 16);
      g->drawLine(cx - 8, y + 32, cx, y + 29, G); g->drawLine(cx, y + 29, cx + 8, y + 31, G);
      break;
    case 8:
      g->drawCircle(cx - 10, y + 21, 17, 0x8410);
      g->drawLine(cx - 10, y + 21, cx - 10, y + 9, TFT_WHITE); g->drawLine(cx - 10, y + 21, cx - 1, y + 24, TFT_WHITE);
      g->setFont(&lgfx::fonts::Font0); g->setTextColor(G); g->drawString("120", cx + 20, y + 21);
      break;
    case 9:
      for (int i = 0; i < 56; i += 2) { int yy = y + 28 + (int)(4 * sinf(i / 6.0f)); g->fillRect(x + 8 + i, yy, 2, y + 42 - yy, dimColor(G, 55)); }
      g->setFont(&lgfx::fonts::Font2); g->setTextColor(G); g->drawString("120", cx, y + 14);
      break;
    case 10:
      g->setFont(&lgfx::fonts::Font4); g->setTextColor(G); g->setTextSize(1.3f); g->drawString("120", cx, y + 21); g->setTextSize(1);
      break;
    case 11:
      g->fillRoundRect(x + 12, y + 5, 10, 32, 5, 0x3186); g->fillRoundRect(x + 14, y + 18, 6, 19, 3, G); g->fillCircle(x + 17, y + 38, 6, G);
      g->setFont(&lgfx::fonts::Font2); g->setTextColor(G); g->drawString("120", x + 46, y + 20);
      break;
    case 12:
      for (int i = 0; i < 4; i++) g->fillRoundRect(x + 8 + (i % 2) * 29, y + 4 + (i / 2) * 19, 26, 16, 3, 0x2945);
      g->setFont(&lgfx::fonts::Font0); g->setTextColor(G); g->drawString("120", x + 21, y + 12); g->drawString("%82", x + 21, y + 31);
      break;
    case 13:
      g->drawCircle(cx, y + 21, 18, dimColor(G, 40)); g->drawCircle(cx, y + 21, 13, dimColor(G, 70)); g->fillCircle(cx, y + 21, 10, 0x2945);
      g->setFont(&lgfx::fonts::Font0); g->setTextColor(G); g->drawString("120", cx, y + 21);
      break;
    case 14:
      for (int i = 0; i < 14; i++) g->drawPixel(x + 6 + (i * 37) % 60, y + 4 + (i * 23) % 36, 0xFFFF);
      g->setFont(&lgfx::fonts::Font2); g->setTextColor(G); g->drawString("120", cx, y + 21);
      break;
    case 15:
      g->fillRect(x + 4, y + 4, 64, 38, rgb565(130, 195, 245)); g->fillRect(x + 4, y + 36, 64, 6, rgb565(96, 190, 90));
      g->fillCircle(cx - 6, y + 17, 9, G); g->fillRect(cx - 9, y + 28, 6, 4, rgb565(150, 90, 40));
      break;
    case 16:
      g->fillRect(x + 4, y + 4, 64, 38, rgb565(8, 10, 34));
      for (int i = 0; i < 8; i++) g->drawPixel(x + 8 + i * 7, y + 8 + (i * 13) % 28, 0xFFFF);
      g->fillRoundRect(cx - 4, y + 12, 8, 18, 3, 0xFFFF); g->fillTriangle(cx - 4, y + 13, cx + 4, y + 13, cx, y + 5, rgb565(230, 50, 60));
      g->fillTriangle(cx - 3, y + 30, cx + 3, y + 30, cx, y + 37, rgb565(255, 160, 40));
      break;
    case 17:
      g->fillRect(x + 4, y + 4, 64, 38, rgb565(40, 140, 210)); g->fillRect(x + 4, y + 37, 64, 5, rgb565(224, 196, 128));
      g->fillEllipse(cx, y + 20, 11, 7, G); g->fillTriangle(cx - 10, y + 20, cx - 18, y + 14, cx - 18, y + 26, G);
      break;
    case 18:
      g->fillCircle(cx, y + 21, 17, rgb565(130, 210, 90)); g->fillCircle(cx - 6, y + 16, 2, 0x2104); g->fillCircle(cx + 6, y + 16, 2, 0x2104);
      g->fillArc(cx, y + 20, 8, 10, 25, 155, 0x2104);
      break;
  }
  g->setFont(&FreeSansTR6pt);
  g->setTextColor(TFT_WHITE);
  g->setTextDatum(BC_DATUM);
  g->drawString(themeName(t), cx, y + h - 3);
  if (sel) {
    g->drawRoundRect(x - 2, y - 2, w + 4, h + 4, 12, UI_ACCENT);
    g->drawRoundRect(x - 3, y - 3, w + 6, h + 6, 13, UI_ACCENT);
  }
  g->setTextDatum(TL_DATUM);
}

void drawBgSwatch(lgfx::LovyanGFX* g, int b, int cx, int cy, bool sel, bool ok) {
  static uint16_t sw[36 * 36];
  const int S = 36, R = 17;
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      int dx = x - S / 2, dy = y - S / 2;
      uint16_t c;
      if (dx * dx + dy * dy <= R * R) {
        c = bgPixelH(b, x * 2, (b == 7) ? y * 6 : y * 2, 240);
        if (!ok) c = blendC(c, 0x4208, 60);
        if (dx * dx + dy * dy >= (R - 1) * (R - 1)) c = 0x7BEF;
      } else c = UI_BG;
      sw[y * S + x] = c;
    }
  g->setSwapBytes(true);
  g->pushImage(cx - S / 2, cy - S / 2, S, S, sw);
  g->setSwapBytes(false);
  if (sel) { g->drawCircle(cx, cy, R + 3, TFT_WHITE); g->drawCircle(cx, cy, R + 4, TFT_WHITE); }
  g->setFont(&FreeSansTR6pt);
  g->setTextColor(ok ? (uint16_t)0xC618 : (uint16_t)0x6B4D);
  g->setTextDatum(TC_DATUM);
  g->drawString(bgName(b), cx, cy + R + 6);
}

void drawThemeHeader() {
  cv->fillRect(0, 0, 320, LIST_Y, UI_BG);
  for (int t = 0; t < 3; t++) {
    cv->drawLine(16 + t, 12, 8 + t, 20, UI_ACCENT);
    cv->drawLine(8 + t, 20, 16 + t, 28, UI_ACCENT);
  }
  cv->setTextColor(UI_ACCENT, UI_BG);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(L("Geri", "Back"), 22, 20, &FreeSansTR7pt);
  if (thPickSlot) {
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(thPickSlot == 2 ? L("2. sayfanın teması", "Theme for page 2") : L("3. sayfanın teması", "Theme for page 3"), 180, 20, &FreeSansTR9pt);
  } else {
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("TEMALAR", "THEMES"), 170, 20, &FreeSansTR9pt);
  }
  cv->drawFastHLine(0, LIST_Y - 2, 320, UI_LINE);
  cv->setTextDatum(TL_DATUM);
}

static void thSection(lgfx::LovyanGFX* g, const char* t, const char* sub, int y) {
  g->setFont(&FreeSansTR9pt); g->setTextColor(TFT_WHITE); g->setTextDatum(ML_DATUM);
  g->drawString(t, 10, y);
  if (sub) {
    int w = g->textWidth(t);
    g->setFont(&FreeSansTR6pt); g->setTextColor(UI_SUBTEXT);
    g->drawString(sub, 18 + w, y + 1);
  }
  g->setTextDatum(TL_DATUM);
}

int thSelNow() { return thSel; }
int thSavedNow() { return thPickSlot == 2 ? pgTh2Theme : thPickSlot == 3 ? pgTh3Theme : homeTheme; }

void drawThemeBody() {
  int maxS = themeContentH() - TH_VIEW;
  if (maxS < 0) maxS = 0;
  if (themeScroll < 0) themeScroll = 0;
  if (themeScroll > maxS) themeScroll = maxS;
  bool tmpKey = bgFrameOn();
  if (tmpKey) uiBg = HOME_KEY;
  int oy;
  lgfx::LovyanGFX* g = listTarget(oy);
  lgfx::LovyanGFX* saved = cv;
  cv = g;
  g->fillRect(0, oy, 320, LIST_H, UI_BG);
  g->setClipRect(0, oy, 320, TH_VIEW);
  int base = oy - themeScroll;
  thSection(g, L("Temalar", "Themes"), nullptr, base + 13);
  for (int k = 0; k < 8; k++) {
    int y = base + TH_SEC1 + (k / 4) * TH_ROW;
    if (y + TH_TILE_H < oy || y > oy + TH_VIEW) continue;
    drawThemeTile(g, TH_MAIN[k], 8 + (k % 4) * 77, y, TH_MAIN[k] == thSel);
  }
  thSection(g, L("Animasyon", "Animated"), L("çocuk temaları dahil", "incl. kids themes"), base + TH_SEC2 - 13);
  for (int k = 0; k < 8; k++) {
    int y = base + TH_SEC2 + (k / 4) * TH_ROW;
    if (y + TH_TILE_H < oy || y > oy + TH_VIEW) continue;
    drawThemeTile(g, TH_ANIM[k], 8 + (k % 4) * 77, y, TH_ANIM[k] == thSel);
  }
  if (!thPickSlot) {
    bool lightOk = themeLightOK(thSel);
    thSection(g, L("Arka plan", "Backdrop"), nullptr, base + TH_SEC3 + 10);
    g->setFont(&FreeSansTR6pt); g->setTextColor(UI_SUBTEXT); g->setTextDatum(TL_DATUM);
    g->drawString(themeOwnScene(thSel) ? L("Koyu - bu tema kendi resmini çizer; diğer sayfalarda geçerli", "Dark - this theme draws its own scene")
                                       : L("Koyu (tüm sayfalar)", "Dark (all pages)"), 10, base + TH_SEC3 + 26);
    if (bgIsLight(bgSel)) {                                // acik seciliyken diger sayfalarda kullanilacak koyu: ince halka
      for (int k = 0; k < 10; k++) if (BG_DARK[k] == thDarkSel) {
        int cy = base + swatchCyDark(k);
        g->drawCircle(swatchCx(k), cy, 20, UI_ACCENT);
      }
    }
    for (int k = 0; k < 10; k++) {
      int cy = base + swatchCyDark(k);
      if (cy + 40 < oy || cy - 24 > oy + TH_VIEW) continue;
      drawBgSwatch(g, BG_DARK[k], swatchCx(k), cy, BG_DARK[k] == bgSel, true);
    }
    g->setTextDatum(TL_DATUM); g->setFont(&FreeSansTR6pt); g->setTextColor(UI_SUBTEXT);
    g->drawString(lightOk ? L("Açık - uygun temalarda; diğer sayfalar mavi halkalı koyuyla", "Light - suitable themes; other pages use the ringed dark")
                          : L("Açık: bu temada kullanılamaz", "Light: not for this theme"),
                  10, base + TH_SEC3 + 176);
    for (int k = 0; k < 5; k++) drawBgSwatch(g, BG_LIGHT[k], swatchCx(k), base + swatchCyLight(), BG_LIGHT[k] == bgSel, lightOk);
  }
  g->clearClipRect();
  drawScrollbar(g, oy, themeScroll, maxS, themeContentH() + (LIST_H - TH_VIEW));
  // alt cubuk: Onizle | Kaydet (secim modunda Sec)
  int by = oy + TH_BAR_Y;
  g->fillRect(0, by - 2, 320, LIST_H - TH_BAR_Y + 2, UI_BG);
  g->drawFastHLine(0, by - 2, 320, UI_LINE);
  g->fillRoundRect(8, by + 3, 148, 30, 15, 0x2945);
  uint16_t sb = (formDirty || thPickSlot) ? (uint16_t)0x0B5E : (uint16_t)0x2945;
  g->fillRoundRect(164, by + 3, 148, 30, 15, sb);
  g->setFont(&FreeSansTR7pt);
  g->setTextColor(TFT_WHITE);
  g->setTextDatum(MC_DATUM);
  g->drawString(L("Önizle", "Preview"), 82, by + 18);
  g->drawString(thPickSlot ? L("Seç", "Choose") : L("Kaydet", "Save"), 238, by + 18);
  g->setTextDatum(TL_DATUM);
  cv = saved;
  listFlush();
  if (tmpKey) uiBg = 0;
}

void drawThemePage() {
  if (themePreview) {
    drawHomeContent(thSel);
    int W = cv->width(), H = cv->height();
    cv->fillRoundRect(W / 2 - 100, H - 36, 200, 30, 15, 0x0B5E);
    cv->setFont(&FreeSansTR7pt);
    cv->setTextColor(TFT_WHITE);
    cv->setTextDatum(MC_DATUM);
    cv->drawString(L("Önizleme - dokun: geri", "Preview - tap: back"), W / 2, H - 21);
    cv->setTextDatum(TL_DATUM);
    return;
  }
  cv->fillScreen(UI_BG);
  formDirty = thPickSlot ? (thSel != thSavedNow()) : ((thSel != homeTheme) || (bgSel != homeBg) || (bgIsLight(bgSel) && thDarkSel != homeBgDark));
  drawThemeHeader();
  drawThemeBody();
}

// 2./3. sayfa temasi secildi: sayfayi ac, Sayfalar listesine don
void themePickDone(bool apply) {
  if (apply) {
    if (thPickSlot == 2) { pgTh2Theme = thSel; pgOn[P_TH2] = true; }
    if (thPickSlot == 3) { pgTh3Theme = thSel; pgOn[P_TH3] = true; }
    saveSettings();
  }
  thPickSlot = 0;
  thSel = thKeepSel; bgSel = thKeepBg; themeScroll = 0;
  if (thPickFrom >= 0) currentScreen = thPickFrom;
  thPickFrom = -1;
  formOpen(F_PAGES);
}

void openThemePick(int slot) {
  thKeepSel = thSel; thKeepBg = bgSel;
  thPickFrom = currentScreen;
  formId = 0; formDepth = 0; formDlg = 0;
  thPickSlot = slot;
  thSel = (slot == 2) ? pgTh2Theme : pgTh3Theme;
  if (themeHidden(thSel)) thSel = 0;
  themePreview = false; themeScroll = 0;
  currentScreen = SCR_THEME;
  drawCurrentScreen();
}

void themeSave() {
  if (thPickSlot) { themePickDone(true); return; }
  homeTheme = thSel; homeBg = bgSel;
  homeBgDark = bgIsLight(thDarkSel) ? 0 : thDarkSel;
  if (!bgIsLight(homeBg)) homeBgDark = homeBg;
  uiPageMode = themeUiMode(homeTheme);
  prefs.begin("metematik", false);
  prefs.putInt("homebgd", homeBgDark);
  prefs.putInt("hometh", homeTheme);
  prefs.putInt("homebg", homeBg);
  prefs.putInt("uipage", uiPageMode);
  prefs.end();
  if (homeTheme == 4) loadFotoFromFS();
  goHome();
}

// liste mekanizmasindan (kind 7) kisa dokunus
void themeTap(int x, int y) {
  if (themePreview) { themePreview = false; drawCurrentScreen(); return; }
  if (y < LIST_Y) {
    if (x < 70) { if (thPickSlot) themePickDone(false); else goApps(); }
    return;
  }
  if (y >= LIST_Y + TH_BAR_Y) {
    if (x < 160) { themePreview = true; drawCurrentScreen(); }
    else themeSave();
    return;
  }
  int ry = y - LIST_Y + themeScroll;
  int c = (x - 8) / 77;
  for (int sec = 0; sec < 2; sec++) {
    int top = sec ? TH_SEC2 : TH_SEC1;
    int r = (ry - top) / TH_ROW;
    if (ry >= top && r < 2 && c >= 0 && c < 4 && (ry - top) % TH_ROW < TH_TILE_H) {
      thSel = sec ? TH_ANIM[r * 4 + c] : TH_MAIN[r * 4 + c];
      drawCurrentScreen();
      return;
    }
  }
  if (thPickSlot) return;
  for (int k = 0; k < 10; k++) {
    int dx = x - swatchCx(k), dy = ry - swatchCyDark(k);
    if (dx * dx + dy * dy <= 26 * 26) { bgSel = BG_DARK[k]; thDarkSel = bgSel; drawCurrentScreen(); return; }
  }
  for (int k = 0; k < 5; k++) {
    int dx = x - swatchCx(k), dy = ry - swatchCyLight();
    if (dx * dx + dy * dy <= 26 * 26) {
      if (!themeLightOK(thSel)) return;
      bgSel = BG_LIGHT[k]; drawCurrentScreen(); return;
    }
  }
}

// =====================================================================
//  2.5.0 - TELEFON TARZI AYAR SAYFALARI (Geri / Kaydet, secmeli dugmeler)
// =====================================================================
FRow fRows[FROW_MAX];
int fRowN = 0, fContentH = 0;
bool fHasSave = true;
String fTitle;
int fPressed = -1;

FSnap fSnap[40];
int fSnapN = 0;

FRow& fAdd(uint8_t type, uint8_t id, const String& label) {
  static FRow dummy;
  if (fRowN >= FROW_MAX) return dummy;
  FRow& r = fRows[fRowN++];
  r.type = type; r.id = id; r.label = label; r.value = "";
  r.ip = nullptr; r.ip2 = nullptr; r.bp = nullptr; r.tp = nullptr;
  r.nOpt = 0; r.help = 0; r.infoX = -100; r.col = TFT_WHITE;
  return r;
}
void fHead(const String& t) { fAdd(FR_HEAD, 0, t); }
void fNote(const String& t, uint16_t col = 0xA514) { FRow& r = fAdd(FR_NOTE, 0, t); r.col = col; }
void fTog(uint8_t id, const String& t, bool* b, uint8_t help = 0) { FRow& r = fAdd(FR_TOG, id, t); r.bp = b; r.help = help; }
void fAct(uint8_t id, const String& t, const String& v = "", uint8_t help = 0, uint16_t col = TFT_WHITE) {
  FRow& r = fAdd(FR_ACT, id, t); r.value = v; r.help = help; r.col = col;
}
void fSeg(uint8_t id, const String& t, int* ip, int n, const char* const* o, const int* v, uint8_t help = 0) {
  FRow& r = fAdd(FR_SEG, id, t); r.ip = ip; r.help = help; r.nOpt = n;
  for (int i = 0; i < n && i < 5; i++) { r.opt[i] = o[i]; r.ov[i] = v[i]; }
}
void fNumSnd(uint8_t id, const String& t, int* ip, bool* snd, const String& v) {
  FRow& r = fAdd(FR_NUMSND, id, t); r.ip = ip; r.bp = snd; r.value = v;
}
void fTime(uint8_t id, const String& t, int* h, int* m) {
  FRow& r = fAdd(FR_TIME, id, t); r.ip = h; r.ip2 = m;
  char b[8]; sprintf(b, "%02d:%02d", *h, *m); r.value = b;
}
String fmtLocalDT(uint32_t ts) {
  if (ts < 1600000000UL) return L("girilmedi", "not set");
  time_t t = (time_t)ts + 3 * 3600;
  struct tm* tm = gmtime(&t);
  char b[24]; sprintf(b, "%02d.%02d.%04d %02d:%02d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900, tm->tm_hour, tm->tm_min);
  return String(b);
}
void fDT(uint8_t id, const String& t, uint32_t* ts) { FRow& r = fAdd(FR_DT, id, t); r.tp = ts; r.value = fmtLocalDT(*ts); }
String lifeLabel(int h) { return String(h / 24) + L(" gün ", " d ") + String(h % 24) + L(" sa", " h"); }
void fLife(uint8_t id, const String& t, int* hours) { FRow& r = fAdd(FR_LIFE, id, t); r.ip = hours; r.value = lifeLabel(*hours); }

String levelName3(int i) { return i == 0 ? L("Düşük", "Low") : (i == 1 ? L("Orta", "Medium") : L("Yüksek", "High")); }

// ---------------------------------------------------------------- form icerikleri
void formBuild(int id) {
  fRowN = 0;
  fHasSave = true;
  static const int v3[3] = {0, 1, 2};
  static const int v4[4] = {0, 1, 2, 3};
  const char* lv3[3] = {L("Düşük", "Low"), L("Orta", "Med"), L("Yüksek", "High")};
  const char* snd4[4] = {L("Sessiz", "Off"), L("Düşük", "Low"), L("Orta", "Med"), L("Yüksek", "High")};
  String un = unitLabel();
  switch (id) {
    case F_ALARMS: {
      fTitle = L("ALARMLAR", "ALARMS");
      fSeg(21, L("Alarm sesi", "Alarm sound"), &soundLevel, 4, snd4, v4);
      fAct(22, L("Sesi dene", "Test sound"), "");
      { String w = soundWarnText(); if (w.length()) fNote(w + L(" - bu durumda alarm sesi çalmaz.", " - alarm sound will not play."), sevColor(0)); }
      if (isNightHours()) fNote(L("Şu an gece modu saatinde: ses Gece modu ayarından gelir.", "Night hours now: sound comes from Night mode."), sevColor(3));
      else if (isSchoolHours()) fNote(L("Şu an okul saatinde: ses Okul modu ayarından gelir.", "School hours now: sound comes from School mode."), sevColor(3));
      fHead(L("ALARM TÜRLERİ", "ALARM TYPES"));
      fTog(1, L("Hızlı düşüş", "Fast fall"), &alarmFastFallOn, TP_H_FASTFALL);
      fTog(2, L("Hızlı yükseliş", "Fast rise"), &alarmFastRiseOn, TP_H_FASTRISE);
      fTog(3, L("Ses bastırma", "Suppress sound"), &recoverySoundSuppressOn, TP_H_SUPPRESS);
      fHead(L("GÖRSEL ALARM", "VISUAL ALARM"));
      fTog(4, L("Yanıp sönme", "Blinking"), &visualAlarmOn, TP_H_VISUAL);
      if (visualAlarmOn) { FRow& r = fAdd(FR_COLORS, 5, ""); r.ip = &alarmColorIdx; }
      fHead(L("ERTELE", "SNOOZE"));
      static const int sv[4] = {15, 30, 60, 0};
      const char* so[4] = {L("15 dk", "15 m"), L("30 dk", "30 m"), L("60 dk", "60 m"), L("Kapalı", "Off")};
      fSeg(6, L("Ertele süresi", "Snooze time"), &snoozeMinutes, 4, so, sv, TP_H_SNOOZE);
      break;
    }
    case F_TARGETS: {
      fTitle = L("HEDEFLER", "TARGETS");
      if (personCount > 1) fNote(String(L("Bu eşikler: ", "These limits: ")) + personLabel(dispCtx), TFT_CYAN);   // 4.0
      fNote(L("Değere dokun ve yaz (mg/dL). Sağdaki hoparlör o alarmın sesini açar/kapatır.",
              "Tap a value to type it (mg/dL). The speaker on the right turns that alarm's sound on/off."));
      { String w = soundWarnText(); if (w.length()) fNote(w + L(" - bu durumda alarm sesi çalmaz.", " - alarm sound will not play."), sevColor(0)); }
      fNumSnd(10, L("Kritik düşük", "Critical low"), &alarmCritLow, &alarmCritLowSound, formatGlucose(alarmCritLow) + " " + un);
      fNumSnd(11, L("Düşük", "Low"), &alarmLow, &alarmLowSound, formatGlucose(alarmLow) + " " + un);
      fNumSnd(12, L("Yüksek", "High"), &alarmHigh, &alarmHighSound, formatGlucose(alarmHigh) + " " + un);
      fNumSnd(13, L("Çok yüksek", "Very high"), &alarmVeryHigh, &alarmVeryHighSound, formatGlucose(alarmVeryHigh) + " " + un);
      fNumSnd(14, L("Veri yok", "No data"), &staleAlarmMin, &alarmStaleSound, String(staleAlarmMin) + L(" dk", " min"));
      break;
    }
    case F_DISPLAY: {
      fTitle = L("EKRAN / DİL", "DISPLAY / LANGUAGE");
      fSeg(20, L("Parlaklık", "Brightness"), &brightnessLevel, 3, lv3, v3);
          if (isNightHours()) fNote(L("Şu an gece modu saatinde: parlaklık Gece modu ayarından gelir.", "Night hours now: brightness comes from Night mode."), sevColor(3));
      else if (isSchoolHours()) fNote(L("Şu an okul saatinde: parlaklık Okul modu ayarından gelir.", "School hours now: brightness comes from School mode."), sevColor(3));
      static const int rv[2] = {1, 3};
      const char* ro[2] = {L("Normal", "Normal"), L("Ters", "Flipped")};
      fSeg(23, L("Ekran yönü", "Rotation"), &landRot, 2, ro, rv);
      fTog(24, L("Renk dostu", "Color-safe"), &colorBlindMode, TP_H_COLOR);
      fHead(L("DİL VE BİRİM", "LANGUAGE & UNIT"));
      static const int v2d[2] = {0, 1};
      const char* uod[2] = {"mg/dL", "mmol/L"};
      fAct(25, L("Dil", "Language"), LANG_NAMES[langIdx]);
      fSeg(26, L("Birim", "Unit"), &unitIdx, 2, uod, v2d);
      if (personCount > 1) {                          // 4.0: aile modu ana ekrani
        static const int fmv[2] = {0, 1};
        const char* fmo[2] = {L("Sırayla", "In turn"), L("Bölünmüş", "Split")};
        fHead(L("AİLE MODU", "FAMILY MODE"));
        fSeg(27, L("Ana ekran", "Home screen"), &famMode, 2, fmo, fmv);
        fNote(L("Kişi eklemek, ad ve kaynak vermek için telefondan web sayfasını açın.", "Add people, names and sources on the web page from a phone."));
      }
      break;
    }
    case F_LANG: {
      fTitle = L("DİL VE BİRİM", "LANGUAGE & UNIT");
      static const int v2[2] = {0, 1};
      const char* uo[2] = {"mg/dL", "mmol/L"};
      fAct(25, L("Dil", "Language"), LANG_NAMES[langIdx]);
      fSeg(26, L("Birim", "Unit"), &unitIdx, 2, uo, v2);
      break;
    }
    case F_NIGHT:
    case F_SCHOOL: {
      bool n = (id == F_NIGHT);
      fTitle = n ? L("GECE MODU", "NIGHT MODE") : L("OKUL MODU", "SCHOOL MODE");
      int b = n ? 30 : 40;
      fTog(b, n ? L("Gece modu", "Night mode") : L("Okul modu", "School mode"), n ? &nightModeOn : &schoolModeOn,
           n ? TP_H_NIGHT : TP_H_SCHOOL);
      fTime(b + 1, L("Başlangıç", "Start"), n ? &nightStartHour : &schoolStartHour, n ? &nightStartMin : &schoolStartMin);
      fTime(b + 2, L("Bitiş", "End"), n ? &nightEndHour : &schoolEndHour, n ? &nightEndMin : &schoolEndMin);
      fSeg(b + 3, L("Parlaklık", "Brightness"), n ? &nightBrightLevel : &schoolBrightLevel, 3, lv3, v3);
      fSeg(b + 4, L("Ses", "Sound"), n ? &nightSoundLevel : &schoolSoundLevel, 4, snd4, v4);
      fNote(L("Gece ve okul saatleri çakışırsa gece modu geçerli olur.",
              "If night and school hours overlap, night mode wins."));
      break;
    }
    case F_SLEEP: {
      fTitle = L("RAHATSIZ ETME", "DO NOT DISTURB");
      fTog(50, L("Rahatsız etme modu", "Do not disturb"), &sleepModeOn, TP_H_SLEEP);
      fTog(51, L("Şıklatma ile aç", "Snap to wake"), &micSnapOn, TP_H_MIC);
      fSeg(52, L("Şıklatma hassasiyeti", "Snap sensitivity"), &snapSens, 3, lv3, v3);
      fAct(53, L("Şıklatma testi", "Snap test"), "");
      fNote(L("Şeker hedef dışındaysa, hızlı değişiyorsa veya veri gelmiyorsa ekran kapanmaz.",
              "The screen stays on if glucose is out of range, changing fast, or data is missing."));
      break;
    }
    case F_RESET: {
      fTitle = L("SIFIRLA / KAPAT", "RESET / POWER");
      fHasSave = false;
      fAct(60, L("Veriyi şimdi yenile", "Refresh data now"), "");
      fAct(61, L("Cihazı sıfırla", "Reset device"), "", 0, sevColor(1));
      fNote(L("Cihazda kayıtlı her şey silinir: WiFi ağları, hesap girişleri, tüm ayarlar, fotoğraflar ve karb listesi. Web şifresi ilk (fabrika) şifreye döner, kurulum ekranı açılır.",
              "Everything stored on the device is erased: WiFi networks, logins, all settings, photos and the carb list. The web password returns to the original (factory) password and setup opens."));
      fAct(63, L("WiFi kurulumu", "WiFi setup"), "");
      fNote(L("Kayıtlı ağlar ve ayarlar silinmez. Kurulum sayfası açılır; yeni ağı ekleyebilir veya şifreyi değiştirebilirsiniz.",
              "Saved networks and settings are kept. Setup opens so you can add a network or change its password."));
      fAct(62, L("Cihazı kapat", "Power off"), "", 0, sevColor(0));
      fNote(L("Tekrar açmak için RESET tuşuna basın veya USB'yi takıp çıkarın.", "Press RESET or re-plug USB to turn it back on."));
      break;
    }
    case F_PREVIEW: {
      fTitle = L("ÖNİZLEMELER", "PREVIEWS");
      fHasSave = false;
      fAct(65, L("Glukagon Videosu Önizle", "Preview glucagon video"), "");
      fNote(L("Dikkat butonundaki glukagon videosu oynatılır.", "Plays the glucagon video from the Attention button."));
      fAct(66, L("Dikkat Butonu Önizle", "Preview Attention button"), L("Düşük", "Low"));
      fAct(67, L("Dikkat Butonu Önizle", "Preview Attention button"), L("Yüksek", "High"));
      fNote(L("Öğretmenin göreceği adımlar örnek bir şeker değeriyle gösterilir. Gerçek değer, alarmlar ve ayarlar etkilenmez.",
              "Shows the teacher's steps with a sample glucose value. The real value, alarms and settings are not affected."));
      break;
    }
    case F_MODES: {
      fTitle = L("MODLAR", "MODES");
      fHasSave = false;
      fAct(70, L("Gece modu", "Night mode"), onOffLabel(nightModeOn), TP_H_NIGHT);
      fAct(71, L("Okul modu", "School mode"), onOffLabel(schoolModeOn), TP_H_SCHOOL);
      fAct(72, L("Rahatsız etme modu", "Do not disturb"), onOffLabel(sleepModeOn), TP_H_SLEEP);
      fTog(73, L("Rehber modu", "Guide mode"), &teacherModeOn, TP_REHBER);
      fAct(74, L("Aile modu", "Family mode"), famEnabled ? (String(L("Açık", "On")) + " - " + String(famCount)) : onOffLabel(false));   // 4.0.1
      fNote(L("Gece ve okul saatleri çakışırsa gece modu geçerli olur. Rahatsız etme modu diğer modlarla birlikte çalışır; şeker hedef dışına çıkınca ekran kendiliğinden açılır.",
              "If night and school hours overlap, night mode wins. Sleep mode works together with the others; the screen wakes by itself when glucose leaves the range."));
      break;
    }
    case F_SENSOR: {
      fTitle = L("SENSÖR / SET", "SENSOR / SET");
      bool ns = (dataSource == "nightscout");
      if (personCount > 1) fNote(String(L("Bu ayarlar: ", "These settings: ")) + personLabel(dispCtx), TFT_CYAN);   // 4.0
      fHead(L("SENSÖR", "SENSOR"));
      fTog(80, L("Sensör takibi", "Sensor tracking"), &sensorTrackOn);
      if (sensorTrackOn) {
        if (!ns) {
          fDT(81, L("Takılma", "Inserted"), &sensInsTs);
          fAct(82, L("Şimdi taktım", "Inserted now"), "");
        }
        fLife(83, L("Sensör ömrü", "Sensor life"), &sensorLifeHours);
      }
      fHead(L("SET / POMPA", "SET / PUMP"));
      fTog(85, L("Set takibi", "Set tracking"), &cannulaTrackOn);
      if (cannulaTrackOn) {
        if (!ns) {
          fDT(86, L("Değişim", "Changed"), &setInsTs);
          fAct(87, L("Şimdi değiştirdim", "Changed now"), "");
        }
        fLife(88, L("Set / pompa ömrü", "Set / pump life"), &cannulaLifeHours);
      }
      if (ns) fNote(L("Nightscout kullandığınız için geçen süre otomatik gelir; burada sadece ömrü (gün + saat) girersiniz. Kalan süre = ömür - geçen süre. Varsayılan: sensör 10 gün 12 saat, set 3 gün 8 saat.",
                      "With Nightscout the elapsed time comes automatically; here you only enter the life (days + hours). Remaining = life - elapsed. Default: sensor 10 d 12 h, set 3 d 8 h."));
      else fNote(L("Kalan süre = ömür - takılmadan beri geçen süre. Ömre dokunup gün ve saat yazın (varsayılan: sensör 10 gün 12 saat, set 3 gün 8 saat). Tarih ve saati web sayfasından da girebilirsiniz.",
                   "Remaining = life - time since insertion. Tap a life to type days and hours (default: sensor 10 d 12 h, set 3 d 8 h). You can also set the date and time on the web page."));
      break;
    }
    case F_PAGES: {
      fTitle = L("SAYFALARI DÜZENLE", "EDIT PAGES");
      fHasSave = false;                                  // her dugme aninda kaydedilir
      fNote(String(L("Açtığınız her özellik cihaza ayrı bir sayfa olarak eklenir; kaydırarak gezersiniz. 1. sayfa ana temanız: ", "Each item you turn on becomes its own page; swipe to move between them. Page 1 is your main theme: ")) + themeName(homeTheme) + ".");
      fTog(90, L("2. tema", "2nd theme"), &pgOn[P_TH2]);
      if (pgOn[P_TH2]) fAct(91, L("   2. sayfanın teması", "   Page 2 theme"), themeName(pgTh2Theme));
      fTog(92, L("3. tema", "3rd theme"), &pgOn[P_TH3]);
      if (pgOn[P_TH3]) fAct(93, L("   3. sayfanın teması", "   Page 3 theme"), themeName(pgTh3Theme));
      fTog(94, L("Sensör / set", "Sensor / set"), &pgOn[P_SENSOR]);
      fTog(95, L("Grafik", "Graph"), &pgOn[P_GRAPH]);
      fTog(96, L("İstatistik", "Stats"), &pgOn[P_STATS]);
      fTog(97, L("Zamanlayıcı", "Timer"), &pgOn[P_TIMER]);
      fTog(98, L("Karb-Not", "Carbs"), &pgOn[P_KARB]);
      fNote(L("Sıra: ana tema, 2. tema, 3. tema, sensör / set, grafik, istatistik, zamanlayıcı, Karb-Not ve en sonda uygulamalar. Kapalı olanlar sayfa olmaz ama uygulamalar sayfasında durur.",
              "Order: main theme, 2nd theme, 3rd theme, sensor / set, graph, stats, timer, carbs and the apps page last. Items turned off are not pages but stay on the apps page."));
      break;
    }
  }
  // yukseklikler
  int y = 4;
  cv->setFont(&FreeSansTR7pt);
  for (int i = 0; i < fRowN; i++) {
    FRow& r = fRows[i];
    r.y = y;
    switch (r.type) {
      case FR_HEAD: r.h = 28; break;
      case FR_SEG: r.h = r.label.length() ? 72 : 46; break;
      case FR_COLORS: r.h = 56; break;
      case FR_NOTE: {
        // satir sayisini hesapla
        int lines = 0; String rest = r.label;
        while (rest.length()) {
          int fit = rest.length();
          while (fit > 0 && cv->textWidth(rest.substring(0, fit).c_str()) > 288) {
            int sp = rest.lastIndexOf(' ', fit - 1);
            fit = (sp > 0) ? sp : fit - 1;
          }
          if (fit <= 0) fit = 1;
          rest = rest.substring(fit); rest.trim(); lines++;
        }
        r.h = lines * 18 + 10;
        break;
      }
      default: r.h = 50; break;
    }
    y += r.h;
  }
  cv->setTextFont(1);
  fContentH = y + 6;
}

// ---------------------------------------------------------------- anlik kopya / geri al
void fSnapAddI(int* p) { if (!p) return; for (int i = 0; i < fSnapN; i++) if (fSnap[i].ip == p) return; if (fSnapN < 40) { fSnap[fSnapN] = {p, nullptr, nullptr, *p, false, 0}; fSnapN++; } }
void fSnapAddB(bool* p) { if (!p) return; for (int i = 0; i < fSnapN; i++) if (fSnap[i].bp == p) return; if (fSnapN < 40) { fSnap[fSnapN] = {nullptr, p, nullptr, 0, *p, 0}; fSnapN++; } }
void fSnapAddT(uint32_t* p) { if (!p) return; for (int i = 0; i < fSnapN; i++) if (fSnap[i].tp == p) return; if (fSnapN < 40) { fSnap[fSnapN] = {nullptr, nullptr, p, 0, false, *p}; fSnapN++; } }

void formSnapshot() {
  fSnapN = 0;
  formBuild(formId);
  for (int i = 0; i < fRowN; i++) { fSnapAddI(fRows[i].ip); fSnapAddI(fRows[i].ip2); fSnapAddB(fRows[i].bp); fSnapAddT(fRows[i].tp); }
  // gizli satirlarin baglandigi degiskenler de (ornegin gorsel alarm kapaliyken renk)
  if (formId == F_ALARMS) fSnapAddI(&alarmColorIdx);
  if (formId == F_DISPLAY || formId == F_LANG) fSnapAddI(&langIdx);   // 3.6.1: dil de Kaydet'e bagli (Geri -> eski dil)
  if (formId == F_SENSOR) { fSnapAddT(&sensInsTs); fSnapAddT(&setInsTs); fSnapAddI(&sensorLifeHours); fSnapAddI(&cannulaLifeHours); }
}
bool formIsDirty() {
  for (int i = 0; i < fSnapN; i++) {
    if (fSnap[i].ip && *fSnap[i].ip != fSnap[i].iv) return true;
    if (fSnap[i].bp && *fSnap[i].bp != fSnap[i].bv) return true;
    if (fSnap[i].tp && *fSnap[i].tp != fSnap[i].tv) return true;
  }
  return false;
}
void formLive() {
  applyBrightness();
  applyRotation();
}
void formRevert() {
  for (int i = 0; i < fSnapN; i++) {
    if (fSnap[i].ip) *fSnap[i].ip = fSnap[i].iv;
    if (fSnap[i].bp) *fSnap[i].bp = fSnap[i].bv;
    if (fSnap[i].tp) *fSnap[i].tp = fSnap[i].tv;
  }
  formLive();
}

// ---------------------------------------------------------------- cizim
void drawSegCtl(lgfx::LovyanGFX* g, int y, const FRow& r) {
  int x0 = 8, w = 296, h = 36;
  g->fillRoundRect(x0, y, w, h, 10, 0x2124);
  int sel = -1;
  for (int i = 0; i < r.nOpt; i++) if (r.ip && *r.ip == r.ov[i]) sel = i;
  for (int i = 0; i < r.nOpt; i++) {
    int sx = x0 + (w * i) / r.nOpt, ex = x0 + (w * (i + 1)) / r.nOpt;
    if (i == sel) g->fillRoundRect(sx + 3, y + 3, ex - sx - 6, h - 6, 8, 0x0B5E);
    else if (i > 0 && i - 1 != sel) g->drawFastVLine(sx, y + 9, h - 18, 0x4208);
    g->setFont(&FreeSansTR7pt);
    g->setTextColor(i == sel ? (uint16_t)TFT_WHITE : UI_SUBTEXT);
    g->setTextDatum(MC_DATUM);
    g->drawString(r.opt[i], (sx + ex) / 2, y + h / 2 + 1);
  }
}

void drawToggleG(lgfx::LovyanGFX* g, int tx, int ty, bool on) {
  g->fillRoundRect(tx, ty, 48, 24, 12, on ? UI_TOG_ON : UI_TOG_OFF);
  g->fillCircle(on ? tx + 36 : tx + 12, ty + 12, 9, TFT_WHITE);
}

void drawFormBody() {
  bool tmpKey = bgFrameOn();
  if (tmpKey) uiBg = HOME_KEY;
  formBuild(formId);
  int maxS = fContentH > LIST_H ? fContentH - LIST_H : 0;
  if (formScroll < 0) formScroll = 0;
  if (formScroll > maxS) formScroll = maxS;
  int oy;
  lgfx::LovyanGFX* g = listTarget(oy);
  lgfx::LovyanGFX* savedCv = cv;
  cv = g;
  g->fillRect(0, oy, 320, LIST_H, UI_BG);
  for (int i = 0; i < fRowN; i++) {
    FRow& r = fRows[i];
    int y = oy + r.y - formScroll;
    r.infoX = -100;
    if (y + r.h < oy || y > oy + LIST_H) continue;
    switch (r.type) {
      case FR_HEAD:
        g->setFont(&FreeSansTR6pt);
        g->setTextColor(UI_ACCENT);
        g->setTextDatum(BL_DATUM);
        g->drawString(r.label.c_str(), 14, y + r.h - 5);
        break;
      case FR_NOTE: {
        g->setFont(&FreeSansTR7pt);
        g->setTextColor(r.col);
        g->setTextDatum(TL_DATUM);
        String rest = r.label; int ly = y + 4;
        while (rest.length()) {
          int fit = rest.length();
          while (fit > 0 && g->textWidth(rest.substring(0, fit).c_str()) > 288) {
            int sp = rest.lastIndexOf(' ', fit - 1);
            fit = (sp > 0) ? sp : fit - 1;
          }
          if (fit <= 0) fit = 1;
          g->drawString(rest.substring(0, fit).c_str(), 14, ly);
          rest = rest.substring(fit); rest.trim(); ly += 18;
        }
        break;
      }
      case FR_SEG: {
        int cy = y;
        if (r.label.length()) {
          g->setFont(&FreeSansTR9pt);
          g->setTextColor(TFT_WHITE);
          g->setTextDatum(ML_DATUM);
          g->drawString(r.label.c_str(), 14, y + 14);
          if (r.help) { int ix = 14 + g->textWidth(r.label.c_str()) + 16; drawInfoBadge(g, ix, y + 14); r.infoX = ix; }
          cy = y + 30;
        }
        drawSegCtl(g, cy, r);
        break;
      }
      case FR_COLORS: {
        for (int k = 0; k < 6; k++) {
          int x = 8 + k * 50;
          bool sel = r.ip && *r.ip == k;
          g->fillRoundRect(x + 2, y + 6, 44, 40, 10, colorVals[k]);
          if (sel) { g->drawRoundRect(x, y + 4, 48, 44, 12, TFT_WHITE); g->drawRoundRect(x - 1, y + 3, 50, 46, 13, TFT_WHITE); }
        }
        break;
      }
      default: {
        g->fillRoundRect(8, y, 296, 44, 10, UI_CARD);
        g->setFont(&FreeSansTR9pt);
        g->setTextColor(r.col);
        g->setTextDatum(ML_DATUM);
        g->drawString(r.label.c_str(), 18, y + 23);
        int lw = g->textWidth(r.label.c_str());
        if (r.help) { int ix = 18 + lw + 16; drawInfoBadge(g, ix, y + 22); r.infoX = ix; }
        if (r.type == FR_TOG) {
          drawToggleG(g, 248, y + 10, r.bp && *r.bp);
        } else if (r.type == FR_NUMSND) {
          g->setTextColor(UI_ACCENT);
          g->setTextDatum(MR_DATUM);
          g->drawString(r.value.c_str(), 250, y + 23);
          bool on = r.bp && *r.bp;
          g->fillRoundRect(258, y + 6, 40, 32, 8, on ? (uint16_t)0x2A69 : (uint16_t)0x2104);
          drawSpeakerIcon(276, y + 22, on ? (uint16_t)TFT_WHITE : (uint16_t)0x7BEF, on);
        } else if (r.type == FR_TIME || r.type == FR_DT || r.type == FR_LIFE) {
          g->setFont(r.type == FR_DT ? &FreeSansTR7pt : &FreeSansTR9pt);
          g->setTextColor(UI_ACCENT);
          g->setTextDatum(MR_DATUM);
          g->drawString(r.value.c_str(), 294, y + 23);
        } else {   // FR_ACT
          int cx = 290, cy = y + 22;
          for (int t = 0; t < 2; t++) {
            g->drawLine(cx - 3 + t, cy - 6, cx + 3 + t, cy, UI_SUBTEXT);
            g->drawLine(cx + 3 + t, cy, cx - 3 + t, cy + 6, UI_SUBTEXT);
          }
          if (r.value.length()) {
            g->setFont(&FreeSansTR7pt);
            g->setTextColor(UI_SUBTEXT);
            g->setTextDatum(MR_DATUM);
            g->drawString(r.value.c_str(), 278, cy + 1);
          }
        }
        break;
      }
    }
  }
  drawScrollbar(g, oy, formScroll, maxS, fContentH);
  g->setTextDatum(TL_DATUM);
  cv = savedCv;
  listFlush();
  if (tmpKey) uiBg = 0;
}

void drawFormDialog() {
  const char* msg; const char* b1; const char* b2; uint16_t c2 = 0x0B5E;
  if (formDlg == 1) { msg = L("Değişiklikler kaydedilsin mi?", "Save your changes?"); b1 = L("Kaydetme", "Discard"); b2 = L("Kaydet", "Save"); }
  else if (formDlg == 2) { msg = L("Cihaz sıfırlansın mı?", "Reset the device?"); b1 = L("Vazgeç", "Cancel"); b2 = L("Sıfırla", "Reset"); c2 = 0xA800; }
  else if (formDlg == 4) { msg = L("WiFi kurulumu açılsın mı?", "Open WiFi setup?"); b1 = L("Vazgeç", "Cancel"); b2 = L("Aç", "Open"); }
  else { msg = L("Cihaz kapatılsın mı?", "Power off the device?"); b1 = L("Vazgeç", "Cancel"); b2 = L("Kapat", "Power off"); c2 = 0xA800; }
  cv->fillRoundRect(20, 62, 280, 124, 16, 0x2945);
  cv->drawRoundRect(20, 62, 280, 124, 16, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(msg, 160, 96, &FreeSansTR9pt);
  cv->fillRoundRect(34, 130, 118, 42, 12, 0x4208);
  cv->setTextColor(TFT_WHITE, 0x4208);
  drawFreeTitle(b1, 93, 151, &FreeSansTR7pt);
  cv->fillRoundRect(168, 130, 118, 42, 12, c2);
  cv->setTextColor(TFT_WHITE, c2);
  drawFreeTitle(b2, 227, 151, &FreeSansTR7pt);
  cv->setTextDatum(TL_DATUM);
}

void drawForm() {
  formBuild(formId);
  formDirty = fHasSave && formIsDirty();
  drawFormHeader(fTitle.c_str(), fHasSave);
  drawFormBody();
  if (formDlg) drawFormDialog();
}

// ---------------------------------------------------------------- acma / kapama / kaydetme
void formOpen(int id) {
  if (formId) {
    if (fHasSave && formIsDirty()) saveSettings();      // ust sayfadaki degisiklikleri kaybetme
    if (formDepth < 4) formStack[formDepth++] = formId;
  }
  formId = id; formScroll = 0; formDlg = 0; fPressed = -1;
  textPageActive = false; timePickerActive = false;
  formSnapshot();
  drawCurrentScreen();
}

void txtButtonTap(int act) {
  if (act == 1) {
    previewFromInfo = true; previewInfoScroll = txtScroll; previewRetScreen = currentScreen;
    formId = 0; formDepth = 0; formDlg = 0;
    formOpen(F_PREVIEW);
  }
}

void openPreviewMenu() {
  teacherPreview = 0; previewVideo = false;
  teacherStep = 0; teacherGlucagonPage = 0;
  currentScreen = previewRetScreen;
  applyRotation();
  formId = 0; formDepth = 0; formDlg = 0; textPageActive = false; timePickerActive = false;
  formOpen(F_PREVIEW);
}

void startTeacherPreview(int k) {
  formId = 0; formDepth = 0; formDlg = 0; textPageActive = false; timePickerActive = false;
  teacherPreview = k;
  teacherConscious = true; teacherGlucagonPage = 0; teacherFingerOk = true;
  teacherCat = 0; teacherBack = 0;
  teacherStep = 1;
  currentScreen = 0;
  applyRotation();
  drawCurrentScreen();
}

void formClose() {
  if (formId == F_PREVIEW && formDepth == 0 && previewFromInfo) {   // 3.4.6: Bilgi sayfasina geri don
    previewFromInfo = false; formId = 0; formDlg = 0; timePickerActive = false;
    textPageId = TP_INFO; buildTextPage(TP_INFO); textPageActive = true; txtScroll = previewInfoScroll;
    drawCurrentScreen();
    return;
  }
  formDlg = 0;
  textPageActive = false; timePickerActive = false;
  if (formDepth > 0) { formId = formStack[--formDepth]; formScroll = 0; formSnapshot(); }
  else formId = 0;
  drawCurrentScreen();
}

bool formValidate() {
  if (formId == F_TARGETS) {
    if (alarmCritLow < 40) alarmCritLow = 40;
    if (alarmLow < alarmCritLow + 5) alarmLow = alarmCritLow + 5;
    if (alarmHigh < alarmLow + 10) alarmHigh = alarmLow + 10;
    if (alarmVeryHigh < alarmHigh + 10) alarmVeryHigh = alarmHigh + 10;
    if (alarmVeryHigh > 400) alarmVeryHigh = 400;
  }
  return true;
}

void formSave() {
  if (!formValidate()) return;
  saveSettings();
  formLive();
  if (formId == F_SENSOR) { computeManualAges(); checkAgeWarnings(); }
  if (formId == F_SLEEP) lastUserTouchMs = millis();
  cv->fillRoundRect(90, 100, 140, 40, 20, 0x2A69);
  cv->setTextColor(TFT_WHITE, 0x2A69);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Kaydedildi", "Saved"), 160, 120, &FreeSansTR9pt);
  cv->setTextDatum(TL_DATUM);
  delay(450);
  fSnapN = 0;
  formClose();
}

void formBack() {
  if (fHasSave && formIsDirty()) { formDlg = 1; drawCurrentScreen(); return; }
  formClose();
}

// numara araliklari (hedefler)
void formOpenNumber(const FRow& r) {
  switch (r.id) {
    case 10: openNumberPicker(L("KRİTİK DÜŞÜK", "CRITICAL LOW"), r.ip, 40, alarmLow - 5, "mg/dL"); break;
    case 11: { int mn = (alarmCritLow + 5 > 40) ? alarmCritLow + 5 : 40;
               openNumberPicker(L("DÜŞÜK EŞİK", "LOW LIMIT"), r.ip, mn, alarmHigh - 10, "mg/dL"); break; }
    case 12: openNumberPicker(L("YÜKSEK EŞİK", "HIGH LIMIT"), r.ip, alarmLow + 10, alarmVeryHigh - 10, "mg/dL"); break;
    case 13: openNumberPicker(L("ÇOK YÜKSEK EŞİK", "VERY HIGH LIMIT"), r.ip, alarmHigh + 10, 400, "mg/dL"); break;
    case 14: openNumberPicker(L("VERİ YOK ALARMI", "NO-DATA ALARM"), r.ip, 5, 60, L("dk", "min")); break;
  }
}

void formAction(int id) {
  switch (id) {
    case 25: openLangPicker(); drawCurrentScreen(); return;                   // 3.6.0: dil secimi
    case 74:                                                                     // 4.0.1: aile modu QR sayfasi
      famQrFrom = currentScreen;
      formId = 0; formDepth = 0; formDlg = 0;
      currentScreen = SCR_SETTINGS; subScreen = 31;
      drawCurrentScreen();
      return;
    case 65: previewVideo = true; openVideo(currentScreen, false); return;     // 3.4.6
    case 66: startTeacherPreview(1); return;
    case 67: startTeacherPreview(2); return;
    case 22: playTestBeep(); return;
    case 53:
      if (formIsDirty()) saveSettings();
      formId = 0; formDepth = 0; snapSingles = 0; subScreen = 21;
      micTestFrom = currentScreen; currentScreen = SCR_SETTINGS;   // 3.4.3
      drawCurrentScreen();
      return;
    case 60:
      cv->fillRoundRect(90, 100, 140, 40, 20, 0x2945);
      cv->setTextColor(TFT_WHITE, 0x2945); cv->setTextDatum(MC_DATUM);
      drawFreeTitle(L("Yenileniyor...", "Refreshing..."), 160, 120, &FreeSansTR7pt);
      cv->setTextDatum(TL_DATUM);
      requestFetch();
      delay(400);
      break;
    case 61: formDlg = 2; break;
    case 62: formDlg = 3; break;
    case 63: formDlg = 4; break;
    case 70: formOpen(F_NIGHT); return;
    case 71: formOpen(F_SCHOOL); return;
    case 72: formOpen(F_SLEEP); return;
    case 82: sensInsTs = (uint32_t)time(nullptr); break;
    case 91: openThemePick(2); return;
    case 93: openThemePick(3); return;
    case 87: setInsTs = (uint32_t)time(nullptr); break;
  }
  drawCurrentScreen();
}

void doPowerOff() {
  cv->fillScreen(UI_BG);
  cv->setTextColor(sevColor(1), UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Kapatılıyor...", "Shutting down..."), 160, 120, &FreeSansTR9pt);
  stopSpeaker();
  localHistSaveAll();                     // 3.4.15 / 4.0: tum kisilerin kaydi
  delay(800);
  cv->fillScreen(TFT_BLACK);
  ledcDetach(BACKLIGHT_PIN);
  pinMode(BACKLIGHT_PIN, OUTPUT); digitalWrite(BACKLIGHT_PIN, LOW);
  digitalWrite(AMP_CTRL_PIN, HIGH);
  gpio_hold_en((gpio_num_t)BACKLIGHT_PIN);
  gpio_hold_en((gpio_num_t)AMP_CTRL_PIN);
  gpio_deep_sleep_hold_en();
  tft.sleep();
  inaPowerDown();
  esp_deep_sleep_start();
}

// 3.4.0 "Cihazi sifirla": cihazda kayitli her sey silinir (WiFi, hesaplar, tum ayarlar,
// fotograflar, karb listesi, sayaclar). Ayar hafizasi (NVS) bolumu tamamen silinir,
// boylece eski bilgi flash'ta da kalmaz. Korunanlar: ilk (fabrika) web sifresi
// ve cihaza yuklenen glukagon videosu.
void fullFactoryWipe() {
  prefs.begin("metematik", true);
  String pw0 = prefs.getString("webpw0", "");
  prefs.end();
  if (pw0.length() < 6) pw0 = webFactoryPass();
  if (SPIFFS.begin(true)) {
    String del[40]; int nd = 0;
    File root = SPIFFS.open("/");
    if (root) {
      File f = root.openNextFile();
      while (f && nd < 40) {
        String p = String(f.path());
        if (!p.startsWith("/")) p = "/" + p;
        f.close();
        if (p != VIDEO_PATH && p != "/logo.rgb") del[nd++] = p;
        f = root.openNextFile();
      }
      root.close();
    }
    for (int i = 0; i < nd; i++) SPIFFS.remove(del[i]);
  }
  fotoCount = 0; fotoActiveIdx = 0; fotoLoaded = false;
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  bool erased = (nvs_flash_deinit() == ESP_OK) && (nvs_flash_erase() == ESP_OK);
  nvs_flash_init();
  if (!erased) {                        // 4.0: aile modu kisileri de silinsin
    Preferences pp;
    pp.begin("mmp1", false); pp.clear(); pp.end();
    pp.begin("mmp2", false); pp.clear(); pp.end();
  }
  prefs.begin("metematik", false);
  if (!erased) prefs.clear();
  prefs.putString("webpw0", pw0);
  prefs.putString("webpw", pw0);
  prefs.end();
}

void doDeviceReset() {
  formId = 0; formDepth = 0; formDlg = 0;
  subScreen = 0;
  stopSpeaker();
  forceLandscape();
  cv = &tft;
  cv->fillScreen(UI_BG);
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Siliniyor...", "Erasing..."), 160, 120, &FreeSansTR9pt);
  cv->setTextDatum(TL_DATUM);
  fullFactoryWipe();
  // restartIntoPortal() KULLANMA: saveSettings() RAM'deki eski bilgileri geri yazardi
  portalOnBoot = PORTAL_MAGIC;
  delay(600);
  ESP.restart();
}

// Diyalog dokunuslari (genel dokunma isleyicisinden)
void formDialogTap(int x, int y) {
  if (y < 128 || y > 176) { if (formDlg == 1 && (y < 62 || y > 186)) { formDlg = 0; drawCurrentScreen(); } return; }
  bool right = (x >= 160);
  int d = formDlg;
  formDlg = 0;
  if (d == 1) { if (right) formSave(); else { formRevert(); formClose(); } return; }
  if (!right) { drawCurrentScreen(); return; }
  if (d == 2) doDeviceReset();
  else if (d == 3) doPowerOff();
  else if (d == 4) doWifiSetup();
}

// Liste mekanizmasindan gelen kisa dokunus (y ekran koordinati)
void formTap(int x, int y) {
  if (y < LIST_Y) {
    if (x < 100) formBack();
    else if (x > 236 && fHasSave) formSave();
    return;
  }
  // satirlar son cizimden (drawFormBody) guncel; yeniden kurmak (i) konumlarini silerdi
  int ry = y - LIST_Y + formScroll;
  int idx = -1;
  for (int i = 0; i < fRowN; i++) if (ry >= fRows[i].y && ry < fRows[i].y + fRows[i].h) { idx = i; break; }
  if (idx < 0) return;
  FRow& r = fRows[idx];
  if (r.help && r.infoX > 0 && abs(x - r.infoX) <= 16 && ry - r.y < 44) { openTextPage(r.help); return; }
  bool changed = false;
  switch (r.type) {
    case FR_SEG: {
      int cy = r.label.length() ? r.y + 30 : r.y;
      if (ry < cy || x < 8 || x > 304 || !r.ip) return;
      int k = ((x - 8) * r.nOpt) / 296;
      if (k < 0) k = 0;
      if (k >= r.nOpt) k = r.nOpt - 1;
      if (*r.ip != r.ov[k]) { *r.ip = r.ov[k]; changed = true; }
      if (r.id == 21 && changed) playTestBeep();
      break;
    }
    case FR_TOG:
      if (r.bp) { *r.bp = !*r.bp; changed = true; }
      if (!fHasSave) saveSettings();
      break;
    case FR_COLORS: {
      int k = (x - 8) / 50;
      if (k >= 0 && k < 6 && r.ip) { *r.ip = k; visualAlarmOn = true; changed = true; }
      break;
    }
    case FR_NUMSND:
      if (x >= 256) { if (r.bp) { *r.bp = !*r.bp; changed = true; } }
      else { formOpenNumber(r); drawCurrentScreen(); return; }
      break;
    case FR_TIME:
      openTimePicker(r.label.c_str(), r.ip, r.ip2);
      drawCurrentScreen();
      return;
    case FR_DT:
      openDateTimePicker(r.label.c_str(), r.tp);
      drawCurrentScreen();
      return;
    case FR_LIFE:
      openLifePicker(r.label.c_str(), r.ip, r.id == 83 ? 30 : 14);
      drawCurrentScreen();
      return;
    case FR_ACT:
      formAction(r.id);
      return;
    default: return;
  }
  if (changed) {
    formLive();
    if (r.id == 23 || r.id == 25) { drawCurrentScreen(); return; }   // yon / dil: tum ekran
  }
  drawCurrentScreen();
}

// ---------------------------------------------------------------- tarih + saat tus takimi (tpMode 2)
uint32_t* tpTs = nullptr;

uint32_t civilToUnixLocal(int y, int m, int d, int hh, int mi) {
  y -= (m <= 2);
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (uint32_t)(days * 86400L + hh * 3600L + mi * 60L - 3L * 3600L);
}

void openDateTimePicker(const char* title, uint32_t* ts) {
  tpMode = 2;
  tpTitle = title; tpTs = ts;
  tpLen = 0; memset(tpDig, 0, sizeof(tpDig)); tpErrUntil = 0;
  timePickerActive = true;
}

// 8 hane: GG AA SS DD (yil = bu yil; gelecekte kaliyorsa gecen yil)
void dtpDisplay(char* out) {
  if (tpLen == 0) {
    String s = (tpTs && *tpTs > 1600000000UL) ? fmtLocalDT(*tpTs) : String("GG.AA SS:DD");
    strcpy(out, s.c_str());
    return;
  }
  char d[8];
  for (int i = 0; i < 8; i++) d[i] = (i < tpLen) ? tpDig[i] : '_';
  sprintf(out, "%c%c.%c%c  %c%c:%c%c", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}

bool dtpTap(int k) {   // true: kapandi
  if (k == 3) { if (tpLen > 0) tpDig[--tpLen] = 0; return false; }
  if (k == 11) {
    if (tpLen == 0) { timePickerActive = false; return true; }
    if (tpLen < 8) { tpErrUntil = millis() + 1500; return false; }
    int dd = (tpDig[0] - '0') * 10 + (tpDig[1] - '0');
    int mo = (tpDig[2] - '0') * 10 + (tpDig[3] - '0');
    int hh = (tpDig[4] - '0') * 10 + (tpDig[5] - '0');
    int mi = (tpDig[6] - '0') * 10 + (tpDig[7] - '0');
    static const int md[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mo < 1 || mo > 12 || dd < 1 || dd > md[mo] || hh > 23 || mi > 59) {
      tpErrUntil = millis() + 2000; tpLen = 0; memset(tpDig, 0, sizeof(tpDig)); return false;
    }
    time_t now = time(nullptr);
    time_t loc = now + 3 * 3600;
    struct tm* tm = gmtime(&loc);
    int yr = (now > 1600000000) ? tm->tm_year + 1900 : 2026;
    uint32_t ts = civilToUnixLocal(yr, mo, dd, hh, mi);
    if (now > 1600000000 && (long)ts > (long)now + 3600) { yr--; ts = civilToUnixLocal(yr, mo, dd, hh, mi); }
    bool leap = (yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0);
    if (mo == 2 && dd == 29 && !leap) { tpErrUntil = millis() + 2000; tpLen = 0; memset(tpDig, 0, sizeof(tpDig)); return false; }
    if (tpTs) *tpTs = ts;
    timePickerActive = false;
    return true;
  }
  if (tpLen >= 8) return false;
  char c = TP_KEYS[k][0];
  tpDig[tpLen++] = c;
  tpDig[tpLen] = 0;
  return false;
}

void computeManualAges() {
  if (dataSource == "nightscout") return;
  uint32_t now = (uint32_t)time(nullptr);
  if (now < 1600000000UL) { sensorAgeHours = -1; cannulaAgeHours = -1; return; }
  sensorAgeHours = (sensInsTs > 1600000000UL && now >= sensInsTs) ? (now - sensInsTs) / 3600.0f : -1;
  cannulaAgeHours = (setInsTs > 1600000000UL && now >= setInsTs) ? (now - setInsTs) / 3600.0f : -1;
}

// ---------------------------------------------------------------- omur tus takimi (tpMode 3): gun + saat
int* tpLife = nullptr;
int tpLifeMaxD = 30;
void openLifePicker(const char* title, int* hours, int maxDays) {
  tpMode = 3;
  tpTitle = title; tpLife = hours; tpLifeMaxD = maxDays;
  tpLen = 0; memset(tpDig, 0, sizeof(tpDig)); tpErrUntil = 0;
  timePickerActive = true;
}
void lifeDisplay(char* out) {
  if (tpLen == 0) { strcpy(out, tpLife ? lifeLabel(*tpLife).c_str() : ""); return; }
  char d[4];
  for (int i = 0; i < 4; i++) d[i] = (i < tpLen) ? tpDig[i] : '_';
  sprintf(out, "%c%c %s %c%c %s", d[0], d[1], L("gün", "d"), d[2], d[3], L("sa", "h"));
}
void lifeTap(int k) {
  if (k == 3) { if (tpLen > 0) tpDig[--tpLen] = 0; return; }
  if (k == 11) {
    if (tpLen == 0) { timePickerActive = false; return; }
    if (tpLen != 2 && tpLen != 4) { tpErrUntil = millis() + 1500; return; }
    int dd = (tpDig[0] - '0') * 10 + (tpDig[1] - '0');
    int hh = (tpLen == 4) ? (tpDig[2] - '0') * 10 + (tpDig[3] - '0') : 0;
    if (dd > tpLifeMaxD || hh > 23 || dd * 24 + hh < 1) { tpErrUntil = millis() + 2000; tpLen = 0; memset(tpDig, 0, sizeof(tpDig)); return; }
    if (tpLife) *tpLife = dd * 24 + hh;
    timePickerActive = false;
    return;
  }
  if (tpLen >= 4) return;
  tpDig[tpLen++] = TP_KEYS[k][0];
  tpDig[tpLen] = 0;
}

// =====================================================================
//  2.5.0 - KARB-NOT (kisisel karbonhidrat defteri) + GLUKAGON VIDEOSU
// =====================================================================
const char* karbUnitName(int t, bool per) {
  switch (t) {
    case 1: case 2: return L("adet", "piece");          // 2 (eski "tane") = adet
    case 3: return L("porsiyon", "portion");
    case 4: return L("kase", "bowl");
    case 5: return L("dilim", "slice");
    case 6: return L("çay bardağı", "tea glass");
    case 7: return L("su bardağı", "glass");
    case 8: return L("paket", "pack");
    default: return per ? "100 g" : "g";
  }
}

String karbNum(float v) {
  char b[16];
  if (fabsf(v - roundf(v)) < 0.05f) sprintf(b, "%d", (int)roundf(v));
  else { dtostrf(v, 0, 1, b); for (char* p = b; *p; p++) if (*p == '.' && langIdx != 1) *p = ','; }
  return String(b);
}

void karbLoad() {
  karbCount = 0;
  if (!SPIFFS.begin(true)) return;
  if (!SPIFFS.exists("/karb.json")) {
    // ilk acilis: ornek (etiketinizden kontrol edin)
    karbItems[0].name = L("Ekmek", "Bread"); karbItems[0].type = 0; karbItems[0].carb = 49; karbItems[0].uses = 0;
    karbCount = 1;
    return;
  }
  File f = SPIFFS.open("/karb.json", FILE_READ);
  if (!f) return;
  DynamicJsonDocument doc(12288);
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (karbCount >= KARB_MAX) break;
    String n = o["n"] | "";
    n.trim();
    if (!n.length()) continue;
    if (n.length() > 24) n = n.substring(0, 24);
    karbItems[karbCount].name = n;
    karbItems[karbCount].type = constrain((int)(o["t"] | 0), 0, 8); if (karbItems[karbCount].type == 2) karbItems[karbCount].type = 1;
    karbItems[karbCount].carb = constrain((float)(o["c"] | 0.0f), 0.0f, 1000.0f);
    karbItems[karbCount].uses = (uint16_t)constrain((int)(o["u"] | 0), 0, 60000);
    karbCount++;
  }
}

bool karbSaveFile() {
  DynamicJsonDocument doc(12288);
  JsonArray a = doc.to<JsonArray>();
  for (int i = 0; i < karbCount; i++) {
    JsonObject o = a.createNestedObject();
    o["n"] = karbItems[i].name; o["t"] = karbItems[i].type; o["c"] = karbItems[i].carb; o["u"] = karbItems[i].uses;
  }
  File f = SPIFFS.open("/karb.json", FILE_WRITE);
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

// Liste (kind 6): bilgi satiri, menu toplami + sifirla, en cok secilen 5, sonra digerleri
void buildKarbItems() {
  uiItemCount = 0;
  if (karbCount == 0) {
    listAdd(98, IC_INFO, 0x5B75, L("Liste boş", "List is empty"), L("web'den ekleyin", "add on the web"), -1);
    return;
  }
  listAdd(97, IC_INFO, 0x5B75, L("Web'den hazırlanır", "Made on the web"), L("nasıl?", "how?"), -1);
  if (karbMenuCount > 0) {
    String v = karbNum(karbMenuTotal) + " g  (" + String(karbMenuCount) + ")";
    listAdd(99, IC_SUM, 0x0B5E, L("Menü toplamı", "Meal total"), v, -1);
    listAdd(96, IC_REFRESH, 0xA945, L("Menüyü sıfırla", "Reset meal"), "", -1);
  }
  // en cok secilen 5 (en az 1 kez secilmis)
  int top[5]; int nTop = 0;
  bool used[KARB_MAX] = {false};
  for (int k = 0; k < 5; k++) {
    int best = -1;
    for (int i = 0; i < karbCount; i++)
      if (!used[i] && karbItems[i].uses > 0 && (best < 0 || karbItems[i].uses > karbItems[best].uses)) best = i;
    if (best < 0) break;
    used[best] = true; top[nTop++] = best;
  }
  for (int k = 0; k < nTop && uiItemCount < UI_LIST_MAX; k++) {
    int i = top[k];
    String v = karbNum(karbItems[i].carb) + " g / " + karbUnitName(karbItems[i].type, true);
    listAdd(100 + i, IC_SPARKLE, 0xD4A0, karbItems[i].name, v, -1);
  }
  for (int i = 0; i < karbCount && uiItemCount < UI_LIST_MAX; i++) {
    if (used[i]) continue;
    String v = karbNum(karbItems[i].carb) + " g / " + karbUnitName(karbItems[i].type, true);
    listAdd(100 + i, IC_FORK, 0x1D0C, karbItems[i].name, v, -1);
  }
}

float karbAmountValue() {
  String s = String(karbAmt);
  s.replace(',', '.');
  return s.toFloat();
}

float karbResult() {
  if (karbSel < 0 || karbSel >= karbCount) return 0;
  float a = karbAmountValue();
  const KarbItem& k = karbItems[karbSel];
  return (k.type == 0) ? a * k.carb / 100.0f : a * k.carb;
}

static const char* KB_KEYS[12] = {"1", "2", "3", "<", "4", "5", "6", ",", "7", "8", "9", "0"};

void drawKarbCalc() {
  cv->fillScreen(UI_BG);
  const KarbItem& k = karbItems[karbSel];
  bool can = karbAmtLen > 0 && karbResult() > 0;
  drawFormHeaderBtn(k.name.c_str(), L("Ekle", "Add"), can);   // 3.4.1: menuye ekle sag ustte
  // bilgi satiri
  cv->setTextColor(UI_SUBTEXT, UI_BG);
  cv->setTextDatum(TL_DATUM);
  String info = karbNum(k.carb) + L(" g karb / ", " g carbs / ") + karbUnitName(k.type, true);
  drawFreeTitle(info.c_str(), 10, 44, &FreeSansTR7pt);
  // miktar
  cv->fillRoundRect(8, 64, 150, 40, 10, UI_CARD);
  cv->setTextColor(karbAmtLen ? (uint16_t)TFT_WHITE : UI_SUBTEXT, UI_CARD);
  cv->setTextDatum(MR_DATUM);
  dsN(cv, karbAmtLen ? karbAmt : "0", 104, 85, 4);
  cv->setTextDatum(ML_DATUM);
  drawFreeTitle(karbUnitName(k.type, false), 110, 85, &FreeSansTR7pt);
  // sonuc
  float r = karbResult();
  cv->fillRoundRect(166, 64, 146, 40, 10, 0x0B5E);
  cv->setTextColor(TFT_WHITE, 0x0B5E);
  cv->setTextDatum(MC_DATUM);
  String rs = "= " + karbNum(r) + L(" g karb", " g carbs");
  drawFreeTitle(rs.c_str(), 239, 85, &FreeSansTR9pt);
  // tuslar
  for (int i = 0; i < 12; i++) {
    int c = i % 4, rr = i / 4;
    int x = 10 + c * 76, y = 110 + rr * 36;
    uint16_t bg = (i == 3) ? (uint16_t)0x6124 : UI_CARD;
    cv->fillRoundRect(x, y, 70, 32, 8, bg);
    cv->setTextColor(TFT_WHITE, bg);
    cv->setTextDatum(MC_DATUM);
    if (i == 3) drawFreeTitle(L("Sil", "Del"), x + 35, y + 16, &FreeSansTR7pt);
    else dsN(cv, KB_KEYS[i], x + 35, y + 17, 4);
  }
  cv->setTextColor(UI_SUBTEXT, UI_BG);
  cv->setTextDatum(MC_DATUM);
  String at = karbMenuCount > 0 ? String(L("Menü toplamı: ", "Meal total: ")) + karbNum(karbMenuTotal) + " g  (" + String(karbMenuCount) + ")"
                                : String(L("Miktarı yazıp sağ üstten Ekle'ye basın", "Type the amount, then tap Add"));
  drawFreeTitle(at.c_str(), 160, 228, &FreeSansTR7pt);
  cv->setTextDatum(TL_DATUM);
}

void karbCalcTap(int x, int y) {
  bool addTap = (y < LIST_Y && x >= 236);
  if (y < LIST_Y && !addTap) { if (x < 90) { karbView = 0; drawCurrentScreen(); } return; }
  if (y >= 110 && y < 216) {
    int c = (x - 10) / 76, r = (y - 110) / 36;
    if (c < 0 || c > 3 || r < 0 || r > 2) return;
    int k = r * 4 + c;
    if (k == 3) { if (karbAmtLen > 0) karbAmt[--karbAmtLen] = 0; }
    else if (k == 7) { if (karbAmtLen > 0 && karbAmtLen < 6 && !strchr(karbAmt, ',')) { karbAmt[karbAmtLen++] = ','; karbAmt[karbAmtLen] = 0; } }
    else if (karbAmtLen < 6) {
      if (!(karbAmtLen == 1 && karbAmt[0] == '0')) { karbAmt[karbAmtLen++] = KB_KEYS[k][0]; karbAmt[karbAmtLen] = 0; }
    }
    drawCurrentScreen();
    return;
  }
  if (addTap) {
    float r = karbResult();
    if (karbAmtLen == 0 || r <= 0) return;
    karbMenuTotal += r; karbMenuCount++;
    if (mealN < MEAL_MAX) {
      mealRecs[mealN].name = karbItems[karbSel].name; mealRecs[mealN].amt = karbAmountValue();
      mealRecs[mealN].type = karbItems[karbSel].type; mealRecs[mealN].carb = r; mealN++;
    }
    cv->fillRoundRect(70, 96, 180, 44, 22, 0x2A69);
    cv->setTextColor(TFT_WHITE, 0x2A69);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle((String(L("Toplam: ", "Total: ")) + karbNum(karbMenuTotal) + " g").c_str(), 160, 118, &FreeSansTR9pt);
    cv->setTextDatum(TL_DATUM);
    delay(700);
    karbView = 0;
    drawCurrentScreen();
  }
}

void drawKarbScreen() {
  if (karbView == 1 && karbSel >= 0 && karbSel < karbCount) { drawKarbCalc(); return; }
  karbView = 0;
  cv->fillScreen(UI_BG);
  buildKarbItems();
  drawListHeader(L("KARB-NOT", "CARB NOTES"), false);
  drawListBody(6);
}

// liste satirina dokunus (listTap'ten)
void karbListTap(int id) {
  if (id == 99) { openTextPage(TP_MEAL); return; }      // 3.4.2: menunun icerigi
  if (id == 96) {                                        // menuyu sifirla
    karbMenuTotal = 0; karbMenuCount = 0; mealN = 0;
    drawCurrentScreen();
    return;
  }
  if (id == 98 || id == 97) { openTextPage(TP_KARB); return; }
  int i = id - 100;
  if (i < 0 || i >= karbCount) return;
  if (karbItems[i].uses < 60000) karbItems[i].uses++;   // sik kullanilanlar icin
  karbSaveFile();
  karbSel = i; karbView = 1; karbAmtLen = 0; memset(karbAmt, 0, sizeof(karbAmt));
  drawCurrentScreen();
}

// ---------------------------------------------------------------- GLUKAGON VIDEOSU
File videoFile;
uint8_t* videoBuf = nullptr;
uint16_t videoW = 0, videoH = 0, videoFps = 8, videoFrames = 0, videoFrameIdx = 0;
bool videoEmb = false;                 // 3.4.5: yazilima gomulu video oynuyor
bool videoV2 = false;                  // 3.4.5: MMV2 (sadece degisen bolgeler)
uint32_t videoEmbPos = 0;
unsigned long videoNextMs = 0;
// 3.6.0: Ingilizce yama akisi (MMVP). Her yazi kutusu bir "yuva"; temel kare o kutunun
// ustune cizdikce yuva yeniden cizilir, boylece Turkce yazi hic gorunmez.
bool videoEn = false;
uint32_t videoEnPos = 0;
#define VID_SLOTS 64
struct VidSlot { uint32_t off, len; int16_t x, y, w, h; };
VidSlot vidSlot[VID_SLOTS];
bool vidSlotOn[VID_SLOTS];

bool videoExists() {
  if (GLK_MMV_LEN > 16 && (memcmp(GLK_MMV, "MMV1", 4) == 0 || memcmp(GLK_MMV, "MMV2", 4) == 0)) return true;   // 3.4.5: gomulu video
  if (!SPIFFS.begin(true)) return false;
  if (!SPIFFS.exists(VIDEO_PATH)) return false;
  File f = SPIFFS.open(VIDEO_PATH, FILE_READ);
  bool ok = f && f.size() > 16;
  if (f) f.close();
  return ok;
}

void videoStop() {
  if (videoFile) videoFile.close();
  videoPlaying = false;
}

bool videoStart() {
  videoStop();
  videoEnded = false; videoPaused = false;
  const uint8_t* h = nullptr; uint8_t hb[12];
  videoV2 = (GLK_MMV_LEN > 16 && memcmp(GLK_MMV, "MMV2", 4) == 0);
  videoEmb = videoV2 || (GLK_MMV_LEN > 16 && memcmp(GLK_MMV, "MMV1", 4) == 0);
  if (videoEmb) {
    h = GLK_MMV; videoEmbPos = 12;
  } else {
    videoV2 = false;
    if (!SPIFFS.begin(true)) return false;
    videoFile = SPIFFS.open(VIDEO_PATH, FILE_READ);
    if (!videoFile) return false;
    if (videoFile.read(hb, 12) != 12 || memcmp(hb, "MMV1", 4) != 0) { videoFile.close(); return false; }
    h = hb;
    if (!videoBuf) videoBuf = (uint8_t*)heap_caps_malloc(96 * 1024, MALLOC_CAP_SPIRAM);
    if (!videoBuf) { videoFile.close(); return false; }
  }
  videoW = h[4] | (h[5] << 8); videoH = h[6] | (h[7] << 8);
  videoFps = h[8] ? h[8] : 8; videoFrames = h[10] | (h[11] << 8);
  videoFrameIdx = 0;
  videoEn = videoV2 && langIdx != 0 && GLK_EN_LEN > 16 && memcmp(GLK_EN, "MMVP", 4) == 0;
  videoEnPos = 12;
  memset(vidSlotOn, 0, sizeof(vidSlotOn));
  videoPlaying = true;
  videoNextMs = millis();
  cv->fillScreen(TFT_BLACK);
  videoShadow = videoV2 && frameSpriteFor(320, 240);
  if (videoShadow) homeSpr.fillScreen(TFT_BLACK);
  return true;
}

// JPEG genislik/yukseklik (SOF basligindan)
bool jpgDims(const uint8_t* j, uint32_t n, int& w, int& h) {
  uint32_t i = 2;
  while (i + 9 < n) {
    if (j[i] != 0xFF) return false;
    uint8_t m = j[i + 1];
    uint32_t L = (j[i + 2] << 8) | j[i + 3];
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) { h = (j[i + 5] << 8) | j[i + 6]; w = (j[i + 7] << 8) | j[i + 8]; return w > 0 && h > 0; }
    i += 2 + L;
  }
  return false;
}

void drawVideoChrome() {
  // alt cubuk: ilerleme + durum
  cv->fillRect(0, 226, 320, 14, TFT_BLACK);
  if (videoFrames > 0) {
    int w = 300 * videoFrameIdx / videoFrames;
    cv->fillRoundRect(10, 231, 300, 4, 2, 0x3186);
    if (w > 0) cv->fillRoundRect(10, 231, w, 4, 2, sevColor(0));
  }
}

void drawVideoScreen() {
  if (videoPlaying && !videoEnded) {
    if (videoPaused) {
      cv->fillRoundRect(110, 90, 100, 60, 14, 0x2945);
      cv->fillRect(144, 104, 10, 32, TFT_WHITE);
      cv->fillRect(166, 104, 10, 32, TFT_WHITE);
    }
    return;                       // kareler serviceVideo() ile ciziliyor
  }
  cv->fillScreen(UI_BG);
  drawListHeader(L("GLUKAGON", "GLUCAGON"), false);
  if (videoEnded) {
    cv->fillRoundRect(60, 70, 200, 56, 16, 0xA800);
    cv->fillTriangle(146, 84, 146, 112, 170, 98, TFT_WHITE);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("Tekrar oynat", "Play again"), 160, 146, &FreeSansTR9pt);
    cv->setTextColor(TFT_DARKGREY, UI_BG);   // 3.6.1
    drawFreeTitle(L("Bir diyabet hemşiresi tarafından gözden geçirildi.", "Reviewed by a diabetes nurse."), 160, 172, &FreeSansTR6pt);
    drawFreeTitle(L("Doktorunuzun talimatlarının yerini tutmaz.", "Does not replace your doctor's instructions."), 160, 186, &FreeSansTR6pt);
    cv->setTextColor(UI_SUBTEXT, UI_BG);
    drawFreeTitle(L("Sağa kaydır veya Geri: çıkış", "Swipe right or Back: exit"), 160, 208, &FreeSansTR7pt);
    cv->setTextDatum(TL_DATUM);
    return;
  }
  // video yok
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("Cihazda video yok", "No video on the device"), 160, 50, &FreeSansTR9pt);
  cv->setTextColor(UI_SUBTEXT, UI_BG);
  drawFreeTitle(L("Glukagon videosu bu cihaza yüklenmemiş.", "The glucagon video is not installed."), 160, 84, &FreeSansTR7pt);
  drawFreeTitle(L("Kurulum kılavuzuna bakın.", "See the setup guide."), 160, 108, &FreeSansTR7pt);
  cv->setTextDatum(TL_DATUM);
}

void openVideo(int returnScreen, bool fromTeacher) {
  videoReturnScreen = returnScreen;
  videoFromTeacher = fromTeacher;
  formId = 0; textPageActive = false; timePickerActive = false;
  currentScreen = SCR_VIDEO;
  applyRotation();
  if (videoExists()) videoStart();
  else { videoPlaying = false; videoEnded = false; }
  drawCurrentScreen();
}

void closeVideo() {
  videoStop();
  videoEnded = false;
  if (previewVideo) { openPreviewMenu(); return; }      // 3.4.6
  if (videoFromTeacher) { videoFromTeacher = false; currentScreen = 0; drawCurrentScreen(); return; }
  goApps();
}

void videoTap(int x, int y) {
  if (videoPlaying && !videoEnded) {
    if (y < 44 && x < 90) { closeVideo(); return; }      // sol ust kose: cikis
    videoPaused = !videoPaused;
    if (!videoPaused) {
      videoNextMs = millis();
      if (videoShadow) { tft.setClipRect(110, 90, 100, 60); homeSpr.pushSprite(&tft, 0, 0); tft.clearClipRect(); }   // 3.4.6
      else cv->fillRect(110, 90, 100, 60, TFT_BLACK);
    }
    drawCurrentScreen();
    return;
  }
  if (y < LIST_Y && x < 90) { closeVideo(); return; }
  if (videoEnded && y >= 60 && y < 170) { if (videoStart()) drawCurrentScreen(); }
}

void serviceVideo() {
  if (currentScreen != SCR_VIDEO || !videoPlaying || videoPaused || videoEnded) return;
  if ((long)(millis() - videoNextMs) < 0) return;
  videoNextMs += 1000UL / (videoFps ? videoFps : 8);
  if ((long)(millis() - videoNextMs) > 500) videoNextMs = millis();   // geride kaldiysa yakala
  const uint8_t* frame = nullptr; uint32_t len = 0;
  if (videoEmb && videoV2) {                           // MMV2: kare icinde degisen bolgeler
    if (videoEmbPos + 4 > GLK_MMV_LEN) { videoEnded = true; videoStop(); drawCurrentScreen(); return; }
    const uint8_t* q = GLK_MMV + videoEmbPos;
    uint32_t rl = q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t)q[3] << 24);
    uint32_t p = videoEmbPos + 4, e = p + rl;
    if (e > GLK_MMV_LEN) { videoEnded = true; videoStop(); drawCurrentScreen(); return; }
    // 3.6.0: ekrana itilecek dikdortgenler (once tampona hepsi cizilir, sonra itilir -> Turkce yazi parlamaz)
    int16_t dr[24][4]; int nd = 0; bool drAll = false;
    auto addRect = [&](int x, int y, int w, int h) {
      if (nd < 24) { dr[nd][0] = x; dr[nd][1] = y; dr[nd][2] = w; dr[nd][3] = h; nd++; } else drAll = true;
    };
    bool enOk = videoEn && videoShadow;
    if (videoEn && videoEnPos + 4 <= GLK_EN_LEN) {     // sahne degisiminde kapanan yuvalar temel kareden ONCE kapanir
      const uint8_t* eq = GLK_EN + videoEnPos;
      uint32_t ep = videoEnPos + 4, ee = ep + (eq[0] | (eq[1] << 8) | (eq[2] << 16) | ((uint32_t)eq[3] << 24));
      if (ee > GLK_EN_LEN) ee = ep;
      while (ep + 10 <= ee) {
        const uint8_t* r = GLK_EN + ep;
        uint32_t jl = r[6] | (r[7] << 8) | (r[8] << 16) | ((uint32_t)r[9] << 24);
        if (jl == 0 && r[0] < VID_SLOTS) vidSlotOn[r[0]] = false;
        ep += 10 + jl;
      }
    }
    while (p + 8 <= e) {
      const uint8_t* r = GLK_MMV + p;
      int rx = r[0] | (r[1] << 8), ry = r[2] | (r[3] << 8);
      uint32_t jl = r[4] | (r[5] << 8) | (r[6] << 16) | ((uint32_t)r[7] << 24);
      p += 8;
      if (jl == 0 || p + jl > e) break;
      if (videoShadow) {                               // 3.4.6: once tampona, sonra sadece o bolge ekrana
        int jw = 0, jh = 0;
        homeSpr.drawJpg(GLK_MMV + p, jl, rx, ry);
        if (!jpgDims(GLK_MMV + p, jl, jw, jh)) { jw = 320 - rx; jh = 240 - ry; drAll = true; }
        if (enOk) {                                    // bu bolgenin altinda kalan Ingilizce yuvalari yeniden ciz
          for (int k = 0; k < VID_SLOTS; k++) {
            if (!vidSlotOn[k]) continue;
            VidSlot& v = vidSlot[k];
            if (v.x >= rx + jw || rx >= v.x + v.w || v.y >= ry + jh || ry >= v.y + v.h) continue;
            homeSpr.drawJpg(GLK_EN + v.off, v.len, v.x, v.y);
            addRect(v.x, v.y, v.w, v.h);
          }
        }
        addRect(rx, ry, jw, jh);
      } else tft.drawJpg(GLK_MMV + p, jl, rx, ry);
      p += jl;
    }
    videoEmbPos = e;
    if (videoEn && videoEnPos + 4 <= GLK_EN_LEN) {     // 3.6.0: bu karenin Ingilizce yamalari
      const uint8_t* eq = GLK_EN + videoEnPos;
      uint32_t el = eq[0] | (eq[1] << 8) | (eq[2] << 16) | ((uint32_t)eq[3] << 24);
      uint32_t ep = videoEnPos + 4, ee = ep + el;
      if (ee > GLK_EN_LEN) { videoEn = false; ee = ep; }
      while (ep + 10 <= ee) {
        const uint8_t* r = GLK_EN + ep;
        int k = r[0];
        int x = r[2] | (r[3] << 8), y = r[4] | (r[5] << 8);
        uint32_t jl = r[6] | (r[7] << 8) | (r[8] << 16) | ((uint32_t)r[9] << 24);
        ep += 10;
        if (ep + jl > ee) break;
        if (k < VID_SLOTS) {
          if (jl != 0) {
            int jw = 0, jh = 0;
            if (jpgDims(GLK_EN + ep, jl, jw, jh)) {
              vidSlot[k] = { ep, jl, (int16_t)x, (int16_t)y, (int16_t)jw, (int16_t)jh };
              vidSlotOn[k] = true;
              if (videoShadow) { homeSpr.drawJpg(GLK_EN + ep, jl, x, y); addRect(x, y, jw, jh); }
              else tft.drawJpg(GLK_EN + ep, jl, x, y);
            }
          }
        }
        ep += jl;
      }
      videoEnPos = ee;
    }
    if (videoShadow) {
      if (drAll) homeSpr.pushSprite(&tft, 0, 0);
      else for (int i = 0; i < nd; i++) { tft.setClipRect(dr[i][0], dr[i][1], dr[i][2], dr[i][3]); homeSpr.pushSprite(&tft, 0, 0); tft.clearClipRect(); }
    }
    videoFrameIdx++;
    return;                                            // videonun kendi ilerleme cubugu var
  }
  if (videoEmb) {                                      // 3.4.5: gomulu video (flash'tan dogrudan)
    if (videoEmbPos + 4 > GLK_MMV_LEN) { videoEnded = true; videoStop(); drawCurrentScreen(); return; }
    const uint8_t* lb = GLK_MMV + videoEmbPos;
    len = lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((uint32_t)lb[3] << 24);
    if (len == 0 || len > 96 * 1024 || videoEmbPos + 4 + len > GLK_MMV_LEN) { videoEnded = true; videoStop(); drawCurrentScreen(); return; }
    frame = lb + 4; videoEmbPos += 4 + len;
  } else {
    uint8_t lb[4];
    if (!videoFile || videoFile.read(lb, 4) != 4) { videoEnded = true; videoStop(); drawCurrentScreen(); return; }
    len = lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((uint32_t)lb[3] << 24);
    if (len == 0 || len > 96 * 1024 || videoFile.read(videoBuf, len) != len) {
      videoEnded = true; videoStop(); drawCurrentScreen(); return;
    }
    frame = videoBuf;
  }
  int ox = (320 - (int)videoW) / 2, oy = (226 - (int)videoH) / 2;
  if (ox < 0) ox = 0;
  if (oy < 0) oy = 0;
  tft.drawJpg(frame, len, ox, oy);
  videoFrameIdx++;
  if ((videoFrameIdx % 4) == 0 || videoFrameIdx == 1) drawVideoChrome();
}

// =====================================================================
//  2.5.0 - GEZINME: geri (saga kaydir / BOOT kisa), liste dokunuslari
// =====================================================================
void formAbandon() {
  if (formId && fHasSave && formIsDirty()) formRevert();
  formId = 0; formDepth = 0; formDlg = 0;
}

void doSnooze() {
  forceLandscape();
  startSnooze();
  stopSpeaker();
  beepPhase = 0;
  cv->fillScreen(UI_BG);
  cv->setTextColor(sevColor(2), UI_BG);
  cv->setTextDatum(MC_DATUM);
  char sb[24];
  snprintf(sb, sizeof(sb), L("Alarm %d dk", "Alarm %d min"), snoozeShownMins());
  dsN(cv, sb, 160, 100, 4);
  drawFreeTitle(L("ertelendi", "snoozed"), 160, 140, &FreeSansTR9pt);
  cv->setTextDatum(TL_DATUM);
  delay(1200);
  drawCurrentScreen();
}

void startTeacherFlow() {
  teacherPreview = 0;
  teacherConscious = true;
  teacherGlucagonPage = 0;
  teacherFingerOk = true;
  teacherCat = 0; teacherBack = 0;
  if (sgv > 0 && (sgv < alarmLow || sgv >= alarmVeryHigh)) teacherStep = 1;
  else teacherStep = 0;
  if (teacherStep > 0) currentScreen = 0;
  drawCurrentScreen();
}

// Siklatma testinden donus: Modlar > Uyku zinciri korunur
void openSleepFormFromTest() {
  if (micTestFrom >= 0) { currentScreen = micTestFrom; micTestFrom = -1; }
  formId = F_MODES; formDepth = 0; formDlg = 0;
  formSnapshot();
  formOpen(F_SLEEP);
}

// ana ekranda bos alana dokunus: hata varsa hata ekrani, yoksa kisa ipucu
void homeEmptyTap() {
  if (!dataValid && lastErrorCode > 0) {
    showErrorScreen = true; errorDismissed = false;
    drawErrorScreenFull();
    return;
  }
  homeHintUntil = millis() + 2500;
  drawCurrentScreen();
}

// ---------------------------------------------------------------- 3.0.0 ANA EKRAN SAYFALARI
// Sira: 1 ana tema, 2. tema, 3. tema, Sensor/Set, Grafik, Istatistik, Zamanlayici, Karb-Not, Uygulamalar.
// Kapali olanlar atlanir. Sola kaydir = sonraki, saga kaydir = onceki (dairesel).
int pagerList(int* out) {
  int n = 0;
  for (int p = 0; p < P_N; p++) if (p == P_HOME || p == P_APPS || pgOn[p]) out[n++] = p;
  return n;
}
int pageScreen(int p) {
  switch (p) {
    case P_SENSOR: return SCR_SENSOR;
    case P_GRAPH:  return SCR_GRAPH;
    case P_STATS:  return SCR_STATS;
    case P_TIMER:  return SCR_TIMER;
    case P_KARB:   return SCR_KARB;
    case P_APPS:   return SCR_APPS;
    default:       return -2;            // tema sayfalari: ekran 0/1
  }
}
// Su an sayfa dizisindeki bir sayfada miyiz (alt pencere / form / hata acik degilken)
bool pagerActive() {
  if (curPage < 0 || curPage >= P_N) return false;
  if (formId || textPageActive || timePickerActive || showErrorScreen || showGuide || screenAsleep) return false;
  if (uiPreviewActive || subScreen != 0) return false;
  int sc = pageScreen(curPage);
  if (sc == -2) return isHomeScreen() && teacherStep == 0;
  if (sc != currentScreen) return false;
  if (currentScreen == SCR_GRAPH && (graphDetailOpen || graphMenuOpen)) return false;
  if (currentScreen == SCR_KARB && karbView != 0) return false;
  return true;
}
void showPage(int p) {
  formId = 0; formDepth = 0; formDlg = 0; textPageActive = false; timePickerActive = false;
  subScreen = 0; showGuide = false; graphDetailOpen = false; graphMenuOpen = false;
  themePreview = false; uiPreviewActive = false;
  if (videoPlaying) videoStop();
  curPage = p;
  pageThemeOverride = (p == P_TH2) ? pgTh2Theme : (p == P_TH3) ? pgTh3Theme : -1;
  if (pageThemeOverride >= 0 && themeHidden(pageThemeOverride)) pageThemeOverride = 0;
  int sc = pageScreen(p);
  if (sc == -2) {
    currentScreen = homeScreenIdx();
    uiPageMode = themeUiMode(activeTheme());
  } else {
    currentScreen = sc;
    if (p == P_KARB) { karbView = 0; karbSel = -1; }
    if (p == P_SENSOR) computeManualAges();
  }
  applyRotation();
  drawCurrentScreen();
}
void pagerStep(int step) {
  int list[P_N]; int n = pagerList(list);
  int i = 0;
  for (int k = 0; k < n; k++) if (list[k] == curPage) { i = k; break; }
  i = (i + step + n) % n;
  showPage(list[i]);
}

void goBack() {
  if (screenAsleep) return;
  lastUserTouchMs = millis();
  if (showErrorScreen) { showErrorScreen = false; errorDismissed = true; goHome(); return; }
  if (timePickerActive) { timePickerActive = false; drawCurrentScreen(); return; }
  if (textPageActive) { textPageActive = false; drawCurrentScreen(); return; }
  if (formId) {
    if (formDlg) { formDlg = 0; drawCurrentScreen(); return; }
    formBack();
    return;
  }
  switch (currentScreen) {
    case 0:
    case 1:
      if (teacherStep > 0) {
        if (teacherGlucagonPage > 0) teacherGlucagonPage = 0;
        else teacherStep = 0;
        if (teacherStep == 0) { if (teacherPreview) openPreviewMenu(); else goHome(); } else drawCurrentScreen();
        return;
      }
      if (pageThemeOverride >= 0) goHome();          // 2./3. tema sayfasi: ana sayfaya
      return;
    case SCR_APPS: goHome(); return;
    case SCR_THEME:
      if (themePreview) { themePreview = false; drawCurrentScreen(); }
      else if (thPickSlot) themePickDone(false);
      else goApps();
      return;
    case SCR_GRAPH:
      if (graphDetailOpen || graphMenuOpen) { graphDetailOpen = false; graphMenuOpen = false; drawCurrentScreen(); }
      else if (pagerActive()) goHome();
      else goApps();
      return;
    case SCR_KARB:
      if (karbView == 1) { karbView = 0; drawCurrentScreen(); }
      else if (pagerActive()) goHome();
      else goApps();
      return;
    case SCR_VIDEO: closeVideo(); return;
    case SCR_SETTINGS:
      if (subScreen == 21) { subScreen = 0; openSleepFormFromTest(); return; }
      if (subScreen != 0) { subScreen = 0; drawCurrentScreen(); return; }
      goApps();
      return;
    default:
      if (pagerActive()) goHome(); else goApps();
      return;
  }
}

// dir: +1 parmak saga (geri), -1 parmak sola (ana ekrandan uygulamalara)
void handleSwipe(int dir) {
  lastUserTouchMs = millis();
  if (pagerActive()) { pagerStep(dir > 0 ? -1 : 1); return; }   // saga = onceki, sola = sonraki
  if (dir > 0) { goBack(); return; }
}

bool autoRefreshOk() {
  if (formId || textPageActive || timePickerActive || showGuide || screenAsleep) return false;
  return isHomeScreen() || currentScreen == SCR_APPS || currentScreen == SCR_GRAPH ||
         currentScreen == SCR_STATS || currentScreen == SCR_SENSOR || currentScreen == SCR_TIMER;
}

bool listScreenActive() {
  if (showErrorScreen) return false;
  if (showGuide || timePickerActive || formDlg) return false;
  if (textPageActive || formId) return true;
  if (currentScreen == SCR_KARB) return karbView == 0;
  if (currentScreen == SCR_THEME) return !themePreview;
  return isSettingsScreen() && subScreen == 0;
}

int listKindNow() {
  if (textPageActive) return 2;
  if (formId) return 5;
  if (currentScreen == SCR_KARB) return 6;
  if (currentScreen == SCR_THEME) return 7;
  return 0;
}

void listTap(int kind, int x, int y) {
  if (kind == 5) { formTap(x, y); return; }
  if (kind == 7) { themeTap(x, y); return; }
  if (y < LIST_Y) {
    if (x < 90) {
      if (kind == 2) { textPageActive = false; drawCurrentScreen(); }
      else goBack();
    }
    return;
  }
  if (kind == 2) {                            // metin sayfasinda sadece dugme satirlari (3.4.6)
    int ry2 = y - LIST_Y - 4 + txtScroll;
    for (int i = 0; i < txtN; i++)
      if (txtL[i].st == 6 && ry2 >= txtL[i].y && ry2 < txtL[i].y + 44) { txtButtonTap(txtL[i].col); return; }
    return;
  }

  int scroll = listScrollRef(kind);
  int ry = y - LIST_Y - 4 + scroll;
  if (ry < 0 || x < 8 || x > 306) return;
  int idx = ry / ROW_P;
  if (idx >= uiItemCount || (ry % ROW_P) > ROW_H) return;

  if (uiItems[idx].help && uiItems[idx].infoX > 0 && abs(x - uiItems[idx].infoX) <= 16) {
    openTextPage(uiItems[idx].help);
    return;
  }

  if (kind == 6) { karbListTap(uiItems[idx].id); return; }
  switch (uiItems[idx].id) {
    case 0:  formOpen(F_ALARMS); return;
    case 1:  formOpen(F_TARGETS); return;
    case 13: formOpen(F_MODES); return;
    case 2:  formOpen(F_DISPLAY); return;
    case 5:  formOpen(F_LANG); return;
    case 8:  subScreen = 7; break;
    case 9:  openTextPage(TP_INFO); return;
    case 10: openTextPage(TP_GUIDE); return;
    case 12: formOpen(F_RESET); return;
    case 14: formOpen(F_PAGES); return;
  }
  drawCurrentScreen();
}

// Liste, form ve metin ekranlarinda dokunma: dikey surukle = kaydir, yatay saga = geri,
// kisa dokunus = ac. true donerse normal dokunma islemesi atlanir.
bool handleListTouch(bool touchNow, bool touchWasPrev, int rawX, int rawY, unsigned long lastNav) {
  static bool active = false, moved = false, hmove = false;
  static int sx = 0, sy = 0, sScroll = 0, sKind = -1, lx = 0;
  if (!listScreenActive()) { active = false; return false; }
  int kind = listKindNow();
  int& scroll = (kind == 2) ? txtScroll : listScrollRef(kind);
  if (touchNow) {
    int x, y;
    mapTouchToScreen(rawX, rawY, x, y);
    if (!active) {
      if (touchWasPrev || millis() - lastNav < 250) return true;
      active = true; moved = false; hmove = false; sx = x; sy = y; lx = x; sScroll = scroll; sKind = kind;
      lastUserTouchMs = millis();
    } else if (kind == sKind) {
      lx = x;
      int dy = y - sy, dx = x - sx;
      if (!moved && !hmove) {
        if (abs(dx) > 14 && abs(dx) > abs(dy)) hmove = true;
        else if ((dy > 8 || dy < -8) && sy >= LIST_Y) moved = true;
      }
      if (moved) {
        int ns = sScroll - dy;
        if (ns != scroll) {
          scroll = ns;
          if (kind == 2) drawTextBody();
          else if (kind == 5) drawFormBody();
          else if (kind == 7) drawThemeBody();
          else drawListBody(kind);
        }
      }
    }
    return true;
  }
  if (active) {
    active = false;
    lastUserTouchMs = millis();
    if (kind != sKind) return true;
    if (hmove) {
      if (pagerActive() && abs(lx - sx) > 60) pagerStep(lx > sx ? -1 : 1);
      else if (lx - sx > 60) goBack();
      return true;
    }
    if (!moved) listTap(kind, sx, sy);
    return true;
  }
  return false;
}

// Ekran dogrudan (tamponsuz) mu cizilecek: Foto temasi, video, siklatma testi
bool screenDirect() {
  if (currentScreen == SCR_VIDEO && videoPlaying && !videoEnded) return true;
  if (isSettingsScreen() && subScreen == 21 && !formId && !textPageActive) return true;
  if (!formId && !textPageActive && !timePickerActive && !ageWarnActive) {
    if (isHomeScreen() && teacherStep == 0 && themeUiMode(activeTheme()) == 4) return true;
    if (currentScreen == SCR_THEME && themePreview && thSel == 4) return true;
  }
  return false;
}

void drawScreenBody() {
  famPillW = 0;                       // 4.0: kisi hapi bu cizimde yeniden yerlestirilir
  // Sensor/Set sure-dolum uyarisi aktifken normal cizim yerine uyari sayfasi
  if (ageWarnActive) { drawAgeWarnScreen(); return; }
  if (currentScreen < 0 || currentScreen >= SCR_COUNT) currentScreen = homeScreenIdx();
  if (textPageActive) { drawTextPage(); return; }
  if (timePickerActive) { drawTimePicker(); return; }
  if (formId) { drawForm(); return; }
  switch (currentScreen) {
    case SCR_SETTINGS: drawSettingsScreen(); return;
    case SCR_TIMER:    drawTimerScreen(); return;
    case SCR_STATS:    drawStatsScreen(); if (personCount > 1) drawPersonPill(168, 30); return;
    case SCR_GRAPH:    drawScreen2(); if (personCount > 1 && !graphDetailOpen && !graphMenuOpen) drawPersonPill(110, 52); return;
    case SCR_SENSOR:   computeManualAges(); drawScreen3_Sensor(); if (personCount > 1) drawPersonPill(160, 214); return;
    case SCR_APPS:     drawAppsPage(); return;
    case SCR_KARB:     drawKarbScreen(); return;
    case SCR_VIDEO:    drawVideoScreen(); return;
    case SCR_THEME:    drawThemePage(); return;
    default:
      if (currentScreen == 0 && teacherStep > 0) { drawScreen0(); return; }
      currentScreen = homeScreenIdx();
      if (personCount > 1 && famMode == 1) { drawFamSplit(); return; }   // 4.0: bolunmus ekran
      drawHome();
      if (personCount > 1) drawPersonPill(cv->width() / 2, 1);          // 4.0: sirayla: kimin degeri
      return;
  }
}

// 2.6.0: her ekran once PSRAM tamponuna cizilir, bos (anahtar renkli) pikseller
// secili arka planla doldurulur ve tek seferde (titremeden) ekrana gonderilir.
void drawCurrentScreen() {
  if (frameActive) { drawScreenBody(); return; }
  applyRotation();
  cv = &tft; uiBg = 0;
  if (!screenDirect() && frameSpriteFor(tft.width(), tft.height())) {
    frameActive = true; cv = &homeSpr; uiBg = HOME_KEY;
    drawScreenBody();
    frameActive = false; cv = &tft; uiBg = 0;
    composeBuf((uint16_t*)homeSpr.getBuffer(), sprW, sprH, 0, sprH, currentBg());
    homeSpr.pushSprite(0, 0);
  } else {
    drawScreenBody();
  }
  cv = &tft;
}

// ---------------------------------------------------------------- WiFi kurulumu (BOOT 2 sn veya menuden)
// Kurulum: cihaz temiz bir yeniden baslatmayla dogrudan kurulum ekranina acilir.
// (Calisirken portal acmak, bazi ag hatalarindan sonra cihazi resetliyordu; acilistaki
//  kurulum yolu ise sorunsuz - ayni yolu kullaniyoruz.)
void restartIntoPortal() {
  stopSpeaker();
  forceLandscape();
  cv = &tft;
  cv->fillScreen(UI_BG);
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Kurulum açılıyor...", "Opening setup..."), 160, 110, &FreeSansTR9pt);
  cv->setTextColor(0x8410, UI_BG);
  drawFreeTitle(L("Cihaz yeniden başlıyor", "Device restarting"), 160, 140, &FreeSansTR7pt);
  cv->setTextDatum(TL_DATUM);
  saveSettings();
  portalOnBoot = PORTAL_MAGIC;
  delay(600);
  ESP.restart();
}

void doWifiSetup() {
  restartIntoPortal();
}

// ---------------------------------------------------------------- 2.9.0 WiFi hata yardimcilari
String savedWifiName() {
  String n = WiFi.SSID();
  if (!n.length() && wifiCount > 0) n = wifiSsid[0];
  if (!n.length()) n = "-";
  return n;
}

bool wifiReasonIsAuth() {
  uint8_t r = wifiDiscReason;
  return r == 2 || r == 15 || r == 202 || r == 204;
}
bool wifiReasonNoAp() {
  uint8_t r = wifiDiscReason;
  return r == 201 || (r >= 210 && r <= 212);
}

void libreLoginError() {
  if (libreLastCode <= 0) setFetchError(3, "Libre baglanti");
  else if (libreLastStatus == 4) setFetchError(2, "Libre onay");
  else setFetchError(2, "Libre giris");
}

// WiFi yokken arka planda (bloklamadan) kayitli aglari sirayla dene
// Hata surdukce yenileme araligi: 25 sn -> 60 sn -> 2 dk (giris/ayar hatasinda 2 dk)
unsigned long fetchIntervalNow() {
  if (netNoInternet && WiFi.status() == WL_CONNECTED) return 10000UL;   // 3.4.8: internet gelince hemen fark et
  // 3.4.20: giris hatasi (sifre yanlis / hesap) ekranda veri olsa da seyrek denenir - hesap kilitlenmesin.
  // Kurulumdan / web'den kaydetme ve hata ekranindaki Yenile hemen dener (sayac sifirlanir).
  if (lastErrorCode == 2) return consecutiveFetchFails < 2 ? FETCH_INTERVAL : consecutiveFetchFails < 4 ? 60000UL : consecutiveFetchFails < 6 ? 120000UL : 300000UL;
  if (!dataValid && lastErrorCode == 5) return consecutiveFetchFails >= 3 ? 300000UL : 120000UL;
  if (dataValid || lastErrorCode == 0 || lastErrorCode == 1) return FETCH_INTERVAL;
  if (consecutiveFetchFails >= 4) return 120000UL;
  if (consecutiveFetchFails >= 2) return 60000UL;
  return FETCH_INTERVAL;
}

// Veri yokken yeniden denemeden hemen once: ekran birkac saniye mesgul olacak
void drawRetryPill() {
  if (screenAsleep) return;
  lgfx::LovyanGFX* sv = cv; cv = &tft;      // dogrudan ekrana (mevcut goruntunun ustune)
  int W = cv->width(), pw = 170, px = W / 2 - pw / 2, py = (W < 300) ? 30 : 4;
  cv->fillRoundRect(px, py, pw, 26, 13, 0x2945);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Yeniden deneniyor...", "Retrying..."), W / 2, py + 13, &FreeSansTR7pt);
  retryPillShown = true;
  cv->setTextDatum(TL_DATUM);
  cv = sv;
}

void wifiBgReconnect() {
  // Bloklamadan: once cevredeki aglari tara (arka planda), sonraki turda kayitli
  // aglardan en guclu olana baglan. Tarama olmazsa sirayla dene.
  static int idx = 0;
  static bool scanNext = true;
  static unsigned long lastBegin = 0;
  if (wifiCount <= 0) return;
  if (lastBegin && millis() - lastBegin < 10000UL) return;     // 3.4.8: suren baglanma denemesini bozma
  ensureWifiEvents();
  WiFi.mode(WIFI_STA);
  int sc = WiFi.scanComplete();
  if (sc == WIFI_SCAN_RUNNING) return;
  if (sc >= 0) {
    int best = -1, bestRssi = -1000;
    for (int i = 0; i < sc; i++) {
      String s = WiFi.SSID(i);
      int r = WiFi.RSSI(i);
      for (int k = 0; k < wifiCount; k++)
        if (wifiSsid[k].length() && s == wifiSsid[k] && r > bestRssi) { best = k; bestRssi = r; }
    }
    WiFi.scanDelete();
    scanNext = false;
    if (best >= 0) { WiFi.begin(wifiSsid[best].c_str(), wifiPass[best].c_str()); lastBegin = millis(); return; }
  }
  if (scanNext) {
    // 3.4.28: kopan aga otomatik yeniden baglanma denemesi surerken tarama basarisiz oluyor ve
    // baska kayitli aga gecilemiyordu (06.10) -> once o denemeyi durdur
    WiFi.disconnect(false, false);
    if (WiFi.scanNetworks(true) != WIFI_SCAN_FAILED) return;   // sonuc sonraki turda
  }
  scanNext = true;
  if (idx >= wifiCount) idx = 0;
  WiFi.disconnect(false, false);                               // 3.4.28
  WiFi.begin(wifiSsid[idx].c_str(), wifiPass[idx].c_str());
  lastBegin = millis();
  idx = (idx + 1) % wifiCount;
}

String errorHintLong() {
  bool dex = (dataSource == "dexcom"), lib = (dataSource == "libre"), med = (dataSource == "medtrum");
  switch (lastErrorCode) {
    case 1:
      if (wifiCount == 0) return L("Kayıtlı WiFi yok. Kurulum'a dokunup ağınızı seçin.",
                                   "No saved WiFi. Tap Setup and choose your network.");
      if (wifiReasonIsAuth()) return L("WiFi şifresi yanlış olabilir. Kurulum'a dokunup şifreyi yeniden girin.",
                                       "The WiFi password may be wrong. Tap Setup and enter it again.");
      if (wifiReasonNoAp()) return L("Ağ bulunamadı. Modem açık ve yakında mı? Değilse Kurulum ile başka ağ seçin.",
                                     "Network not found. Is the router on and nearby? Or pick another in Setup.");
      return L("Bağlanılamadı. Şifre yanlış ya da modem kapalı / uzak olabilir. Kurulum'a dokunup şifreyi yeniden girin.",
               "Could not connect. Wrong password, or router off / far away. Tap Setup and enter it again.");
    case 2:
      if (lib && lastErrorDetail == "Libre onay")
        return L("LibreLinkUp uygulamasını açıp yeni kullanım koşullarını onaylayın, sonra Yenile'ye dokunun.",
                 "Open the LibreLinkUp app and accept the new terms, then tap Retry.");
      if (med) return L("Medtrum EasyFollow e-posta veya şifresi yanlış olabilir. Kurulum'dan ya da web sayfasından yeniden girin.",
                        "Medtrum EasyFollow e-mail or password may be wrong. Re-enter it in Setup or on the web page.");
      if (dex) return L("Dexcom kullanıcı adı / şifre yanlış olabilir ya da sunucu (us / ous) hatalı. Kurulum'dan yeniden girin.",
                        "Dexcom username / password may be wrong, or the server (us / ous). Re-enter it in Setup.");
      if (lib) return L("LibreLinkUp e-posta veya şifresi yanlış olabilir. Kurulum'dan ya da web sayfasından yeniden girin.",
                        "LibreLinkUp e-mail or password may be wrong. Re-enter it in Setup or on the web page.");
      return L("Nightscout API secret / token yanlış olabilir. Kurulum'dan ya da web sayfasından yeniden girin.",
               "Nightscout API secret / token may be wrong. Re-enter it in Setup or on the web page.");
    case 3:
      if (netNoInternet) return L("Cihaz WiFi'ye bağlı ama internete çıkamıyor. Modemin internet bağlantısını kontrol edin; bağlantı gelince veri kendiliğinden gelir.",
                                  "Connected to WiFi but there is no internet. Check the router's internet; data returns by itself.");
      if (dataSource == "nightscout")
        return L("Nightscout'a ulaşılamadı. Adres doğru mu, site açık mı? Sitenin güvenlik sertifikası geçerli olmalı (https). Birazdan yeniden denenecek.",
                 "Could not reach Nightscout. Is the address right and the site up? Its https certificate must be valid. Will retry shortly.");
      return L("Kaynağa ulaşılamadı. İnternet ya da kaynak adresini kontrol edin; birazdan yeniden denenecek.",
                     "Could not reach the source. Check internet or the source address; will retry shortly.");
    case 4:
      if (lib && lastErrorDetail == "Libre baglanti yok")
        return L("LibreLinkUp'ta takip edilen kişi yok. Libre uygulamasından bu hesaba davet gönderin.",
                 "No one is shared in LibreLinkUp. Send an invite to this account from the Libre app.");
      if (med && lastErrorDetail == "Medtrum takip yok")
        return L("Bu EasyFollow hesabı kimseyi takip etmiyor. Medtrum uygulamasından bu hesaba davet gönderin.",
                 "This EasyFollow account follows no one. Send an invite to it from the Medtrum app.");
      if (med && lastErrorDetail == "Medtrum isiniyor")
        return L("Sensör ısınıyor; ısınma bitince değerler gelecek.", "The sensor is warming up; readings will follow.");
      if (med) return L("Aktif sensör yok ya da yeni ölçüm gelmedi. Medtrum uygulaması çalışıyor mu?",
                        "No active sensor or no new reading. Is the Medtrum app running?");
      if (dex) return L("Son 24 saatte veri yok. Dexcom uygulamasında Paylaşım (Share) açık ve en az bir takipçi ekli mi?",
                        "No data in 24 h. Is Share on in the Dexcom app, with at least one follower?");
      if (lib) return L("Sensörden yeni ölçüm gelmemiş. Libre uygulaması çalışıyor mu?",
                        "No new reading from the sensor. Is the Libre app running?");
      return L("Nightscout'a son 24 saatte veri gelmemiş. Telefon / uploader çalışıyor mu?",
               "No data reached Nightscout in 24 h. Is the phone / uploader running?");
    case 5: return L("Kaynak bilgisi eksik. Kurulum'a dokunup Nightscout, Dexcom, Libre veya Medtrum bilgisini girin.",
                     "Source is missing. Tap Setup and enter Nightscout, Dexcom or Libre details.");
    case 6: return L("Kaynaktan beklenmeyen yanıt geldi. Birazdan yeniden denenecek.",
                     "Unexpected response from the source. Will retry shortly.");
    default: return L("Birazdan yeniden denenecek.", "Will retry shortly.");
  }
}

String homeNetPillText() {
  if (themePreview || teacherStep != 0) return "";
  if (WiFi.status() != WL_CONNECTED) return L("WiFi yok - dokunun", "No WiFi - tap");
  if (netNoInternet) return L("İnternet yok - dokunun", "No internet - tap");
  if (lastErrorCode == 2 && dataValid) return L("Giriş hatası - dokunun", "Login error - tap");   // 3.4.20
  if ((lastErrorCode == 3 || lastErrorCode == 6) && consecutiveFetchFails >= 2)
    return L("Sunucuya ulaşılamıyor", "Server unreachable");
  return "";
}
bool homeWifiPillOn() { return homeNetPillText().length() > 0; }
bool homeWifiHit(int x, int y) {
  return y < ((cv->width() < 300) ? 70 : 44) && abs(x - cv->width() / 2) < 125;
}
void openWifiErrorScreen() {
  if (WiFi.status() != WL_CONNECTED || lastErrorCode == 0) setFetchError(1, savedWifiName());
  showErrorScreen = true; errorDismissed = false;
  drawErrorScreenFull();
}

// BOOT basili tutulurken geri bildirim (2 sn dolunca WiFi kurulumu)
void drawThemeHoldPill(unsigned long held) {
  int W = cv->width();
  int pwBox = (W < 300) ? 228 : 260, x = W / 2 - pwBox / 2;
  cv->fillRoundRect(x, 8, pwBox, 44, 12, 0x2945);
  cv->drawRoundRect(x, 8, pwBox, 44, 12, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("Temalar: basılı tutun", "Themes: keep holding"), W / 2, 22, &FreeSansTR7pt);
  int barW = pwBox - 24;
  long p = (long)held - 800; if (p < 0) p = 0;
  int pw = (int)constrain(p * barW / 2200, 0L, (long)barW);
  cv->fillRoundRect(x + 12, 38, barW, 6, 3, 0x4208);
  if (pw > 4) cv->fillRoundRect(x + 12, 38, pw, 6, 3, UI_ACCENT);
  cv->setTextDatum(TL_DATUM);
}

void drawBootHoldPill(unsigned long held) {
  int W = cv->width();
  int pwBox = (W < 300) ? 228 : 260, x = W / 2 - pwBox / 2;
  cv->fillRoundRect(x, 8, pwBox, 44, 12, 0x2945);
  cv->drawRoundRect(x, 8, pwBox, 44, 12, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x2945);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(L("WiFi kurulumu: basılı tutun", "WiFi setup: keep holding"), W / 2, 22, &FreeSansTR7pt);
  int barW = pwBox - 24;
  int pw = (int)constrain((long)(held * barW / 2000), 0L, (long)barW);
  cv->fillRoundRect(x + 12, 38, barW, 6, 3, 0x4208);
  if (pw > 4) cv->fillRoundRect(x + 12, 38, pw, 6, 3, sevColor(2));
  cv->setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------- dusuk pil (%20)
int lowBatStep = 0;                // 1: %20, 2: %15, 3: %10, 4: %5 uyarisi verildi
int lowBatStepFor(int p) { return p <= 5 ? 4 : p <= 10 ? 3 : p <= 15 ? 2 : p <= 20 ? 1 : 0; }
void lowBatPopup(const char* line2) {
  if (screenAsleep) return;
  stopSpeaker();
  int W = cv->width(), H = cv->height(), bx = W / 2 - 116, by = H / 2 - 30;
  cv->fillRoundRect(bx, by, 232, 60, 16, 0x8000);
  cv->drawRoundRect(bx, by, 232, 60, 16, sevColor(0));
  cv->setTextColor(TFT_WHITE, 0x8000);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle(batteryPctText(L("Pil azaldı: %%%d", "Battery low: %d%%")).c_str(), W / 2, by + 20, &FreeSansTR9pt);
  drawFreeTitle(line2, W / 2, by + 44, &FreeSansTR7pt);
  cv->setTextDatum(TL_DATUM);
  delay(1800);
  redrawNow();
}
// 3.4.16: pil bitmek uzereyken (yuk altinda <=3,30 V, 1 dk boyunca) kendini kapatir; pil tamamen
// bitip cihazin acilip kapanma dongusune girmesini onler. 5 dk'da bir uyanip sarja takili mi bakar
// (ekran kapali); takiliysa normal acilir.
void lowBatSleepNow(bool screenOn) {
  lowBatSleepFlag = LOWBAT_MAGIC;
  ledcDetach(BACKLIGHT_PIN);
  pinMode(BACKLIGHT_PIN, OUTPUT); digitalWrite(BACKLIGHT_PIN, LOW);
  digitalWrite(AMP_CTRL_PIN, HIGH);
  gpio_hold_en((gpio_num_t)BACKLIGHT_PIN);
  gpio_hold_en((gpio_num_t)AMP_CTRL_PIN);
  gpio_deep_sleep_hold_en();
  if (screenOn) tft.sleep();
  inaPowerDown();
  esp_sleep_enable_timer_wakeup(5ULL * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void lowBatShutdown() {
  stopSpeaker();
  forceLandscape();
  cv = &tft;
  cv->fillScreen(UI_BG);
  ledcWrite(BACKLIGHT_PIN, 120);
  cv->setTextDatum(MC_DATUM);
  cv->drawRoundRect(110, 40, 92, 44, 8, sevColor(0));
  cv->fillRect(202, 52, 8, 20, sevColor(0));
  cv->fillRect(116, 46, 12, 32, sevColor(0));
  cv->setTextColor(sevColor(0), UI_BG);
  drawFreeTitle(L("Pil bitti", "Battery empty"), 160, 120, &FreeSansTR9pt);
  cv->setTextColor(TFT_WHITE, UI_BG);
  drawFreeTitle(L("Lütfen şarja takın", "Please plug in the charger"), 160, 156, &FreeSansTR7pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  drawFreeTitle(L("Şarja takılınca birkaç dakika içinde kendiliğinden açılır.", "It turns on by itself a few minutes after plugging in."), 160, 190, &FreeSansTR6pt);
  cv->setTextDatum(TL_DATUM);
  localHistSaveAll();   // 4.0
  delay(5000);
  cv->fillScreen(TFT_BLACK);
  lowBatSleepNow(true);
}

void checkLowBattery() {
  if (!inaPresent || batteryPercent < 0) return;
  {
    static unsigned long lowSince = 0;
    if (!batteryCharging && batteryMv >= 2800 && batteryMv <= 3300) {
      if (!lowSince) lowSince = millis();
      if (millis() - lowSince > 60000UL) lowBatShutdown();
    } else lowSince = 0;
  }
  bool low = batteryPercent <= 20 && !batteryCharging;
  if (low && !lowBatWarned) {
    lowBatWarned = true;
    lowBatDim = true;
    lowBatStep = lowBatStepFor(batteryPercent);
    applyBrightness();
    playAgeWarnBeep();                       // tek kisa bip (sessizde calmaz)
    lowBatPopup(L("Ekran kısıldı - şarja takın", "Screen dimmed - please charge"));
  } else if (low && lowBatStepFor(batteryPercent) > lowBatStep) {
    // 3.4.0: %15, %10 ve %5'te tekrar tek bip + yazi
    lowBatStep = lowBatStepFor(batteryPercent);
    playAgeWarnBeep();
    lowBatPopup(lowBatStep >= 4 ? L("Cihaz yakında kapanacak - şarja takın", "Shutting down soon - please charge")
                                : L("Şarja takın", "Please charge"));
  } else if (!low && (batteryCharging || batteryPercent >= 25)) {
    lowBatWarned = false;
    lowBatStep = 0;
    if (lowBatDim) { lowBatDim = false; applyBrightness(); }
  }
}

// Hata ekrani aciksa onu, degilse normal ekrani yeniden ciz
void redrawNow() {
  if (showErrorScreen) drawErrorScreenFull();
  else drawCurrentScreen();
}

// Tam ekran yatay cizilen sayfalar (acilis, kurulum, kilit, uyarilar) icin
void forceLandscape() {
  tft.setRotation((landRot + 3) % 4);
  tftEffRot = (landRot + 2) % 4;
}

// =====================================================================
//  3.0.0 - MEDTRUM (EasyView / EasyFollow takipci hesabi)
//  Medtrum uygulamasinda "Takip / Paylas" ile davet edilen EasyFollow hesabinin
//  e-posta ve sifresiyle giris yapilir (nightscout-clock / nightscout-easyview yontemi).
// =====================================================================
#define MED_BASE "https://easyview.medtrum.eu/mobile/ajax"

String medUrlEnc(const String& s) {
  String o; char b[4];
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { sprintf(b, "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

void medHeaders(HTTPClient& h) {
  h.addHeader("DevInfo", "Android 12;Xiamoi vayu;Android 12");
  h.addHeader("AppTag", "v=1.2.70(112);n=eyfo;p=android");
  h.addHeader("User-Agent", "okhttp/3.5.0");
  if (medCookie.length()) h.addHeader("Cookie", medCookie);
}

// 0: tamam, 3: baglanti, 2: giris reddedildi
int medLogin() {
  medCookie = "";
  HTTPClient http;
  
  http.begin(caReady(), MED_BASE "/login");
  http.setTimeout(12000);
  http.setConnectTimeout(6000);
  medHeaders(http);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const char* keys[] = {"Set-Cookie"};
  http.collectHeaders(keys, 1);
  String body = "apptype=Follow&user_name=" + medUrlEnc(medEmail) + "&password=" + medUrlEnc(medPass) + "&platform=google&user_type=M";
  int code = netPOST(http, body);
  medLastCode = code;
  String resp = netBody(http);
  String ck = http.header("Set-Cookie");
  http.end();
  if (code <= 0) return 3;
  if (code != 200) return (code >= 500) ? 3 : 2;
  DynamicJsonDocument d(2048);
  if (jsonParse(d, resp)) return 3;
  String res = d["res"] | "";
  if (res != "OK") return 2;
  // Set-Cookie: birden fazla cerez virgulle birlesmis olabilir; tum ad=deger ciftlerini al
  String out;
  int st = 0;
  while (st < (int)ck.length()) {
    int e1 = ck.indexOf(';', st), e2 = ck.indexOf(',', st);
    int e = (e1 < 0) ? e2 : (e2 < 0 ? e1 : min(e1, e2));
    if (e < 0) e = ck.length();
    String seg = ck.substring(st, e); seg.trim();
    String lo = seg; lo.toLowerCase();
    if (seg.indexOf('=') > 0 && !lo.startsWith("path=") && !lo.startsWith("expires=") && !lo.startsWith("domain=") &&
        !lo.startsWith("max-age=") && !lo.startsWith("samesite=")) {
      if (out.length()) out += "; ";
      out += seg;
    }
    st = e + 1;
  }
  medCookie = out;
  return out.length() ? 0 : 2;
}

// grafige olcum ekle (yaklasik 5 dk aralik; son nokta hep guncel)
void medPush(int mg, uint32_t ts) {
  if (mg <= 20 || ts < 1600000000UL) return;
  int n = historyCount;
  if (n > 0 && ts <= historyTsSec[n - 1]) {
    if (ts == historyTsSec[n - 1]) history[n - 1] = mg;
    return;
  }
  if (n >= 2 && ts - historyTsSec[n - 2] < 290) {      // araya sik olcum: son noktayi guncelle
    history[n - 1] = mg; historyTsSec[n - 1] = ts;
    historyDay[n - 1] = dayStampFromMs((uint64_t)ts * 1000ULL);
    return;
  }
  if (n >= HIST_MAX) {
    memmove(history, history + 1, (HIST_MAX - 1) * sizeof(history[0]));
    memmove(historyTsSec, historyTsSec + 1, (HIST_MAX - 1) * sizeof(historyTsSec[0]));
    memmove(historyDay, historyDay + 1, (HIST_MAX - 1) * sizeof(historyDay[0]));
    n = HIST_MAX - 1;
  }
  history[n] = mg; historyTsSec[n] = ts;
  historyDay[n] = dayStampFromMs((uint64_t)ts * 1000ULL);
  historyCount = n + 1;
}

int medToMg(double g) { return (g < 30.0) ? (int)(g * 18.0 + 0.5) : (int)(g + 0.5); }

// gecmis olcumler (4 saatlik parcalar halinde)
void medFillHistory(const String& user, uint32_t from, uint32_t to) {
  medFillOk = true;
  if (!user.length() || to <= from + 60) return;
  for (uint32_t f = from; f < to; f += 4UL * 3600UL) {
    uint32_t t2 = f + 4UL * 3600UL; if (t2 > to) t2 = to;
    char st[32], et[32];
    time_t a = (time_t)f, b = (time_t)t2;
    strftime(st, sizeof(st), "%Y-%m-%d%%20%H:%M:%S", gmtime(&a));
    strftime(et, sizeof(et), "%Y-%m-%d%%20%H:%M:%S", gmtime(&b));
    HTTPClient http;
    
    http.begin(caReady(), String(MED_BASE "/download?flag=sg&st=") + st + "&et=" + et + "&user_name=" + medUrlEnc(user));
    http.setTimeout(12000);
    http.setConnectTimeout(6000);
    medHeaders(http);
    int code = netGET(http);
    if (code != 200) { http.end(); medFillOk = false; return; }
    String resp = netBody(http);
    http.end();
    DynamicJsonDocument doc(40960);
    if (jsonParse(doc, resp)) { medFillOk = false; return; }
    String res = doc["res"] | "";
    if (res != "OK") { medFillOk = false; return; }
    // zamana gore sirala (sunucu sirasina guvenme), sonra ekle
    static uint32_t tsA[200]; static int16_t mgA[200];
    int m = 0;
    for (JsonVariant e : doc["data"].as<JsonArray>()) {
      if (m >= 200) break;
      uint64_t ts = e[1].as<uint64_t>();
      if (ts > 100000000000ULL) ts /= 1000ULL;
      int mg = medToMg(e[3].as<double>());
      if (ts < 1600000000ULL || mg <= 20) continue;
      int k = m++;
      while (k > 0 && tsA[k - 1] > (uint32_t)ts) { tsA[k] = tsA[k - 1]; mgA[k] = mgA[k - 1]; k--; }
      tsA[k] = (uint32_t)ts; mgA[k] = mg;
    }
    if (medResetPending) { historyCount = 0; medResetPending = false; }
    for (int i = 0; i < m; i++) medPush(mgA[i], tsA[i]);
  }
}

bool fetchMedtrumTry(bool again) {
  if (medEmail.length() == 0 || medPass.length() == 0) { setFetchError(5, "Medtrum e-posta/sifre"); return false; }
  if (medCookie.length() == 0) {
    int r = medLogin();
    if (r == 3) { setFetchError(3, "Medtrum baglanti" + (medLastCode != 0 ? String(" HTTP ") + String(medLastCode) : String(""))); return false; }
    if (r == 2) { setFetchError(2, "Medtrum giris"); return false; }
  }
  HTTPClient http;
  
  http.begin(caReady(), MED_BASE "/monitor?flag=monitor_list");
  http.setTimeout(12000);
  http.setConnectTimeout(6000);
  medHeaders(http);
  int code = netGET(http);
  String resp = (code > 0) ? netBody(http) : String("");
  http.end();
  if (code <= 0) { setFetchError(3, "Medtrum baglanti"); return false; }
  if (code == 401 || code == 403) {
    medCookie = "";
    if (!again) return fetchMedtrumTry(true);
    setFetchError(2, "Medtrum oturum"); return false;
  }
  if (code != 200) { setFetchError(3, "Medtrum HTTP " + String(code)); return false; }
  StaticJsonDocument<256> filter;
  filter["res"] = true;
  filter["monitorlist"][0]["username"] = true;
  filter["monitorlist"][0]["sensor_status"]["glucose"] = true;
  filter["monitorlist"][0]["sensor_status"]["glucoseRate"] = true;
  filter["monitorlist"][0]["sensor_status"]["updateTime"] = true;
  filter["monitorlist"][0]["sensor_status"]["status"] = true;
  DynamicJsonDocument doc(4096);
  if (jsonParseF(doc, resp, filter)) { setFetchError(6, "Medtrum JSON"); return false; }
  String res = doc["res"] | "";
  if (res != "OK") {                                   // oturum dustu: bir kez yeniden giris
    medCookie = "";
    if (!again) return fetchMedtrumTry(true);
    setFetchError(2, "Medtrum oturum"); return false;
  }
  JsonArray ml = doc["monitorlist"].as<JsonArray>();
  if (ml.isNull() || ml.size() == 0) { setFetchError(4, "Medtrum takip yok"); return false; }
  JsonObject ss = ml[0]["sensor_status"];
  if (ss.isNull()) { setFetchError(4, "Medtrum sensor yok"); return false; }
  String user = ml[0]["username"] | "";
  int mg = medToMg(ss["glucose"].as<double>());
  int rate = ss["glucoseRate"].isNull() ? 0 : (int)ss["glucoseRate"].as<double>();
  int stat = ss["status"].isNull() ? 3 : (int)ss["status"].as<double>();
  uint64_t ts = (uint64_t)ss["updateTime"].as<double>();
  if (ts > 100000000000ULL) ts /= 1000ULL;
  if (ts < 1600000000ULL) { setFetchError(6, "Medtrum zaman"); return false; }
  if (stat == 2 || mg <= 20) { setFetchError(4, stat == 2 ? "Medtrum isiniyor" : "Medtrum SGV"); return false; }
  switch (rate) {
    case 1: direction = "FortyFiveUp"; break;
    case 2: direction = "SingleUp"; break;
    case 3: direction = "DoubleUp"; break;
    case 4: direction = "FortyFiveDown"; break;
    case 5: direction = "SingleDown"; break;
    case 6: direction = "DoubleDown"; break;
    default: direction = "Flat"; break;
  }
  // grafik: ilk seferde / 10 dk'dan uzun bosluk varsa gecmisi indir (en fazla 24 saat)
  uint32_t lastTs = historyCount > 0 ? historyTsSec[historyCount - 1] : 0;
  unsigned long& lastHistTry = medLastHistTry;   // 4.0: kisi basina
  if ((uint32_t)time(nullptr) > 1600000000UL && (!medHistOk || lastTs == 0 || (uint32_t)ts > lastTs + 600) &&
      (lastHistTry == 0 || millis() - lastHistTry > 600000UL)) {
    lastHistTry = millis();                            // basarisizsa 10 dk sonra tekrar dene
    if (!medHistOk) { medResetPending = true; lastTs = 0; }  // 3.4.3: yarim kaldi: 24 saat bastan (ilk basarili parcada silinir)
    uint32_t from = (uint32_t)ts - 86400UL;
    if (lastTs > from) from = lastTs + 1;
    medFillHistory(user, from, (uint32_t)ts);
    medHistOk = medFillOk; medResetPending = false;
  }
  medPush(mg, (uint32_t)ts);
  sgv = mg;
  delta = 0; deltaInterp = false; deltaUnknown = false;
  for (int i = historyCount - 2; i >= 0; i--) {
    uint32_t age = (uint32_t)ts - historyTsSec[i];
    if (age >= 240) { delta = deltaGap(mg - history[i], age); break; }   // 3.6.1
  }
  markNewReading(ts * 1000ULL, String((uint32_t)ts));
  sensorAgeHours = -1; cannulaAgeHours = -1;
  nsIob = -1; nsCob = -1; bolusCount = 0; graphDetailOpen = false;
  clearFetchError();
  return true;
}
bool fetchMedtrum() { return fetchMedtrumTry(false); }

// ---------------------------------------------------------------- 3.3.0 bekci (donma olursa kendini resetler)
#include <esp_task_wdt.h>
bool wdtOn = false;
void wdtStart() {
  wdtOn = true;
  esp_task_wdt_config_t c;
  c.timeout_ms = 150000;                 // 2,5 dk tepkisiz kalirsa (en uzun veri cekme ~1 dk)
  c.idle_core_mask = 0;
  c.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&c) != ESP_OK) esp_task_wdt_init(&c);
  esp_task_wdt_add(NULL);
}
void wdtFeed() {
  if (!wdtOn) return;
  if (loopTaskH && xTaskGetCurrentTaskHandle() != loopTaskH) return;   // sadece ana dongu besler
  if (fetchStuck()) return;                                            // veri gorevi takildi: reset
  esp_task_wdt_reset();
}
void wdtPause() { if (wdtOn) esp_task_wdt_delete(NULL); }
void wdtResume() { if (wdtOn) { esp_task_wdt_add(NULL); esp_task_wdt_reset(); } }

// Donma / cokme sonrasi mi acildik
bool crashReset() {
  esp_reset_reason_t r = esp_reset_reason();
  return r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT || r == ESP_RST_WDT || r == ESP_RST_PANIC;
}

// Hizli acilis ekrani (melodi, logo ve uyari ekrani yok)
void showQuickBootScreen() {
  forceLandscape();
  cv = &tft;
  cv->fillScreen(UI_BG);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(MC_DATUM);
  drawFreeTitle("MeteMatik", 160, 80, &FreeSansTR9pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  drawFreeTitle(L("Yeniden başlatıldı, bağlanıyor...", "Restarted, connecting..."), 160, 120, &FreeSansTR7pt);
  cv->setTextDatum(TL_DATUM);
}

void drawSettingsScreen() {
  if (textPageActive) { drawTextPage(); return; }
  if (timePickerActive) { drawTimePicker(); return; }
  if (showGuide) { drawGuidePage(); return; }
  if (subScreen == 1 && alarmEditStep == 0) { drawAlarmList(); return; }
  if (subScreen == 19) { drawModesList(); return; }
  if (subScreen == 20) { drawSleepList(); return; }
  if (subScreen == 21) { drawMicTestScreen(); return; }
  if (subScreen == 1 || subScreen == 18) { drawAlarmSetupScreen(); return; }
  if (subScreen == 2) { drawSoundScreen(); return; }
  if (subScreen == 3) { drawResetScreen(); return; }
  if (subScreen == 4) { drawInfoScreen(); return; }
  if (subScreen == 5) { drawLangUnitScreen(); return; }
  if (subScreen == 6) { drawBrightnessScreen(); return; }
  if (subScreen == 7) { drawQrScreen(); return; }
  if (subScreen == 31) { drawFamQrScreen(); return; }   // 4.0.1
  if (subScreen == 8) { drawUiModeScreen(); return; }
  if (subScreen == 9) { drawRotationScreen(); return; }
  if (subScreen == 10) { drawGeceScreen(); return; }
  if (subScreen == 13) { drawOkulScreen(); return; }
  if (subScreen == 14) { drawDigerScreen(); return; }
  if (subScreen == 15) { drawSourcesScreen(); return; }
  if (subScreen == 16) { drawTeacherScreen(); return; }
  if (subScreen == 0) { drawSettingsList(); return; }

  // (2.1.0 oncesi 6 kutulu ayar ekrani - artik kullanilmiyor, yedek olarak duruyor)
  cv->fillScreen(UI_BG);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("AYARLAR", "SETTINGS"), 160, 8, &FreeSansTR9pt);

  // WiFi gosterge (sol ust) — sayi yok, ustuste binmesin
  cv->setTextDatum(TL_DATUM);
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    uint16_t wcol = (rssi > -60) ? sevColor(2) : (rssi > -75) ? sevColor(1) : sevColor(0);
    int bars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : (rssi > -85) ? 1 : 0;
    for (int b = 0; b < 4; b++) {
      int h = 5 + b * 3;
      uint16_t c = (b < bars) ? wcol : 0x3186;
      cv->fillRect(8 + b * 5, 18 - h, 4, h, c);
    }
    cv->setTextColor(wcol, UI_BG);
    dsN(cv, "WiFi", 32, 8, 2);
  } else {
    cv->setTextColor(sevColor(0), UI_BG);
    drawFreeTitle(L("WiFi yok", "No WiFi"), 8, 8, &FreeSansTR7pt);
  }

  // Saat gostergesi (sag ust) - WiFi gostergesiyle ayni hizada
  {
    struct timeval tv; gettimeofday(&tv, nullptr);
    if (tv.tv_sec > 1600000000) {
      time_t local = tv.tv_sec + 3 * 3600;
      struct tm* tmv = gmtime(&local);
      char clk[8];
      sprintf(clk, "%02d:%02d", tmv->tm_hour, tmv->tm_min);
      cv->setTextColor(TFT_LIGHTGREY, UI_BG);
      cv->setTextDatum(TR_DATUM);
      dsN(cv, clk, 312, 8, 2);
    }
  }

  // UST: Parlaklik | Ses | Alarm
  const int ty = 36, th = 52, tw = 96;
  const int iconY = ty + 15, labelY = ty + 38;

  cv->fillRoundRect(10, ty, tw, th, 8, 0x333333);
  cv->drawRoundRect(10, ty, tw, th, 8, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x333333);
  cv->setTextDatum(MC_DATUM);
  drawBrightnessLevelIcon(58, iconY, TFT_WHITE, brightnessLevel);
  drawFreeTitle(L("Parlaklık", "Bright"), 58, labelY, &FreeSansTR7pt);

  cv->fillRoundRect(112, ty, tw, th, 8, 0x2945);
  cv->drawRoundRect(112, ty, tw, th, 8, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x2945);
  if (soundLevel == 0) drawMuteIcon(160, iconY, TFT_WHITE);
  else drawSoundLevelIcon(160, iconY, TFT_WHITE, soundLevel - 1);
  drawFreeTitle(L("Ses", "Sound"), 160, labelY, &FreeSansTR7pt);

  cv->fillRoundRect(214, ty, tw, th, 8, getAlarmColor());
  cv->drawRoundRect(214, ty, tw, th, 8, 0x4A69);
  cv->setTextColor(TFT_BLACK, getAlarmColor());
  drawBellIcon(262, iconY, TFT_BLACK);
  drawFreeTitle(L("Alarm", "Alarm"), 262, labelY, &FreeSansTR7pt);

  // ALT: Hedefler | QR/Web | Diger
  const int by = 110, bh = 52, bw = 96;
  const int iconY2 = by + 15, labelY2 = by + 38;

  cv->fillRoundRect(10, by, bw, bh, 8, 0x0A60);
  cv->drawRoundRect(10, by, bw, bh, 8, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x0A60);
  drawTargetIcon(58, iconY2, TFT_WHITE);
  drawFreeTitle(L("Hedefler", "Targets"), 58, labelY2, &FreeSansTR7pt);

  cv->fillRoundRect(112, by, bw, bh, 8, 0x780F);
  cv->drawRoundRect(112, by, bw, bh, 8, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x780F);
  drawQrIcon(160, iconY2, TFT_WHITE);
  drawFreeTitle(L("QR / Web", "QR / Web"), 160, labelY2, &FreeSansTR7pt);

  cv->fillRoundRect(214, by, bw, bh, 8, 0x4B72);
  cv->drawRoundRect(214, by, bw, bh, 8, 0x4A69);
  cv->setTextColor(TFT_WHITE, 0x4B72);
  drawMoreIcon(262, iconY2, TFT_WHITE);
  drawFreeTitle(L("Diğer", "More"), 262, labelY2, &FreeSansTR7pt);

  cv->setTextColor(TFT_DARKGREY, UI_BG);
  cv->setTextDatum(BC_DATUM);
  drawFreeTitle(L("Boş alana dokun: sayfa değiştir", "Tap empty area: change page"), 160, 232, &FreeSansTR6pt);
}


void updateTimerCountdownOnly() {
  if (!timerRunning || !isTimerScreen()) return;
  long left = (long)(timerEndMs - millis());
  if (left < 0) left = 0;
  int sec = (int)(left / 1000);
  int mm = sec / 60, ss = sec % 60;
  char buf[16];
  sprintf(buf, "%02d:%02d", mm, ss);
  // Not: bu alan eskiden x=150'den basliyordu; ama sol "Anlik seker" kartinin
  // yuvarlatilmis cercevesi x=156'da bitiyor, yani 150-156 araligi o kartin
  // sag kenar cizgisinin ustune biniyordu. Sayac her saniye yenilendiginde bu
  // dar serit siyahla siliniyor, boylece hem zamanlayici hem "Anlik seker"
  // kartinin cercevesi "bozuluyormus" gibi goruntu veriyordu. Artik sadece
  // sag (Zamanlayici) kartinin ic alani temizleniyor.
  cv->fillRect(166, 70, 144, 55, UI_BG);
  cv->setTextColor(sevColor(1), UI_BG);
  cv->setTextDatum(MC_DATUM);
  cv->setTextSize(2);
  dsN(cv, buf, 235, 95, 4);
  cv->setTextSize(1);
}

void drawTimerScreen() {
  uint16_t accent = cv->color565(0x4A, 0x8C, 0xE0); // Ertele ile ayni marka mavisi
  cv->fillScreen(UI_BG);
  drawBackChevron(14, 17);
  cv->setTextColor(TFT_CYAN, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(L("ZAMANLAYICI", "TIMER"), 160, 8, &FreeSansTR9pt);
  cv->drawFastHLine(10, 34, 300, TFT_DARKGREY);

  char buf[16];
  // Sol kart: anlik seker
  cv->drawRoundRect(8, 42, 148, 98, 10, TFT_DARKGREY);
  drawForkIcon(24, 58, TFT_DARKGREY);
  cv->setTextColor(TFT_DARKGREY, UI_BG);
  cv->setTextDatum(TL_DATUM);
  drawFreeTitle(L("Anlık şeker", "Current glucose"), 40, 52, &FreeSansTR6pt);
  if (dataValid) {
    { String _gs = formatGlucose(sgv); strncpy(buf, _gs.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0; }
    cv->setTextColor(getGlucoseColor(sgv), UI_BG);
    cv->setTextDatum(MC_DATUM);
    cv->setTextSize(2);
    dsN(cv, buf, 80, 95, 4);
    cv->setTextSize(1);
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    dsN(cv, unitLabel(), 80, 128, 1);
  } else {
    cv->setTextColor(sevColor(0), UI_BG);
    cv->setTextDatum(MC_DATUM);
    dsN(cv, "--", 80, 95, 4);
  }

  // Sag kart: zamanlayici
  cv->drawRoundRect(164, 42, 148, 98, 10, TFT_DARKGREY);
  drawClockIcon(180, 58, TFT_DARKGREY);
  cv->setTextColor(TFT_DARKGREY, UI_BG);
  cv->setTextDatum(TL_DATUM);
  drawFreeTitle(L("Zamanlayıcı", "Timer"), 196, 52, &FreeSansTR6pt);

  if (timerRunning) {
    long left = (long)(timerEndMs - millis());
    if (left < 0) left = 0;
    int sec = (int)(left / 1000);
    int mm = sec / 60, ss = sec % 60;
    sprintf(buf, "%02d:%02d", mm, ss);
    cv->setTextColor(sevColor(1), UI_BG);
    cv->setTextDatum(MC_DATUM);
    cv->setTextSize(2);
    dsN(cv, buf, 235, 95, 4);
    cv->setTextSize(1);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("kalan", "left"), 235, 128, &FreeSansTR6pt);

    cv->fillRoundRect(70, 165, 180, 48, 14, 0x8000);
    cv->drawRoundRect(70, 165, 180, 48, 14, sevColor(0));
    drawXIcon(160, 177, TFT_WHITE);
    cv->setTextColor(TFT_WHITE, 0x8000);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("İPTAL", "CANCEL"), 160, 198, &FreeSansTR7pt);
  } else if (dataValid && sgv < alarmLow) {
    cv->setTextColor(sevColor(0), UI_BG);
    cv->setTextDatum(TL_DATUM);
    char hb[64]; snprintf(hb, sizeof(hb), L("<%s: önce hipoyu tedavi et", "<%s: treat the low first"), formatGlucose(alarmLow).c_str());
    drawFreeTitle(hb, 12, 145, &FreeSansTR7pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(L("Şeker/glukoz tablet/meyve suyu,", "Treat low first (tabs/juice),"), 12, 172, &FreeSansTR6pt);
    drawFreeTitle(L("10 dk bekle; bolusu yemekle yap.", "wait 10 min; bolus with the meal."), 12, 190, &FreeSansTR6pt);
    drawFreeTitle(L("Bu durumda zamanlayıcı başlamaz.", "Timer will not start in this case."), 12, 208, &FreeSansTR6pt);
  } else if (dataValid) {
    int mins = sgv / 10;
    if (mins < 1) mins = 1;
    snprintf(buf, sizeof(buf), L("%d dk", "%d min"), mins);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(MC_DATUM);
    cv->setTextSize(2);
    dsN(cv, buf, 235, 95, 4);
    cv->setTextSize(1);
    cv->setTextColor(TFT_DARKGREY, UI_BG);
    drawFreeTitle(L("(şeker / 10)", "(glucose / 10)"), 235, 128, &FreeSansTR6pt);

    cv->fillRoundRect(70, 165, 180, 48, 14, 0x0B5E);
    cv->drawRoundRect(70, 165, 180, 48, 14, accent);
    cv->fillTriangle(154, 168, 154, 182, 166, 175, TFT_WHITE);
    cv->setTextColor(TFT_WHITE, 0x0B5E);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("BAŞLAT", "START"), 160, 198, &FreeSansTR7pt);
  } else {
    cv->setTextColor(sevColor(0), UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("Veri yok", "No data"), 160, 160, &FreeSansTR9pt);
  }
}

// Genel melodi calici: seq[i] = {frekans_Hz, sure_ms}. frekans 0 ise sadece sessizlik.
// Notlar kisa duty ile calinir (ton/3 calar, kalani + 40ms bosluk) -- yumusak/hafif bir tını icin.
void playMelody(const int seq[][2], int count) {
  for (int i = 0; i < count; i++) {
    if (seq[i][0] > 0) {
      spkTone(seq[i][0]);
      delay(seq[i][1] / 3);          // ton cok kisa = yumusak his
      noTone(SPEAKER_PIN);
      digitalWrite(SPEAKER_PIN, LOW);
      delay(seq[i][1] * 2 / 3 + 40); // uzun bosluk
    } else {
      delay(seq[i][1]);
    }
  }
}

// Acilis melodisi: kisa, yukselen ozgun bir arpej (C5-E5-G5-C6-G5-C6)
void playBootChime() {
  const int seq[][2] = {
    {523, 150}, {659, 150}, {784, 150}, {1047, 320},
    {0, 60},
    {784, 150}, {1047, 420}
  };
  playMelody(seq, 7);
}

// Zamanlayici/yemek bitti melodisi: neseli, kisa inip-cikan ozgun bir motif
void playMealChime() {
  const int seq[][2] = {
    {784, 130}, {988, 130}, {1319, 260},
    {0, 50},
    {1175, 130}, {988, 130}, {784, 380}
  };
  playMelody(seq, 7);
}

void playTimerDoneMelody() {
  playMealChime();
}

void serviceTimer() {
  if (!timerRunning) return;
  if ((long)(millis() - timerEndMs) < 0) return;
  if (currentScreen == SCR_VIDEO || teacherStep > 0) return;   // 3.4.3: acil akis bitince gosterilir
  timerRunning = false;
  timerFinishedPending = true;

  // Hangi sayfa aciksa: tam ekran uyari (sonra eski sayfaya don)
  forceLandscape();
  for (int i = 0; i < 3; i++) {
    cv->fillScreen(sevColor(1));
    delay(120);
    cv->fillScreen(0x0320);
    delay(100);
  }
  cv->fillScreen(0x0320); // koyu yesil zemin
  drawForkIcon(160, 24, TFT_WHITE);
  drawSparkleIcon(40, 24, sevColor(1));
  drawSparkleIcon(280, 24, sevColor(1));
  cv->setTextColor(TFT_WHITE, 0x0320);
  cv->setTextDatum(MC_DATUM);
  cv->setTextSize(2);
  drawFreeTitle(L("AFİYET", "ENJOY"), 160, 70, &FreeSansTR9pt);
  drawFreeTitle(L("OLSUN", "YOUR MEAL"), 160, 120, &FreeSansTR9pt);
  cv->setTextSize(1);
  cv->fillRoundRect(90, 158, 140, 26, 13, sevColor(1));
  cv->setTextColor(0x0320, sevColor(1));
  drawFreeTitle(L("Süre bitti", "Time is up"), 160, 171, &FreeSansTR7pt);
  cv->setTextColor(TFT_WHITE, 0x0320);
  drawFreeTitle(L("Yemeğe başlayabilirsiniz", "You may start eating"), 160, 205, &FreeSansTR7pt);

  // Zamanlayici bitis alarmi biraz daha uzun calsin diye melodi 2 kez calinip
  // bekleme suresi uzatildi (kullanici istegiyle).
  playTimerDoneMelody();
  delay(300);
  playTimerDoneMelody();
  delay(2200);

  timerFinishedPending = false;
  // Uyari + ses sonrasi ana ekrana don
  formAbandon(); textPageActive = false; timePickerActive = false;
  currentScreen = homeScreenIdx();
  subScreen = 0;
  showGuide = false;
  drawCurrentScreen();
}



// Kurulum agi sifresi: cihaz etiketindeki (fabrika) web sifresi
String portalApPass() {
  prefs.begin("metematik", true);
  String p = prefs.getString("webpw0", "");
  prefs.end();
  if (p.length() < 8) p = webPass;
  return p;
}

void drawPwSpaced(const String& pw, int cx, int y, int cell, const lgfx::IFont* f, uint16_t col, uint16_t bg) {
  cv->setFont(f);
  cv->setTextColor(col, bg);
  cv->setTextDatum(TC_DATUM);
  int n = pw.length();
  int x0 = cx - n * cell / 2 + cell / 2;
  for (int i = 0; i < n; i++) { char c[2] = {pw[i], 0}; cv->drawString(c, x0 + i * cell, y); }
  cv->setTextFont(1);
  cv->setTextDatum(TL_DATUM);
}

String pwMask(const String& p) { String m; for (unsigned i = 0; i < p.length() && i < 40; i++) m += '*'; return m; }
// form degeri -> kayitli sifre: maske aynen geldiyse dokunma, bos ise sil, degilse yeni sifre
void applyPw(const String& v, String& target) {
  if (target.length() && v == pwMask(target)) return;
  target = v;
}

void showPortalScreen() {
  forceLandscape();
  cv->fillScreen(UI_BG);
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(TC_DATUM);
  bool bi = (langIdx == 0);          // 3.6.1: Turkcede kurulum ekrani Turkce + Ingilizce
  drawFreeTitle(bi ? "WiFi kurulumu / WiFi setup" : L("WiFi kurulumu", "WiFi setup"), 160, 4, &FreeSansTR9pt);
  // 1) kurulum agina baglan  2) kurulum sayfasini ac (telefon kendiliginden acmazsa)
  const char* qrs[2];
  String apPw = portalApPass();
  String wifiQr = (apPw.length() >= 8) ? ("WIFI:T:WPA;S:" + deviceName() + ";P:" + apPw + ";;") : ("WIFI:T:nopass;S:" + deviceName() + ";;");
  qrs[0] = wifiQr.c_str();
  qrs[1] = "http://192.168.4.1";
  const char* labs[2] = {L("1) Ağa bağlan", "1) Join network"), L("2) Sayfayı aç", "2) Open page")};
  if (bi) { labs[0] = "1) Ağa bağlan / Join WiFi"; labs[1] = "2) Sayfayı aç / Open page"; }
  for (int k = 0; k < 2; k++) {
    QRCode qrcode;
    uint8_t qrbuf[qrcode_getBufferSize(4)];
    qrcode_initText(&qrcode, qrbuf, 4, ECC_LOW, qrs[k]);
    int scale = 3, qsize = qrcode.size * scale;
    int cx = (k == 0) ? 82 : 238, ox = cx - qsize / 2, oy = 56;
    cv->setTextColor(sevColor(1), UI_BG);
    cv->setTextDatum(TC_DATUM);
    drawFreeTitle(labs[k], cx, 30, &FreeSansTR7pt);
    cv->fillRoundRect(ox - 6, oy - 6, qsize + 12, qsize + 12, 8, TFT_WHITE);
    for (uint8_t y = 0; y < qrcode.size; y++)
      for (uint8_t x = 0; x < qrcode.size; x++)
        if (qrcode_getModule(&qrcode, x, y)) cv->fillRect(ox + x * scale, oy + y * scale, scale, scale, TFT_BLACK);
  }
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle(deviceName().c_str(), 82, 170, &FreeSansTR7pt);
  drawFreeTitle("192.168.4.1", 238, 170, &FreeSansTR7pt);
  if (apPw.length() >= 8) {                         // 3.4.6: harfler ayri ayri, okunakli
    int cell = 13, pwW = apPw.length() * cell;
    cv->setFont((const lgfx::IFont*)&FreeSansTR7pt);
    const char* lab = bi ? "Şifre/Pass:" : L("Şifre:", "Pass:");
    int labW = cv->textWidth(lab);
    cv->setTextFont(1);
    int x0 = 82 - (labW + 6 + pwW) / 2;
    if (x0 < 2) x0 = 2;
    cv->setTextColor(sevColor(1), UI_BG);
    cv->setTextDatum(TL_DATUM);
    drawFreeTitle(lab, x0, 187, &FreeSansTR7pt);
    drawPwSpaced(apPw, x0 + labW + 6 + pwW / 2, 186, cell, &lgfx::fonts::FreeMonoBold9pt7b, sevColor(1), UI_BG);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(TC_DATUM);
  }
  cv->setTextColor(TFT_DARKGREY, UI_BG);
  if (bi) {
    drawFreeTitle("1. QR ile ağa bağlanın; sayfa açılmazsa 2. QR'ı okutun.", 160, 204, &FreeSansTR6pt);
    drawFreeTitle("Scan QR 1 to join; if no page opens, scan QR 2.", 160, 220, &FreeSansTR6pt);
  } else {
    drawFreeTitle(L("Önce 1. QR ile ağa bağlanın. Kurulum sayfası", "Join with QR 1 first. If the setup page"), 160, 204, &FreeSansTR6pt);
    drawFreeTitle(L("gecikirse 2. QR'ı okutun (bekleme yok).", "is slow to appear, scan QR 2."), 160, 220, &FreeSansTR6pt);
  }
  cv->setTextDatum(TL_DATUM);
}

// "Cihaz Sifirla": kayitli WiFi aglari + tum kaynak giris bilgileri silinir.
// Alarm esikleri ve ekran/ses ayarlari KORUNUR.
void factoryResetCredentials() {
  restoreFactoryWebPass();
  resetWebToken();
  for (int i = 0; i < MAX_WIFI; i++) { wifiSsid[i] = ""; wifiPass[i] = ""; }
  wifiCount = 0;
  nightscoutUrl = "https://";
  apiSecret = "";
  dexUser = ""; dexPass = "";
  libreEmail = ""; librePass = "";
  medEmail = ""; medPass = ""; medCookie = "";
  dexAccountId = ""; dexSessionId = "";
  libreToken = ""; libreAccountId = ""; librePatientId = "";
  dataValid = false;
  saveSettings();
  prefs.begin("metematik", false);
  for (int i = 0; i < MAX_WIFI; i++) {
    prefs.remove(("ws" + String(i)).c_str());
    prefs.remove(("wp" + String(i)).c_str());
  }
  prefs.end();
  WiFi.disconnect(true, true);   // ESP'nin kendi sakladigi agi da sil
  delay(150);
}

void startConfigPortal() {
  WiFi.setHostname(deviceName().c_str());
  stopOtaServer();
  showGuide = false; subScreen = 0;
  ensureWifiEvents();
  WiFi.disconnect(true, true); delay(150);
  WiFi.mode(WIFI_AP); WiFi.setSleep(false); delay(150);
  wifiDiscReason = 0;   // yeni kurulum: eski nedeni unut
  wifiSetupFailed = false;

  WiFiManager wm;
  wm.setTitle(WT("metematik Ayarlar"));
  wm.setConfigPortalTimeout(300);
  wm.setConnectTimeout(25);
  wm.setShowInfoUpdate(false);
  wm.setShowInfoErase(false);
  wm.setBreakAfterConfig(true);
  wm.setHostname(deviceName().c_str());
  wm.setDebugOutput(false);
  // WiFiManager kendi /update (yazilim yukleme), /u ve /erase adreslerini dugmesi gizli olsa da
  // sifresiz acik tutuyor. Bizim rotalarimiz onlardan ONCE kaydedilir ve kazanir: 403.
  wm.setWebServerCallback([&wm]() {
    auto deny = [&wm]() { wm.server->send(403, "text/plain; charset=utf-8", "Kapali"); };
    wm.server->on("/update", HTTP_ANY, deny);
    wm.server->on("/u", HTTP_ANY, deny, []() {});
    wm.server->on("/erase", HTTP_ANY, deny);
  });
  // Daha duzenli stil
  // Acik arka plan - WiFi listesi okunabilir
  // 3.6.0: dugme metni dile gore; Kiril vb. harfler bozulmasin diye utf-8 bildirimi eklendi
  static String wmHead;
  wmHead = String("<meta charset='utf-8'>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#f5f6f8!important;color:#222!important}"
    ".c{background:#fff;border-radius:12px;padding:12px;margin:8px 0;border:1px solid #ddd}"
    "input,select{border-radius:8px!important;padding:10px!important;background:#fff!important;color:#111!important;border:1px solid #ccc!important}"
    "button,.button{border-radius:8px!important;background:#1a73e8!important;color:#fff!important}"
    "h1,h2,h3{color:#1565c0!important}"
    "a{color:#1565c0}"
    ".wifi-item,.network{color:#111!important;background:#fff!important}"
    "</style>"
    // WiFiManager kutuphanesinin kok/menu sayfasindaki "Configure WiFi"
    // butonu sabit Ingilizce metin - kutuphane bunu degistirmek icin bir
    // API sunmuyor (setMenu sadece HANGI butonlarin gorunecegini secer,
    // metinlerini degil). Bu yuzden basit bir JS ile sayfa yuklendiginde
    // metni secili dile ceviriyoruz.
    "<script>"
    "var MM1='") + WT("Kuruluma Devam Et") + "',MM2='" + WT("Kuruluma Devam Et (Taramasiz)") + "';"
    "document.addEventListener('DOMContentLoaded',function(){"
    "document.querySelectorAll('a,button,input[type=submit]').forEach(function(el){"
    "var t=(el.textContent||el.value||'').trim();"
    "if(t==='Configure WiFi'){if(el.tagName==='INPUT')el.value=MM1;else el.textContent=MM1;}"
    "if(t==='Configure WiFi (No Scan)'){if(el.tagName==='INPUT')el.value=MM2;else el.textContent=MM2;}"
    "});"
    "});"
    "</script>";
  wm.setCustomHeadElement(wmHead.c_str());

  WiFiManagerParameter p_note(
    WT("<div class='c'><p style='margin:0;color:#ccc;font-size:0.95em'>"
    "Asagida <b>sadece bir kaynak</b> doldurun. "
    "Daha sonra http://IP/ adresinden de degistirebilirsiniz."
    "</p></div>")
  );
  WiFiManagerParameter p_hd_ns("<div class='c' style='border-left:4px solid #4a90d9'><b style='color:#4a90d9'>Nightscout</b></div>");
  WiFiManagerParameter p_ns("ns", "Nightscout URL", (nightscoutUrl.length() > 12 ? nightscoutUrl.c_str() : "https://"), 200);
  WiFiManagerParameter p_sec("apisec", "NS API Secret", pwMask(apiSecret).c_str(), 128);
  WiFiManagerParameter p_hd_dx(WT("<div class='c' style='border-left:4px solid #e67e22'><b style='color:#e67e22'>Dexcom</b><br><small>Kendi Dexcom hesabiniz (Follow degil)</small></div>"));
  WiFiManagerParameter p_dexu("dexu", WT("Dexcom kullanici adi / e-posta / telefon (+905XXXXXXXXX)"), dexUser.c_str(), 128);
  WiFiManagerParameter p_dexp("dexp", WT("Dexcom sifre"), pwMask(dexPass).c_str(), 128);
  WiFiManagerParameter p_dexs("dexs", WT("us veya ous"), dexServer.c_str(), 8);
  WiFiManagerParameter p_hd_lb(WT("<div class='c' style='border-left:4px solid #27ae60'><b style='color:#27ae60'>LibreLinkUp</b><br><small>Takipci (LibreLinkUp) hesabi</small></div>"));
  WiFiManagerParameter p_leu("leu", WT("Libre e-posta"), libreEmail.c_str(), 128);
  WiFiManagerParameter p_lep("lep", WT("Libre sifre"), pwMask(librePass).c_str(), 128);
  WiFiManagerParameter p_ler("ler", "EU / US / DE", libreRegion.c_str(), 8);
  WiFiManagerParameter p_hd_md(WT("<div class='c' style='border-left:4px solid #9b59b6'><b style='color:#9b59b6'>Medtrum</b><br><small>EasyFollow hesabi</small></div>"));
  WiFiManagerParameter p_mdu("mdu", WT("EasyFollow e-posta"), medEmail.c_str(), 128);
  WiFiManagerParameter p_mdp("mdp", WT("EasyFollow sifre"), pwMask(medPass).c_str(), 128);

  wm.addParameter(&p_note);
  wm.addParameter(&p_hd_ns); wm.addParameter(&p_ns); wm.addParameter(&p_sec);
  wm.addParameter(&p_hd_dx); wm.addParameter(&p_dexu); wm.addParameter(&p_dexp); wm.addParameter(&p_dexs);
  wm.addParameter(&p_hd_lb); wm.addParameter(&p_leu); wm.addParameter(&p_lep); wm.addParameter(&p_ler);
  wm.addParameter(&p_hd_md); wm.addParameter(&p_mdu); wm.addParameter(&p_mdp);

  showPortalScreen();
  String oldNs = nightscoutUrl, oldDexU = dexUser, oldDexP = dexPass;
  String oldLibreE = libreEmail, oldLibreP = librePass;
  String oldMedE = medEmail, oldMedP = medPass;
  String apPw = portalApPass();
  wdtPause();
  bool ok = wm.startConfigPortal(deviceName().c_str(), apPw.length() >= 8 ? apPw.c_str() : NULL);
  wdtResume();
  medEmail = p_mdu.getValue();
  applyPw(String(p_mdp.getValue()), medPass);

  nightscoutUrl = p_ns.getValue(); nightscoutUrl.trim();
  applyPw(String(p_sec.getValue()), apiSecret);
  dexUser = p_dexu.getValue();
  applyPw(String(p_dexp.getValue()), dexPass);
  dexServer = p_dexs.getValue(); dexServer.toLowerCase();
  if (dexServer != "us") dexServer = "ous";
  libreEmail = p_leu.getValue();
  applyPw(String(p_lep.getValue()), librePass);
  cleanField(nightscoutUrl); cleanField(dexUser); cleanField(libreEmail); cleanField(medEmail);
  libreRegion = p_ler.getValue(); libreRegion.toUpperCase();
  // Sadece kimlik bilgileri gercekten degistiyse (veya kayitli bir kaynak
  // yoksa) yeniden algila - aksi halde kullanici sadece WiFi sifirlamak icin
  // bu ekrana girdiginde, NVS'de kalan eski/kullanilmayan bir kaynagin
  // (ornegin eskiden denenmis Libre bilgileri) aktif kaynagi sessizce
  // degistirmesini onluyoruz.
  bool credsChanged = (nightscoutUrl != oldNs) || (dexUser != oldDexU) || (dexPass != oldDexP) ||
                       (libreEmail != oldLibreE) || (librePass != oldLibreP) ||
                       (medEmail != oldMedE) || (medPass != oldMedP);
  if (credsChanged || (dataSource != "nightscout" && dataSource != "dexcom" && dataSource != "libre" && dataSource != "medtrum")) {
    detectDataSource();
  }

  if ((medEmail != oldMedE || medPass != oldMedP) && medEmail.length() > 3 && medPass.length() > 0) dataSource = "medtrum";
  if (WiFi.status() == WL_CONNECTED) addWifiCredential(WiFi.SSID(), WiFi.psk());
  else if (wm.getWiFiSSID(true).length()) addWifiCredential(wm.getWiFiSSID(true), wm.getWiFiPass(true));
  saveSettings();
  dexAccountId = ""; dexSessionId = ""; libreToken = ""; libreAccountId = ""; medCookie = "";
  if (currentScreen >= screenCount()) currentScreen = homeScreenIdx();
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  if (WiFi.status() == WL_CONNECTED) startOtaServer();

  bool newNet = wm.getWiFiSSID(true).length() > 0;
  wifiSetupFailed = newNet && WiFi.status() != WL_CONNECTED;
  cv->fillScreen(UI_BG);
  cv->setTextDatum(MC_DATUM);
  if (WiFi.status() == WL_CONNECTED) {
    cv->setTextColor(sevColor(2), UI_BG);
    drawFreeTitle(L("Kaydedildi, bağlandı", "Saved, connected"), 160, 100, &FreeSansTR9pt);
    delay(1000);
  } else if (wifiSetupFailed) {
    cv->setTextColor(sevColor(0), UI_BG);
    drawFreeTitle(L("WiFi'ye bağlanamadı", "Could not join WiFi"), 160, 92, &FreeSansTR9pt);
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    String nn = wm.getWiFiSSID(true);
    if (nn.length() > 24) nn = nn.substring(0, 24) + "..";
    drawFreeTitle(nn.c_str(), 160, 124, &FreeSansTR7pt);
    drawFreeTitle(wifiReasonIsAuth() ? L("Şifre yanlış olabilir", "Password may be wrong")
                                     : L("Ağ bulunamadı veya şifre yanlış", "Network not found or wrong password"), 160, 150, &FreeSansTR7pt);
    delay(2200);
  } else {
    cv->setTextColor(TFT_LIGHTGREY, UI_BG);
    drawFreeTitle(ok ? L("Kaydedildi", "Saved") : L("Kurulum kapandı", "Setup closed"), 160, 100, &FreeSansTR9pt);
    delay(1000);
  }
}

bool connectWifi(bool silent = false) {
  buildWifiMulti();
  if (wifiCount == 0) return false;
  if (!silent) {
    cv->fillScreen(UI_BG);
    cv->setTextColor(TFT_WHITE, UI_BG);
    cv->setTextDatum(MC_DATUM);
    drawFreeTitle(L("WiFi bağlanıyor...", "Connecting WiFi..."), 160, 120, &FreeSansTR9pt);
  }
  WiFi.setHostname(deviceName().c_str());   // modem listesinde metematik-XXXX gorunsun
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  unsigned long start = millis();
  while (millis() - start < 25000) {
    if (wifiMulti.run() == WL_CONNECTED) {
      addWifiCredential(WiFi.SSID(), WiFi.psk());
      return true;
    }
    delay(300);
  }
  return false;
}

// =====================================================================
//  2.5.0 - ACILIS: "Onemli uyari" sayfasinda durum yazisi + dolan yesil cubuk
//  Cubuk ayri bir gorevde (cekirdek 0) akici ilerler; bu sirada ana gorev
//  sadece ag islerini yapar, ekrana cizmez.
// =====================================================================
volatile int bootCap = 0;
volatile bool bootAnimRun = false;
volatile bool bootAnimAlive = false;
volatile int bootPctShown = 0;
const char* volatile bootMsg = "";

#define BOOT_BAR_X 32
#define BOOT_BAR_Y 224
#define BOOT_BAR_W 256
#define BOOT_BAR_H 8

void bootAnimTask(void*) {
  const char* lastMsg = nullptr;
  int lastW = -1;
  float p = 0;
  while (bootAnimRun) {
    int cap = bootCap;
    if (p < cap) {
      if (cap >= 100) p += max(2.0f, (cap - p) * 0.25f);
      else p += (cap - p) * 0.03f + 0.04f;       // hedefe yavasca yaklas (hic durmuyor gibi)
      if (p > cap) p = cap;
    }
    const char* m = bootMsg;
    if (m != lastMsg) {
      cv->fillRect(0, 198, 320, 22, UI_BG);
      cv->setTextColor(0xC618, UI_BG);
      cv->setTextDatum(MC_DATUM);
      drawFreeTitle(m, 160, 209, &FreeSansTR7pt);
      cv->setTextDatum(TL_DATUM);
      lastMsg = m;
    }
    int w = (int)(p * BOOT_BAR_W / 100.0f);
    if (lastW < 0) cv->fillRoundRect(BOOT_BAR_X, BOOT_BAR_Y, BOOT_BAR_W, BOOT_BAR_H, 4, 0x2945);
    if (w != lastW && w > 0) cv->fillRoundRect(BOOT_BAR_X, BOOT_BAR_Y, max(w, BOOT_BAR_H), BOOT_BAR_H, 4, sevColor(2));
    lastW = w;
    bootPctShown = (int)p;
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  bootAnimAlive = false;
  vTaskDelete(NULL);
}

void bootStage(const char* msg, int cap) {
  bootMsg = msg;
  bootCap = cap;
  if (!bootAnimAlive) {
    bootAnimRun = true;
    bootAnimAlive = true;
    if (xTaskCreatePinnedToCore(bootAnimTask, "bootbar", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
      bootAnimRun = false; bootAnimAlive = false;
    }
  }
}

void bootStop() {
  bootAnimRun = false;
  unsigned long t0 = millis();
  while (bootAnimAlive && millis() - t0 < 1000) delay(5);
}

void bootFinish() {
  bootStage(L("Hazır", "Ready"), 100);
  unsigned long t0 = millis();
  while (bootAnimAlive && bootPctShown < 100 && millis() - t0 < 2000) delay(10);
  delay(300);
  bootStop();
}

void showSplash() {
  forceLandscape();
  // Buyuk logo, uzun gosterim (~7 sn)
  // Not: yeni logo (damla + egilim noktalari + yazi)
  // gomuldu. Bu bitmap'in icinde "MeteMatik" yazisi zaten var, ayrica yazi
  // CIZMIYORUZ. Versiyon yazisi da kullanici talebiyle acilis ekranindan
  // kaldirildi. setSwapBytes gerekli - bu dizi Python/PIL ile RGB565'e
  // donusturuldu (eski, ilk-surumden kalma bitmap'in aksine).
  cv->fillScreen(UI_BG);
  int lx = (320 - LOGO_W) / 2;
  int ly = (240 - LOGO_H) / 2; // ekran tam yuksekligi (240) - dikey ortalama duzeltildi
  if (ly < 4) ly = 4;
  cv->setSwapBytes(true);
  cv->pushImage(lx, ly, LOGO_W, LOGO_H, metematik_logo, (uint16_t)0x0000); // siyah koseler seffaf
  cv->setSwapBytes(false);
  playBootChime();
  delay(2500);
}

void showBootWarning() {
  forceLandscape();
  uint16_t c = sevColor(3);
  cv->fillScreen(UI_BG);
  cv->drawRoundRect(8, 4, 304, 188, 16, c);          // 3.4.4: alt kenar y=192 (durum yazisi 198'den basliyor)
  cv->drawRoundRect(10, 6, 300, 184, 14, c);
  int cx = 160, cy = 30;
  cv->fillTriangle(cx, cy - 18, cx - 19, cy + 15, cx + 19, cy + 15, c);
  cv->fillRect(cx - 2, cy - 6, 5, 13, UI_BG);
  cv->fillCircle(cx, cy + 11, 2, UI_BG);
  cv->setTextDatum(TC_DATUM);
  cv->setTextColor(TFT_WHITE, UI_BG);
  drawFreeTitle(L("ÖNEMLİ UYARI", "IMPORTANT"), 160, 54, &FreeSansTR9pt);
  cv->setTextColor(c, UI_BG);
  drawFreeTitle(L("Tıbbi cihaz değildir", "Not a medical device"), 160, 82, &FreeSansTR9pt);
  cv->setTextColor(TFT_LIGHTGREY, UI_BG);
  drawFreeTitle(L("İnsülin ve tedavi kararları için", "For insulin and treatment decisions"), 160, 110, &FreeSansTR7pt);
  drawFreeTitle(L("resmi CGM uygulamanızı esas alın.", "rely on your official CGM app."), 160, 129, &FreeSansTR7pt);
  drawFreeTitle(L("Asıl CGM alarmlarınızı kapatmayın.", "Do not turn off your CGM alarms."), 160, 148, &FreeSansTR7pt);   // 3.6.1
  cv->setTextColor(TFT_DARKGREY, UI_BG);
  drawFreeTitle(L("Sorumluluk kullanıcıya aittir.", "Use at your own responsibility."), 160, 171, &FreeSansTR6pt);
  cv->setTextDatum(TL_DATUM);
}

// 3.6.1: ilk acilista (hic dil kaydedilmemis) once dil secimi - kurulum ekrani da secilen dilde gelir
void firstBootLangPick() {
  prefs.begin("metematik", true);
  bool has = prefs.isKey("lang");
  prefs.end();
  // 3.6.2: kurulumu yapilmamis cihaz (kayitli WiFi yok) her acilista dili sorar - yanlis secen geri donebilsin
  if ((has && wifiCount > 0) || !touchChipOk) return;
  forceLandscape();
  int keep = langIdx;
  if (!has) langIdx = LANG_COUNT;       // ilk kez: hicbiri secili gorunmesin; sonra: kayitli dil secili
  cv->fillScreen(UI_BG);
  cv->setTextColor(TFT_WHITE, UI_BG);
  cv->setTextDatum(TC_DATUM);
  drawFreeTitle("Dil / Language", 160, 14, &FreeSansTR9pt);
  drawLangPicker();
  cv->setTextDatum(TL_DATUM);
  int rx, ry, x, y, pick = -1;
  unsigned long t0 = millis();
  while (touchReadRaw(rx, ry) && millis() - t0 < 3000) delay(20);   // once parmak kalksin
  unsigned long limit = has ? 30000UL : 180000UL;   // dil zaten seciliyse 30 sn sonra kayitli dille devam
  while (pick < 0 && millis() - t0 < limit) {
    if (touchReadRaw(rx, ry)) {
      mapTouchToScreen(rx, ry, x, y);
      int c = (x - 8) / 102, r = (y - 48) / 62;
      if (y >= 48 && c >= 0 && c <= 2 && r >= 0 && r <= 2 && r * 3 + c < LANG_COUNT) pick = r * 3 + c;
    }
    delay(20);
  }
  if (pick < 0) { langIdx = keep; return; }          // secilmedi: sonraki acilista yine sorulur
  langIdx = pick;
  drawLangPicker();                                  // secimi goster
  prefs.begin("metematik", false); prefs.putInt("lang", langIdx); prefs.end();
  delay(400);
  while (touchReadRaw(rx, ry) && millis() - t0 < 200000UL) delay(20);
}

void setup() {
  Serial.begin(115200); delay(200);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  // "Cihazi Kapat" derin uykusundan kalan pin kilitlerini ac
  gpio_hold_dis((gpio_num_t)BACKLIGHT_PIN);
  gpio_hold_dis((gpio_num_t)AMP_CTRL_PIN);
  gpio_deep_sleep_hold_dis();
  pinMode(AMP_CTRL_PIN, OUTPUT);
  digitalWrite(AMP_CTRL_PIN, HIGH);          // hoparlor amfisi kapali (cit sesi olmasin)
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(SPEAKER_PIN, LOW);
  ledcAttach(BACKLIGHT_PIN, 5000, 8);
  ledcWrite(BACKLIGHT_PIN, 0);               // acilista siyah ekran gorunmesin
  // 3.4.16: "pil bitti" uykusundan uyandiysa: sarja takili degilse ekran acmadan tekrar uyu
  // (RTC bayragi korunmasa da: zamanlayiciyla uyanma sadece "pil bitti" uykusunda kurulur)
  if (lowBatSleepFlag == LOWBAT_MAGIC ||
      (esp_reset_reason() == ESP_RST_DEEPSLEEP && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER)) {
    lowBatSleepFlag = 0;
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
      Wire.begin(TP_SDA, TP_SCL, 400000);
      inaInit();
      bool wake = true;
      if (inaPresent) {
        delay(150);                           // ilk olcum (128 ornek ortalama)
        int16_t bus, sh;
        if (inaRead16(0x02, bus) && inaRead16(0x01, sh)) {
          int vbat = ((uint16_t)bus >> 3) * 4 + sh / 100;
          int ma = sh / 10;
          // 3.4.20: sadece sarjda / dolu pil takildi (>=3,9 V) / pil yok (USB). Yuk kalkinca dinlenen pil
          // 3,5-3,6 V'a cikar; eskiden bu yuzden sarja takilmadan acilip kapanma dongusu olabiliyordu.
          wake = (ma < -15) || vbat >= 3900 || vbat < 2800;
        }
      }
      if (!wake) lowBatSleepNow(false);
      wokeFromLowBat = true;
    }
  }
  tft.init(); loadSettings(); ensureWebCredentials(); thisChipId = getChipIdStr(); applyRotation(); cv->fillScreen(UI_BG);
  Serial.printf("Chip ID: %s\n", thisChipId.c_str());
  Wire.begin(TP_SDA, TP_SCL, 400000);
  touchInit();
  inaInit();
  {                                          // 3.4.19: varsayilan parlaklik pilli modelde Orta, pilsizde Yuksek
    prefs.begin("metematik", true);
    bool hasBright = prefs.isKey("bright");
    prefs.end();
    if (!hasBright && inaPresent) brightnessLevel = 1;
  }
  startMicTask();
  delay(120);                                // INA219 ilk olcum
  readBattery();
  // 3.4.20: acilista da pil kontrolu - pil bitmek uzereyse WiFi/ekran acilmadan kapan (acilis dongusu olmasin)
  if (inaPresent && batteryPercent >= 0 && !batteryCharging && batteryMv >= 2800 &&
      (batteryMv <= 3350 || (esp_reset_reason() == ESP_RST_BROWNOUT && batteryMv <= 3550)))
    lowBatShutdown();
  loadFotoFromFS();
  karbLoad();
  applyRotation();
  applyBrightness();

  if (!crashReset()) firstBootLangPick();   // 3.6.1
  // "Kurulum" ile yeniden baslatildiysa acilis ekranlarini atla, dogrudan kurulum
  forcePortalBoot = (portalOnBoot == PORTAL_MAGIC);
  portalOnBoot = 0;
  quickBoot = crashReset() && !forcePortalBoot;
  if (quickBoot) {
    // donma / cokme sonrasi otomatik yeniden baslatma: melodi, logo ve uyari ekrani yok
    prefs.begin("metematik", false);
    prefs.putUInt("crashcnt", prefs.getUInt("crashcnt", 0) + 1);
    prefs.putUInt("crashrsn", (uint32_t)esp_reset_reason());
    prefs.end();
    showQuickBootScreen();
  } else {
    // 1) Uzun splash (WiFi yazisi yok)
    if (!forcePortalBoot) showSplash();
    // 2) Uyari ekrani + durum yazisi + dolan yesil cubuk (WiFi/veri arka planda)
    showBootWarning();
  }
  localHistLoad();                        // 3.4.15: kapanmadan onceki kendi kayit
  initPersons();                          // 4.0: aile modu kisileri
  bootStage(L("İnternete bağlanıyor...", "Connecting to the internet..."), 45);
  bool wifiOk = forcePortalBoot ? false : connectWifi(true);
  // Kayitli ag varsa (modem gec acildi, elektrik kesintisi) kurulumu ACMA: hata ekrani +
  // arka planda yeniden baglanma. Kurulum sadece hic ag yoksa veya Kurulum / BOOT 2 sn ile.
  if (!wifiOk && (forcePortalBoot || wifiCount == 0)) {
    bootStop();
    startConfigPortal();
    if (WiFi.status() != WL_CONNECTED) { buildWifiMulti(); wifiMulti.run(); }
    wifiOk = (WiFi.status() == WL_CONNECTED);
    showBootWarning();
  }
  if (wifiOk) {
    bootStage(L("Saat ayarlanıyor...", "Setting the clock..."), 62);
    startOtaServer();
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  if (wifiOk) { unsigned long t0 = millis(); while (time(nullptr) < 1600000000 && millis() - t0 < 4000) delay(100); }

  bool dataOk = false;
  if (wifiOk) {
    bootStage(L("Veri alınıyor...", "Fetching data..."), 92);
    if (quickBoot) delay(10);
    dataOk = fetchAllPersons();   // 4.0
  }
  bootFinish();
  currentScreen = homeScreenIdx();

  if (!wifiOk) {
    setFetchError(1, "Acilista");
    showErrorScreen = true;
    drawErrorScreenFull();
    delay(2000);
  } else if (!dataOk) {
    if (lastErrorCode == 0) setFetchError(3, sourceLabel());
    showErrorScreen = true;
    drawErrorScreenFull();
    delay(2500);
  }

  pumpAgeWarnQueue(); // acilista bile 12sa/3sa/1sa/15dk esigi gecilmisse hemen bildir

  if (!showErrorScreen) drawCurrentScreen();
  else drawErrorScreenFull();
  wdtStart();                             // bundan sonra 2,5 dk tepkisiz kalirsa kendini resetler
  bgFetchStart();                         // 3.4.0: veri cekme arka planda
}

void loopBody() {
  { unsigned long nw = millis();
    if ((long)(nw - visualBurstEnd) >= 0) visualBurstEnd = nw;
    if ((long)(nw - alarmBurstEnd) >= 0) alarmBurstEnd = nw;
    if ((long)(nw - sleepWakeUntil) >= 0) sleepWakeUntil = nw;
    if ((long)(nw - loginLockUntil) >= 0) loginLockUntil = nw; }
  if (otaServerStarted) { server.handleClient(); }

  static unsigned long bootDownAt = 0;
  static bool bootLong = false;
  if (digitalRead(BOOT_PIN) == LOW) {
    if (bootDownAt == 0) { bootDownAt = millis(); bootLong = false; Serial.println("BOOT basildi"); }
    unsigned long held = millis() - bootDownAt;
    static unsigned long lastHoldDraw = 0;
    if (!bootLong && held > 400 && millis() - lastHoldDraw > 80) {
      lastHoldDraw = millis();
      if (screenAsleep) sleepWake(true);
      drawBootHoldPill(held);
    }
    if (!bootLong && held > 2000) {
      bootLong = true;
      Serial.println("BOOT 2 sn: WiFi kurulumu");
      doWifiSetup();
    }
  } else {
    // 2.5.0: kisa basis = geri (uyuyorsa ekrani ac)
    if (bootDownAt && !bootLong && millis() - bootDownAt > 40 && millis() - bootDownAt < 1000) {
      if (screenAsleep) sleepWake(true);
      else { goBack(); if (millis() - bootDownAt > 400) redrawNow(); }
    } else if (bootDownAt && !bootLong && millis() - bootDownAt >= 1000) {
      redrawNow();                      // basili tutma yarida birakildi: cubugu temizle
    }
    bootDownAt = 0;
  }

  // Sensor/Set sure-dolum uyarisi (12sa/3sa/1sa/15dk kala) acikken: birkac
  // saniye (AGE_WARN_SHOW_MS) bekle, sure dolunca kaldigi ekrana geri don ve
  // sirada baska bir esik varsa hemen onu goster. Bu sure boyunca normal
  // dokunma/yenileme islenmiyor - "device lock" ekranindaki desenle ayni.
  if (ageWarnActive) {
    if ((long)(millis() - ageWarnShowUntil) >= 0) {
      ageWarnActive = false;
      drawCurrentScreen();
      pumpAgeWarnQueue();
    }
    serviceAlarmSoundFam();               // 3.4.3: uyari ekrani acikken de alarm sesi (4.0.1: aile modu)
    touchReset = true;
    wdtFeed();
    delay(20);
    return;
  }

  if (millis() - lastBatRead > 5000UL) {      // 3.4.12: 30 sn -> 5 sn (sarja takinca simge hemen degissin)
    readBattery();
    checkLowBattery();
    // 3.4.24: pille calisirken WiFi modem uykusu (aralarda radyo uyur, baglanti kopmaz); USB'de hep acik
    if (WiFi.getMode() == WIFI_STA && ((WiFi.getSleep() != WIFI_PS_NONE) != onBatteryPower())) WiFi.setSleep(onBatteryPower());
    if ((batteryPercent != lastDrawnBatPct || batteryCharging != lastDrawnBatChg) && !screenAsleep) {
      lastDrawnBatPct = batteryPercent; lastDrawnBatChg = batteryCharging;
      if ((isHomeScreen() && uiPageMode != 4 && teacherStep == 0) || currentScreen == SCR_APPS) { if (autoRefreshOk()) drawCurrentScreen(); }
      else if (batteryIndicatorVisible()) drawBatteryIndicator();
    }
  }

  static bool touchWas = false;
  static unsigned long lastTouchNav = 0;
  int sc = screenCount();
  int rawX = 0, rawY = 0;
  bool touchNow = touchChipOk && touchReadRaw(rawX, rawY);
  if (holdSwallow) { if (touchNow) touchNow = false; else holdSwallow = false; }   // 3.4.2

  bool sleepSwallow = false;
  if (screenAsleep && touchNow) {          // uyurken dokunus: sadece ekrani ac
    sleepWake(true);
    sleepSwallow = true;
    lastTouchNav = millis();
  }
  bool listHandled = !sleepSwallow && handleListTouch(touchNow, touchWas, rawX, rawY, lastTouchNav);
  if (listHandled && !touchNow) lastTouchNav = millis();

  // 2.5.0: karar parmak kalkinca verilir - kisa dokunus = tiklama, yatay kaydirma = gezinme
  static bool gDown = false;
  if (touchReset) { gDown = false; touchReset = false; }
  static int gsx = 0, gsy = 0, glx = 0, gly = 0;
  static bool teacherPlayVideo = false;
  bool tapNow = false;
  int swipeDir = 0;
  // 3.4.2: herhangi bir temada 5 sn basili tutunca Temalar sayfasi
  {
    static unsigned long holdT0 = 0, holdDraw = 0;
    bool homeHold = touchNow && !sleepSwallow && !listHandled && gDown && isHomeScreen() && teacherStep == 0 &&
                    !showErrorScreen && abs(glx - gsx) < 25 && abs(gly - gsy) < 25;
    if (homeHold) {
      if (holdT0 == 0) holdT0 = millis();
      unsigned long held = millis() - holdT0;
      if (held > 800 && millis() - holdDraw > 100) { holdDraw = millis(); cv = &tft; drawThemeHoldPill(held); themeHoldShown = true; }
      if (held >= 3000) {
        themeHoldShown = false;
        holdT0 = 0; gDown = false; holdSwallow = true; lastTouchNav = millis();
        openApp(0);
      }
    } else if (!touchNow) {
      if (holdT0 && millis() - holdT0 > 800) { gDown = false; if (isHomeScreen()) drawCurrentScreen(); }   // yarida birakildi
      themeHoldShown = false;
      holdT0 = 0;
    }
  }
  if (!sleepSwallow && !listHandled) {
    if (touchNow) {
      int tx, ty;
      mapTouchToScreen(rawX, rawY, tx, ty);
      if (!touchWas) {
        if (millis() - lastTouchNav > 250) { gDown = true; gsx = glx = tx; gsy = gly = ty; lastUserTouchMs = millis(); }
      } else if (gDown) { glx = tx; gly = ty; }
    } else if (gDown) {
      gDown = false;
      int dx = glx - gsx, dy = gly - gsy;
      if (abs(dx) >= 60 && abs(dx) > abs(dy) * 3 / 2) swipeDir = (dx > 0) ? 1 : -1;
      else if (abs(dx) < 30 && abs(dy) < 30) tapNow = true;
    }
  } else gDown = false;
  if (swipeDir != 0 && !showErrorScreen) { handleSwipe(swipeDir); lastTouchNav = millis(); }

  if (tapNow) {
    int x = gsx, y = gsy;
    lastUserTouchMs = millis();

    if (famPillHit(x, y) && !(formId && formDlg) && !timePickerActive) { famManualNext(); lastTouchNav = millis(); }   // 4.0
    else if (famSplitTap(x, y)) { lastTouchNav = millis(); }                                                        // 4.0
    else if (showErrorScreen) {
      // Buton alani: y>=140, Yenile | Kurulum | Geri
      if (y >= 140 && x >= 108 && x < 212) {
        showErrorScreen = false; errorDismissed = false;
        doWifiSetup();
      } else if (y >= 140) {
        if (x < 108) {
          errorDismissed = false;
          cv = &tft;
          cv->fillRoundRect(8, 150, 96, 56, 10, 0x4208);
          cv->setTextColor(TFT_WHITE, 0x4208);
          cv->setTextDatum(MC_DATUM);
          drawFreeTitle(L("Bekleyin...", "Wait..."), 56, 178, &FreeSansTR7pt);
          cv->setTextDatum(TL_DATUM);
          if (WiFi.status() != WL_CONNECTED) {
            buildWifiMulti();
            wifiMulti.run();
          }
          if (WiFi.status() == WL_CONNECTED) {
            if (!otaServerStarted) startOtaServer();
            errRetryPending = true;          // sonuc gelince ana dongu karar verir
            requestFetch();
          } else {
            setFetchError(1, savedWifiName());
            showErrorScreen = true;
            drawErrorScreenFull();
          }
        } else {
          showErrorScreen = false;
          errorDismissed = true;
          currentScreen = homeScreenIdx();
          subScreen = 0;
          drawCurrentScreen();
        }
      }
      lastTouchNav = millis();
    }
    else if (formId && formDlg) { formDialogTap(x, y); lastTouchNav = millis(); }
    else if (timePickerActive) { timePickerTap(x, y); drawCurrentScreen(); lastTouchNav = millis(); }
    else if (currentScreen == SCR_APPS) { appsTap(x, y); lastTouchNav = millis(); }
    else if (currentScreen == SCR_THEME) { themeTap(x, y); lastTouchNav = millis(); }
    else if (currentScreen == SCR_KARB && karbView == 1) { karbCalcTap(x, y); lastTouchNav = millis(); }
    else if (currentScreen == SCR_VIDEO) { videoTap(x, y); lastTouchNav = millis(); }
    else if (currentScreen == SCR_SENSOR) {
      if (y < LIST_Y) { if (x < 100) goBack(); else if (x > 236) formOpen(F_SENSOR); }
      lastTouchNav = millis();
    }
    else if ((currentScreen == SCR_STATS || currentScreen == SCR_TIMER) && x < 70 && y < 40) { goBack(); lastTouchNav = millis(); }
    else if (currentScreen == SCR_GRAPH && !graphDetailOpen && !graphMenuOpen && x < 60 && y < 56) { goBack(); lastTouchNav = millis(); }
    else if (isHomeScreen() && teacherStep == 0 && homeWifiPillOn() && homeWifiHit(x, y)) {
      openWifiErrorScreen(); lastTouchNav = millis();
    }
    else if (currentScreen == 1 && teacherStep == 0 && dataValid && shouldShowSnoozeButton() && homeSnoozeHit(x, y)) {
      doSnooze(); lastTouchNav = millis();
    }
    else if (currentScreen == 1 && teacherStep == 0 && dataValid && shouldShowDikkat() && homeDikkatHit(x, y)) {
      startTeacherFlow(); lastTouchNav = millis();
    }
    else if (uiPreviewActive) {
      // Onizleme ekrani: dokun -> Arayuz menusune don (ilk sayfaya degil)
      uiPreviewActive = false;
      if (uiPreviewMode == 3) {
        uiPreviewMode = 0;
        applyRotation();
      }
      subScreen = 8;
      currentScreen = settingsScreenIndex();
      drawSettingsScreen();
      lastTouchNav = millis();
    }
    else if (showGuide) {
      guidePage++;
      if (guidePage >= GUIDE_PAGES) { showGuide = false; guidePage = 0; }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if ((subScreen == 1 || subScreen == 18) && !timePickerActive) {
      if (alarmEditStep == 0 && subScreen == 1) {
        // Ust satir (3): Gorsel | Ertele | Modlar. Alt satir (2): Ses Bastirma | Geri
        if (y >= 48 && y < 98) {
          int col = (x - 12) / 102;
          if (col == 0) alarmEditStep = 3;       // Gorsel
          else if (col == 1) alarmEditStep = 6;  // Ertele
          else if (col == 2) alarmEditStep = 20; // Modlar
        } else if (y >= 110 && y < 168) {
          int col2 = (x - 12) / 102;
          if (col2 <= 0) alarmEditStep = 21;      // Ses Bastirma (aciklama + ac/kapa)
          else if (col2 == 1) alarmEditStep = 22; // Cift Ok alarmlari
          else { saveSettings(); subScreen = 0; alarmEditStep = 0; } // Geri
        }
      } else if (alarmEditStep == 22) {
        if (y >= 100 && y < 140) alarmFastFallOn = !alarmFastFallOn;
        else if (y >= 146 && y < 186) alarmFastRiseOn = !alarmFastRiseOn;
        else if (y >= 192 && y < 234 && x >= 100 && x < 220) { saveSettings(); alarmEditStep = 0; }
      } else if (alarmEditStep == 21) {
        if (y >= 190 && y < 224) {
          if (x < 200) recoverySoundSuppressOn = !recoverySoundSuppressOn;
          else alarmEditStep = 0; // Geri
        }
      } else if (alarmEditStep == 20) {
        // Okul | Gece / Ogretmen | Geri
        if (y >= 60 && y < 180) {
          int col = (x < 160) ? 0 : 1;
          int row = (y - 60) / 60;
          int i = row * 2 + col;
          if (i == 0) { subScreen = 13; alarmEditStep = 0; } // Okul
          else if (i == 1) { subScreen = 10; alarmEditStep = 0; } // Gece
          else if (i == 2) { subScreen = 16; alarmEditStep = 0; } // Ogretmen
          else if (i == 3) alarmEditStep = 0; // Geri
        }
      } else if (alarmEditStep == 10) {
        if (y >= 48 && y < 200) {
          int row = (y - 48) / 52;
          int col = (x < 160) ? 0 : 1;
          if (row >= 0 && row < 3) {
            int i = col * 3 + row;
            if (i == 0) alarmEditStep = 4;
            else if (i == 1) alarmEditStep = 1;
            else if (i == 2) alarmEditStep = 2;
            else if (i == 3) alarmEditStep = 7;
            else if (i == 4) alarmEditStep = 5;
            else if (i == 5) {
              if (subScreen == 18) { saveSettings(); subScreen = 0; alarmEditStep = 0; }
              else alarmEditStep = 0;
            }
          }
        }
      } else if (alarmEditStep == 1) {
        if (x >= 270 && y < 32) {
          alarmLowSound = !alarmLowSound;
        } else if (y >= 38 && y < 118 && x >= 60 && x <= 260) {
          int mn = (alarmCritLow + 5 > 40) ? alarmCritLow + 5 : 40;
          openNumberPicker(L("DÜŞÜK EŞİK", "LOW LIMIT"), &alarmLow, mn, alarmHigh - 10, "mg/dL");
        } else if (y >= 128 && y <= 170) {
          // Alt sinir: hem mutlak taban (40) hem de Kritik Dusuk esiginin
          // en az 5 birim uzerinde (web formundaki ayni kural burada da gecerli).
          if (x < 160) {
            alarmLow -= 5;
            int floorV = (alarmCritLow + 5 > 40) ? alarmCritLow + 5 : 40;
            if (alarmLow < floorV) alarmLow = floorV;
          }
          else { alarmLow += 5; if (alarmLow > alarmHigh - 10) alarmLow = alarmHigh - 10; }
        } else if (y >= 188) alarmEditStep = 10;
      } else if (alarmEditStep == 2) {
        if (x >= 270 && y < 32) {
          alarmHighSound = !alarmHighSound;
        } else if (y >= 38 && y < 118 && x >= 60 && x <= 260) {
          openNumberPicker(L("YÜKSEK EŞİK", "HIGH LIMIT"), &alarmHigh, alarmLow + 10, alarmVeryHigh - 5, "mg/dL");
        } else if (y >= 128 && y <= 170) {
          if (x < 160) { alarmHigh -= 5; if (alarmHigh < alarmLow + 10) alarmHigh = alarmLow + 10; }
          else { alarmHigh += 5; if (alarmHigh >= alarmVeryHigh) alarmHigh = alarmVeryHigh - 5; }
        } else if (y >= 188) alarmEditStep = 10;
      } else if (alarmEditStep == 7) {
        if (x >= 270 && y < 32) {
          alarmVeryHighSound = !alarmVeryHighSound;
        } else if (y >= 38 && y < 118 && x >= 60 && x <= 260) {
          openNumberPicker(L("ÇOK YÜKSEK EŞİK", "VERY HIGH LIMIT"), &alarmVeryHigh, alarmHigh + 10, 400, "mg/dL");
        } else if (y >= 128 && y <= 170) {
          if (x < 160) { alarmVeryHigh -= 5; if (alarmVeryHigh < alarmHigh + 10) alarmVeryHigh = alarmHigh + 10; }
          else { alarmVeryHigh += 5; if (alarmVeryHigh > 400) alarmVeryHigh = 400; }
        } else if (y >= 188) alarmEditStep = 10;
      } else if (alarmEditStep == 4) {
        if (x >= 270 && y < 32) {
          alarmCritLowSound = !alarmCritLowSound;
        } else if (y >= 38 && y < 118 && x >= 60 && x <= 260) {
          openNumberPicker(L("KRİTİK DÜŞÜK EŞİK", "CRITICAL LOW"), &alarmCritLow, 40, alarmLow - 5, "mg/dL");
        } else if (y >= 128 && y <= 170) {
          if (x < 160) { alarmCritLow -= 5; if (alarmCritLow < 40) alarmCritLow = 40; }
          else {
            alarmCritLow += 5;
            if (alarmCritLow > alarmLow - 5) alarmCritLow = alarmLow - 5;
            if (alarmCritLow < 40) alarmCritLow = 40; // Dusuk esik 40'a cok yakinken taban altina dusmesin
          }
        } else if (y >= 188) alarmEditStep = 10;
      } else if (alarmEditStep == 5) {
        if (x >= 270 && y < 32) {
          alarmStaleSound = !alarmStaleSound;
        } else if (y >= 38 && y < 118 && x >= 60 && x <= 260) {
          openNumberPicker(L("VERİ YOK ALARMI", "NO-DATA ALARM"), &staleAlarmMin, 5, 60, L("dk", "min"));
        } else if (y >= 128 && y <= 170) {
          if (x < 160) { staleAlarmMin -= 5; if (staleAlarmMin < 5) staleAlarmMin = 5; }
          else { staleAlarmMin += 5; if (staleAlarmMin > 60) staleAlarmMin = 60; }
        } else if (y >= 188) alarmEditStep = 10;
      } else if (alarmEditStep == 3) {
        // 2.2.0 duzeni: baslik geri oku | ac/kapa satiri | 6 renk
        if (y < 40 && x < 70) { saveSettings(); alarmEditStep = 0; }
        else if (y >= 46 && y < 92) visualAlarmOn = !visualAlarmOn;
        else if (y >= 118 && y < 226) {
          int col = (x - 12) / 100;
          int row = (y - 118) / 58;
          if (col >= 0 && col < 3 && row >= 0 && row < 2) {
            alarmColorIdx = row * 3 + col;
            visualAlarmOn = true;
          }
        }
      } else if (alarmEditStep == 6) {
        int col = (x < 160) ? 0 : 1;
        const int opts[] = {15, 30, 60, 0};
        if (y >= 55 && y < 103) snoozeMinutes = opts[col];
        else if (y >= 113 && y < 161) snoozeMinutes = opts[2 + col];
        else if (y >= 195) alarmEditStep = 0;
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (subScreen == 2) {
      // Ses secimi 2x2
      if (y >= 70 && y < 118) {
        if (x < 160) soundLevel = 0; else soundLevel = 1;
        prefs.begin("metematik", false); prefs.putInt("sound", soundLevel); prefs.end();
        playTestBeep();
      } else if (y >= 125 && y < 173) {
        if (x < 160) soundLevel = 2; else soundLevel = 3;
        prefs.begin("metematik", false); prefs.putInt("sound", soundLevel); prefs.end();
        playTestBeep();
      } else if (y >= 195) {
        subScreen = 0;
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (subScreen == 3) {
      // Reset ekrani: sifre yerine sec + Evet/Hayir onayi (yanlislikla dokunmaya karsi
      // koruma artik iki bilincli dokunustan geliyor, ezberlenecek PIN yok).
      if (resetAction == 0) {
        if (y >= 54 && y <= 144) {
          resetAction = (x < 160) ? 1 : 2;
        } else if (y >= 194 && y < 234) {
          subScreen = 0;
        }
      } else {
        bool poweroff = (resetAction == 2);
        if (y >= 150 && y <= 200) {
          if (x < 160) {
            // Evet
            resetAction = 0;
            if (poweroff) {
              doPowerOff();                       // 3.4.20: kendi kayit da kaydedilir, pil olcer uyur
            } else {
              doDeviceReset();
            }
          } else {
            // Hayir — secim ekranina don
            resetAction = 0;
          }
        }
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    // NS Detay ekrani — Geri
    else if (currentScreen == 2 && graphDetailOpen && x >= 200 && y <= 48) {
      graphDetailOpen = false;
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    // NS: ayri Detay butonu (delta ile saat arasi)
    else if (currentScreen == 2 && !graphDetailOpen && dataSource == "nightscout"
             && x >= 175 && x < 228 && y <= 50) {
      graphMenuOpen = false;
      graphDetailOpen = true;
      lastNsExtraFetch = 0;               // 3.4.0: IOB/COB arka plandaki cekmede alinir
      detailRefreshPending = true;
      requestFetch();
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    // Grafik saat kutusu
    else if (currentScreen == 2 && !graphDetailOpen && x >= 228 && x <= 318 && y <= 44) {
      graphMenuOpen = !graphMenuOpen;
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (currentScreen == 2 && graphMenuOpen && x >= 220 && x <= 320 &&
             y >= 44 && y < 48 + GRAPH_HOURS_N * 36 + 8) {
      const int rowH = 36;
      int i = (y - 48) / rowH;
      if (y < 48) i = 0;
      int ghMax = (dataSource == "libre") ? 3 : GRAPH_HOURS_N;
      if (i >= 0 && i < ghMax) {
        graphHoursIdx = i;
        graphMenuOpen = false;
        graphDetailOpen = false;
        prefs.begin("metematik", false);
        prefs.putInt("ghours", graphHoursIdx);
        prefs.end();
        drawCurrentScreen();
      }
      lastTouchNav = millis();
    }
    // Ogretmen soru agaci (+ glukagon rehberi) - 3.4.5
    else if (currentScreen == 0 && teacherStep > 0) {
      int col3 = (x < 108) ? 0 : (x < 208 ? 1 : 2);
      if (teacherGlucagonPage > 0) {
        if (teacherGlucagonPage == 4 && y >= 70 && y < 180 && x >= 36 && x < 284 && videoExists()) {
          teacherPlayVideo = true;
        } else if (y >= 200) {
          if (x < 160) { if (teacherGlucagonPage > 1) teacherGlucagonPage--; else teacherGlucagonPage = 0; }
          else { if (teacherGlucagonPage < 4) teacherGlucagonPage++; else teacherGlucagonPage = 0; }
        }
      } else if (y >= 200) {
        if (x < 160) {                    // Geri
          if (teacherStep == 3) teacherStep = teacherBack ? teacherBack : 1;
          else if (teacherStep == 4) teacherStep = 2;
          else if (teacherStep == 2) teacherStep = 1;
          else teacherStep = 0;
          teacherGlucagonPage = 0;
        } else { teacherStep = 0; teacherGlucagonPage = 0; }   // Anasayfa
      } else if (teacherStep == 1 && y >= 120 && y < 180) {
        if (x < 160) { teacherConscious = false; teacherCat = teacherSensorCat(); teacherBack = 1; teacherStep = 3; }
        else { teacherConscious = true; teacherStep = 2; }
      } else if (teacherStep == 2 && y >= 120 && y < 180) {
        if (col3 == 1) { teacherFingerOk = false; teacherStep = 4; }               // farkli: sonucu sor
        else { teacherFingerOk = true; teacherCat = teacherSensorCat(); teacherBack = 2; teacherStep = 3; }   // yakin / olcemiyorum
      } else if (teacherStep == 4 && y >= 120 && y < 180) {
        teacherCat = col3 + 1; teacherBack = 4; teacherStep = 3;
      } else if (teacherStep == 3 && !teacherConscious && teacherSensorCat() == 1 && y >= 150 && y < 195) {
        teacherGlucagonPage = 1;
      }
      if (teacherStep == 0 && teacherPreview) { teacherPlayVideo = false; openPreviewMenu(); }   // 3.4.6
      else {
        if (teacherStep == 0) currentScreen = homeScreenIdx();
        if (teacherPlayVideo) { teacherPlayVideo = false; openVideo(0, true); }
        else drawCurrentScreen();
      }
      lastTouchNav = millis();
    }
    else if (currentScreen == 0 && shouldShowDikkat() && x < 120 && y > 180) {
      // Soru agaci seker degerine gore
      teacherConscious = true;
      teacherGlucagonPage = 0;
      teacherFingerOk = true;
      // Onceden sadece "kritik dusuk" (alarmCritLow alti) icin bilinc
      // sorusuyla basliyordu; dusuk ama kritik olmayan (55-69 gibi) ve cok
      // yuksek durumlarda dogrudan parmak olcume atlaniyordu - bu, bilinc
      // kaybi ihtimalini o durumlarda hic sormadan atlamak anlamina
      // geliyordu (orn. cok yuksekte DKA'ya bagli bilinc bulanikligi
      // olabilir, oyle bir ogrenciye "su icir" onerisi tehlikeli olur).
      // Artik her iki yon icin de once HER ZAMAN bilinc sorusu soruluyor.
      if (sgv > 0 && (sgv < alarmLow || sgv >= alarmVeryHigh)) teacherStep = 1;
      else teacherStep = 0;
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    // Snooze alani — ses de kesilir (artik sag alttaki dikdortgen kart, Dikkat'in aynasi)
    else if (currentScreen == 0 && shouldShowSnoozeButton() && x > 200 && y > 180) {
      startSnooze();
      stopSpeaker();
      beepPhase = 0;
      cv->fillScreen(UI_BG);
      cv->setTextColor(sevColor(2), UI_BG);
      cv->setTextDatum(MC_DATUM);
      char sb[24];
      snprintf(sb, sizeof(sb), L("Alarm %d dk", "Alarm %d min"), snoozeShownMins());
      dsN(cv, sb, 160, 100, 4);
      drawFreeTitle(L("ertelendi", "snoozed"), 160, 140, &FreeSansTR9pt);
      delay(1200);
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isTimerScreen()) {
      bool handled = false;
      if (timerRunning) {
        // IPTAL (asagida)
        if (y >= 160 && y <= 220 && x >= 60 && x <= 260) {
          timerRunning = false;
          timerEndMs = 0;
          drawCurrentScreen();
          handled = true;
        }
      } else if (dataValid && sgv >= 70) {
        // BASLAT (asagida, IPTAL ile ayni dikdortgen alan)
        if (y >= 160 && y <= 220 && x >= 60 && x <= 260) {
          int mins = sgv / 10;
          if (mins < 1) mins = 1;
          timerTotalSec = mins * 60;
          timerEndMs = millis() + (unsigned long)timerTotalSec * 1000UL;
          timerRunning = true;
          drawCurrentScreen();
          handled = true;
        }
      }
      lastTouchNav = millis();
      (void)handled;
    }
    else if (isSettingsScreen() && subScreen == 0 && y >= 36 && y <= 100) {
      // UST: Parlaklik | Ses | Alarm
      if (x < 108) {
        subScreen = 6; drawCurrentScreen();
      } else if (x < 210) {
        subScreen = 2; drawCurrentScreen();
      } else {
        subScreen = 1; alarmEditStep = 0; drawCurrentScreen();
      }
      lastTouchNav = millis();
    }
    else if (currentScreen == 1 && !isSettingsScreen() && !showGuide) {
      // bos alana dokunus: hata varsa hata ekrani, yoksa kisa ipucu (uygulamalar icin sola kaydir)
      homeEmptyTap();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 0 && y >= 110 && y <= 168) {
      // ALT: Hedefler | QR/Web | Diger
      if (x < 108) { subScreen = 18; alarmEditStep = 10; drawCurrentScreen(); }
      else if (x < 210) { subScreen = 7; drawCurrentScreen(); }
      else { subScreen = 0; drawCurrentScreen(); }
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 21) {
      if (y < 40 && x < 90) { subScreen = 0; openSleepFormFromTest(); }
      else { snapSingles = 0; drawCurrentScreen(); }   // dokun: sayaci sifirla
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && timePickerActive) {
      timePickerTap(x, y);
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 10) {
      if (y >= 32 && y <= 68) {
        nightModeOn = (x < 160);
      } else if (y >= 72 && y <= 112) {
        if (x >= 194 && x <= 240) stepHalfHour(nightStartHour, nightStartMin, -1);
        else if (x >= 248 && x <= 294) stepHalfHour(nightStartHour, nightStartMin, 1);
        else if (x < 190) openTimePicker(L("GECE BAŞLANGIÇ", "NIGHT START"), &nightStartHour, &nightStartMin);
      } else if (y >= 118 && y <= 158) {
        if (x >= 194 && x <= 240) stepHalfHour(nightEndHour, nightEndMin, -1);
        else if (x >= 248 && x <= 294) stepHalfHour(nightEndHour, nightEndMin, 1);
        else if (x < 190) openTimePicker(L("GECE BİTİŞ", "NIGHT END"), &nightEndHour, &nightEndMin);
      } else if (y >= 166 && y <= 216) {
        if (x < 106) nightBrightLevel = (nightBrightLevel + 1) % 3;
        else if (x < 212) nightSoundLevel = (nightSoundLevel + 1) % 4;
        else {
          saveSettings();
          applyBrightness();
          subScreen = 19; alarmEditStep = 0;   // Modlar listesine don
        }
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 13) {
      if (y >= 32 && y <= 68) {
        schoolModeOn = (x < 160);
      } else if (y >= 72 && y <= 112) {
        if (x >= 194 && x <= 240) stepHalfHour(schoolStartHour, schoolStartMin, -1);
        else if (x >= 248 && x <= 294) stepHalfHour(schoolStartHour, schoolStartMin, 1);
        else if (x < 190) openTimePicker(L("OKUL BAŞLANGIÇ", "SCHOOL START"), &schoolStartHour, &schoolStartMin);
      } else if (y >= 118 && y <= 158) {
        if (x >= 194 && x <= 240) stepHalfHour(schoolEndHour, schoolEndMin, -1);
        else if (x >= 248 && x <= 294) stepHalfHour(schoolEndHour, schoolEndMin, 1);
        else if (x < 190) openTimePicker(L("OKUL BİTİŞ", "SCHOOL END"), &schoolEndHour, &schoolEndMin);
      } else if (y >= 166 && y <= 216) {
        if (x < 106) schoolBrightLevel = (schoolBrightLevel + 1) % 3;
        else if (x < 212) schoolSoundLevel = (schoolSoundLevel + 1) % 4;
        else {
          saveSettings();
          applyBrightness();
          subScreen = 19; alarmEditStep = 0;   // Modlar listesine don
        }
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 14) {
      // Dil Reset / Rotasyon Yenile / Geri (Bilgi ve Kilavuz artik QR sayfasinda)
      if (y >= 56 && y < 108) {
        if (x < 160) subScreen = 5; // Dil
        else { resetAction = 0; subScreen = 3; } // Reset
      } else if (y >= 120 && y < 172) {
        if (x < 160) subScreen = 9; // Rotasyon
        else requestFetch(); // Yenile
      } else if (y >= 194 && y < 234) {
        subScreen = 0;
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 16) {
      // Ogretmen modu ac/kapa + geri
      if (y >= 28 && y < 66) {
        if (x < 160) teacherModeOn = true;
        else teacherModeOn = false;
        prefs.begin("metematik", false); prefs.putBool("teachon", teacherModeOn); prefs.end();
      } else if (y >= 210) {
        subScreen = 19; alarmEditStep = 0;  // Modlar listesine don
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 4) {
      if (y >= 198 && y < 226 && x < 160) subScreen = 15; // Kaynaklar
      else subScreen = infoReturnScreen; // Geri: nereden gelindiyse oraya don
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 15) {
      subScreen = 4;
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 5) {
      // Dil / Birim tek 2x2 izgarada, Renk Dostu Modu altta, Geri en altta
      // (web arayuzu henuz Turkce sabit)
      if (y >= 34 && y < 82) {
        openLangPicker();
      } else if (y >= 88 && y < 136) {
        if (x < 160) unitIdx = 0; else unitIdx = 1;
        prefs.begin("metematik", false); prefs.putInt("unit", unitIdx); prefs.end();
      } else if (y >= 158 && y < 184) {
        colorBlindMode = (x >= 160);
        prefs.begin("metematik", false); prefs.putBool("cblind", colorBlindMode); prefs.end();
      } else if (y >= 194 && y < 234) {
        subScreen = 0;
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 6) {
      // Parlaklik: Alarm menusuyle ayni 2x2 ortali izgara (Dusuk/Orta/Yuksek/Geri)
      if (y >= 60 && y < 180) {
        int col = (x < 160) ? 0 : 1;
        int row = (y - 60) / 60;
        int i = row * 2 + col;
        if (i == 0 || i == 1 || i == 2) {
          brightnessLevel = i;
          applyBrightness();
          prefs.begin("metematik", false); prefs.putInt("bright", brightnessLevel); prefs.end();
        } else if (i == 3) {
          subScreen = 0;
        }
      }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 31 && famEnabled && x >= 170 && y >= 150 && y < 204) {   // 4.0.2: Sirayla / Bolunmus
      int m = (y < 177) ? 0 : 1;
      if (m != famMode) { famMode = m; famLastRot = millis(); saveSettings(); }
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 31) {    // 4.0.1: aile modu QR - diger dokunuslar Modlar'a doner
      subScreen = 0;
      currentScreen = (famQrFrom >= 0) ? famQrFrom : SCR_SETTINGS;
      famQrFrom = -1;
      formId = F_MODES; formDepth = 0; formDlg = 0;
      formSnapshot();
      drawCurrentScreen();
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 7) {
      // Sag sutun butonlari: Bilgi / Kilavuz / Geri (drawQrScreen ile ayni
      // koordinatlar: bx=176, by1=24..80, by2=90..146, by3=156..212).
      // Sol taraftaki QR/uyari alanina dokunmak da (eskisi gibi) geri gonderir.
      if (y < LIST_Y && x < 100) { subScreen = 0; drawCurrentScreen(); }
      else if (x >= 170 && y >= 142 && y < 188) openTextPage(TP_INFO);
      else if (x >= 170 && y >= 188 && y < 236) openTextPage(TP_GUIDE);
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 8) {
      // Onizleme sec / Uygula / Geri (Dikey kaldirildi; Alarm menusuyle ayni izgara)
      if (y >= 60 && y < 180) {
        int col = (x < 160) ? 0 : 1;
        int row = (y - 60) / 60;
        int sel = row * 2 + col;
        const int dispModes[] = {0, 1, 2, 4};
        if (row >= 0 && row < 2 && sel >= 0 && sel < 4) {
          uiPreviewMode = dispModes[sel];
          uiPreviewActive = true;
          applyRotation();
          if (uiPreviewMode == 0) drawUiSade();
          else if (uiPreviewMode == 1) drawUiBigDigits();
          else if (uiPreviewMode == 2) drawUiCalendar();
          else drawUiFotoPlaceholder();
        }
      } else if (y >= 185) {
        if (x < 160) {
          // Uygula -> 2. sayfa
          uiPageMode = uiPreviewMode;
          uiPreviewActive = false;
          prefs.begin("metematik", false);
          prefs.putInt("uipage", uiPageMode);
          prefs.end();
          applyRotation();
          subScreen = 0;
          drawSettingsScreen();
        } else {
          uiPreviewActive = false;
          applyRotation();
          subScreen = 0;
          drawSettingsScreen();
        }
      }
      lastTouchNav = millis();
    }
    else if (isSettingsScreen() && subScreen == 9) {
      if (y >= 70 && y <= 145) {
        if (x < 160) landRot = 1; else landRot = 3;
        prefs.begin("metematik", false);
        prefs.putInt("landrot", landRot);
  prefs.putInt("uipage", uiPageMode);
        prefs.end();
        applyRotation();
        cv->fillScreen(UI_BG);
        drawRotationScreen();
      } else if (y >= 172) {
        subScreen = 0;
        drawSettingsScreen();
      }
      lastTouchNav = millis();
    }
    else {
      // 2.5.0: bos alana dokunmak sayfa degistirmez; ana ekranda kisa ipucu gosterir
      if (isHomeScreen() && teacherStep == 0) homeEmptyTap();
      lastTouchNav = millis();
    }
  }
  touchWas = touchNow;
  (void)sc;

  // ipucu suresi dolunca ana ekrani temizle
  if (homeHintUntil && (long)(millis() - homeHintUntil) >= 0) {
    homeHintUntil = 0;
    if (isHomeScreen() && autoRefreshOk()) drawCurrentScreen();
  }
  serviceVideo();
  static unsigned long lastAnim = 0;
  if (isHomeScreen() && themeAnimated() && teacherStep == 0 && autoRefreshOk() && !showErrorScreen &&
      bootDownAt == 0 && !themeHoldShown && (long)(millis() - alarmBurstEnd) >= 0 && millis() - lastAnim > themeAnimPeriod()) {
    lastAnim = millis();
    drawCurrentScreen();
  }

  serviceSleep();
  serviceMicTestScreen();

  updateAge();
  if (!showErrorScreen && teacherStep == 0 && millis() - lastBarRedraw > 15000) {
    lastBarRedraw = millis();
    // 2.5.0: ana ekran (sprite) ve uygulamalar sayfasi tek seferde, titremeden yenilenir
    if (autoRefreshOk() && ((isHomeScreen() && uiPageMode != 4) || currentScreen == SCR_APPS)) drawCurrentScreen();
  }
  // Arayuz kutusu: 10 sn dokunulmazsa gizle (her dongude kontrol)
  if (currentScreen == 1 && uiChromeVisible && !uiMenuOpen
      && lastUiChromeMs > 0 && millis() - lastUiChromeMs > 5000UL) {
    uiChromeVisible = false;
    if (uiPageMode == 4) {
      if (fotoLoaded) restoreFotoTopBand();
      else drawUiFotoPlaceholder();
    } else {
      drawScreen1();
    }
  }
  // 3.4.0: veri cekme arka planda (bkz. requestFetch). Dokunurken ertelemeye gerek kalmadi;
  // sadece video oynarken bekletilir.
  bool deferBgFetch = (currentScreen == SCR_VIDEO && videoPlaying && !videoPaused && !videoEnded);   // video takilmasin
  unsigned long fetchIv = famFetchInterval();   // 4.0
  famService();                                 // 4.0: aile modu sirasi / alarm sabitleme
  if (!fetchBusy && !fetchDoneFlag) {
    if (deferBgFetch && millis() - lastFetch > fetchIv) {
      lastFetch = millis() - (fetchIv - 2000);
    } else if (millis() - lastFetch > fetchIv) {
      if (!dataValid && isHomeScreen() && !showErrorScreen && autoRefreshOk()) drawRetryPill();
      lastFetch = millis();
      requestFetch();
    }
  }
  if (fetchDoneFlag) {
    fetchDoneFlag = false;
    bool ok = fetchResult;
    bool discarded = fetchDiscarded;
    if (discarded) {
      // ayarlar cekme surerken degisti: bu sonuc yok sayilir, yenisi asagida istenir
    } else if (srcChangedPending && !ok && autoRefreshOk() && !showGuide) {
      srcChangedPending = false; errRetryPending = false;
      showErrorScreen = true; errorDismissed = false;
      drawErrorScreenFull();
    } else if (errRetryPending) {
      // Hata ekranindaki "Yenile"
      errRetryPending = false;
      if (ok) {
        showErrorScreen = false; errorDismissed = false;
        currentScreen = homeScreenIdx(); subScreen = 0;
        drawCurrentScreen();
      } else {
        showErrorScreen = true;
        drawErrorScreenFull();
      }
    } else {
      if (showErrorScreen) {
        if (ok) {
          showErrorScreen = false;
          if (!showGuide && subScreen == 0) drawCurrentScreen();
        } else if (errShownKey != lastErrorCode * 2 + (netNoInternet ? 1 : 0)) {
          drawErrorScreenFull();             // 3.4.28: yazi guncel durumu gostersin (or. WiFi geldi, internet yok)
        }
      } else if (ok && autoRefreshOk() && subScreen == 0) {
        // Kullanici az once dokunduysa tam yenileme sayfa gecisini bozar — atla
        if (millis() - lastUserTouchMs < 2500UL) {
          // sadece bekleyen dokunma islensin
        } else if (currentScreen == 1 && uiPageMode == 4 && dataValid) {
          // Foto: sadece rozet
          uint16_t c = getGlucoseColor(sgv);
          cv->fillRoundRect(6, 6, 92, 44, 8, 0x4208);
          cv->fillRoundRect(8, 8, 88, 40, 8, c);
          char b[12];
          { String _gs = formatGlucose(sgv); strncpy(b, _gs.c_str(), sizeof(b)-1); b[sizeof(b)-1]=0; }
          cv->setTextColor(TFT_BLACK, c);
          cv->setTextDatum(MC_DATUM);
          dsN(cv, b, 52, 28, 4);
        } else if (!(currentScreen == 2 && graphDetailOpen)) {
          // Detay acikken arka planda full refresh yapma
          drawCurrentScreen();
        }
      } else if (!ok && !dataValid && !showGuide && subScreen == 0 && autoRefreshOk() && !errorDismissed) {
        // Geri ile kapatildiysa zorla acma; Yenile veya yeni basarili veri sonrasi tekrar
        static unsigned long lastFullErr = 0;
        if ((lastErrorCode == 2 || lastErrorCode == 5) && millis() - lastFullErr > 5000UL) {
          lastFullErr = millis();
          showErrorScreen = true;
          drawErrorScreenFull();
        } else if (lastErrorCode != 2 && lastErrorCode != 5 && consecutiveFetchFails >= 2
                   && millis() - lastFullErr > 120000UL) {
          // 3/4/6 (sunucu yanit vermiyor / veri yok / okunamadi) genelde gecici bir ag
          // aksakligi olabiliyor. Tek seferlik basarisizlikta hemen tam ekran hata
          // gostermek yerine bir sonraki denemeyi de bekleyip iki ard arda hata
          // olursa uyariyoruz — kisa surup kendi kendine duzelen durumlar artik
          // ekrani kaplamiyor.
          lastFullErr = millis();
          showErrorScreen = true;
          drawErrorScreenFull();
        }
      }
    }
    if (!discarded) srcChangedPending = false;
    if (retryPillShown) {
      retryPillShown = false;
      if (!showErrorScreen && isHomeScreen() && autoRefreshOk() && subScreen == 0) drawCurrentScreen();
    }
    if (detailRefreshPending) {
      detailRefreshPending = false;
      if (currentScreen == 2 && graphDetailOpen && !showErrorScreen && autoRefreshOk()) drawCurrentScreen();
    }
    // Bu fetch sirasinda (fetchSageCage -> checkAgeWarnings) yeni bir esik
    // tetiklendiyse ve su an baska bir uyari gosterilmiyorsa hemen goster.
    pumpAgeWarnQueue();
    if (fetchAgain || discarded) { fetchAgain = false; lastFetch = millis(); requestFetch(); }
  }
  // Hata ekrani veya veri yokken yanip sonme yenilemesi YAPMA (titreme)
  // Gorsel alarm: ses gibi kisa burst; yeni olcumde requestAlarmBurst ile tekrar
  if (!showErrorScreen && dataValid && visualAlarmOn && isAlarmCondition()
      && (long)(millis() - visualBurstEnd) < 0 && millis() - lastBlink > 400) {
    alarmBlinkState = !alarmBlinkState;
    lastBlink = millis();
    if (isHomeScreen() && autoRefreshOk() && subScreen == 0) {
      if (currentScreen == 1 && uiPageMode == 4) drawFotoGlucoseBadge(); else drawCurrentScreen();
    }
  } else if ((long)(millis() - visualBurstEnd) >= 0 && alarmBlinkState) {
    alarmBlinkState = false;
    if (isHomeScreen() && autoRefreshOk() && subScreen == 0 && !showErrorScreen)
      drawCurrentScreen();
  }

  // Foto Cerceve: birden fazla foto varsa, webden secilen dakikada
  // bir sonraki fotoya gec. Sadece o ekran acikken calisir (arka planda
  // dolasmiyor) - kullanici Foto Cerceve'ye donduginde kaldigi fotoyu gorur.
  if (fotoCount > 1 && currentScreen == 1 && uiPageMode == 4 && !showGuide
      && subScreen == 0 && millis() - fotoLastRotate > (unsigned long)fotoRotateMin * 60000UL) {
    fotoLastRotate = millis();
    fotoActiveIdx = (fotoActiveIdx + 1) % fotoCount;
    loadFotoFromFS();
    drawCurrentScreen();
  }

  serviceAlarmSoundFam();   // 4.0.1: alarmdaki kisinin sesi (ekranda baska kisi olsa da)
  serviceTimer();

  // Gece modu: saat dilimi degisince parlaklik uygula
  static bool wasNight = false;
  static unsigned long lastNightCheck = 0;
  if (millis() - lastNightCheck > 30000) {
    lastNightCheck = millis();
    bool nowN = isNightHours();
    if (nowN != wasNight) {
      wasNight = nowN;
      applyBrightness();
    }
  }

  // Sadece sayac alani (tam ekran yenileme yok → titreme yok)
  static unsigned long lastTimerRedraw = 0;
  if (timerRunning && isTimerScreen() && millis() - lastTimerRedraw > 1000) {
    lastTimerRedraw = millis();
    if (autoRefreshOk()) drawCurrentScreen();
  }
  // Anim A hafif yenileme
  // Anime kaldirildi

  {
    static String lastPill = "";
    String pl = homeNetPillText();
    if (pl != lastPill) {
      lastPill = pl;
      if (isHomeScreen() && autoRefreshOk() && !showErrorScreen) drawCurrentScreen();
    }
  }
  {
    static bool wasStale = false;
    bool st = homeDataStale();
    if (st != wasStale) {
      wasStale = st;
      if (isHomeScreen() && autoRefreshOk() && !showErrorScreen) drawCurrentScreen();
    }
  }
  wdtFeed();
  static unsigned long lastWifiCheck = 0;
  static bool wifiWasDown = false;
  if (wifiWasDown && WiFi.status() == WL_CONNECTED) {
    // arka planda yeniden baglandi: sunucu + hemen veri
    wifiWasDown = false;
    netNoInternet = false;
    if (!otaServerStarted) startOtaServer();
    clearFetchError();
    lastFetch = 0;
    // 3.4.28: WiFi hata ekrani acikken modem geri gelince ana sayfaya don (veri varsa)
    if (showErrorScreen && dataValid && !showGuide && subScreen == 0) {
      showErrorScreen = false; errorDismissed = false;
      currentScreen = homeScreenIdx();
      drawCurrentScreen();
    } else if (isHomeScreen() && autoRefreshOk() && !showErrorScreen) drawCurrentScreen();
  }
  if (millis() - lastWifiCheck > (wifiWasDown ? 5000UL : 10000UL)) {   // 3.4.8: 20 sn -> 10 sn / kopukken 5 sn
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      bool first = !wifiWasDown;
      wifiWasDown = true;
      setFetchError(1, savedWifiName());
      wifiBgReconnect();                 // bloklamaz - dokunmalar calismaya devam eder
      if (first && isHomeScreen() && autoRefreshOk() && !showErrorScreen) drawCurrentScreen();   // "WiFi yok" hapi
      else if (!isHomeScreen() && !showErrorScreen && !isSettingsScreen() && !showGuide && subScreen == 0 && autoRefreshOk()) {
        static unsigned long lastBanner = 0;
        if (millis() - lastBanner > 60000UL) {
          lastBanner = millis();
          drawWifiBanner();
        }
      }
    }
  }
  delay(20);
}
