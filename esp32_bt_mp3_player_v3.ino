/*
  ESP32 Bluetooth MP3 Player - Device list (Mini boost 4 / AirPods) + connect
  Board: "ESP32 Dev Module", Partition Scheme: "Huge APP (3MB No OTA / 1MB SPIFFS)"

  FLOW:
    Boot -> device list (Prev/Next scroll, Play = select) -> connect -> Song List
    Connecting screen par Prev = device list par wapas
    Song list: Prev/Next = scroll, Play = gaana chalao
    Player:    Play = pause/resume, Play hold = song list,
               Next/Prev tap = agla/pichla, Next hold = Vol-, Prev hold = Vol+
    Connecting screen par Play dabao = connect hone ka wait chhod ke song list.

  Scan/select wala poora function hata diya hai.
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <vector>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/Communication/A2DPStream.h"
#include "esp_bt.h"
#include "esp_bt_main.h"

// ================= USER CONFIG =================
// Device list - naam EXACT likho (phone ke Bluetooth settings me jaisa dikhta hai, capital/space sab same)
// Aur device add karna ho to bas yahan ek naam aur jod do.
const char* DEVICES[] = {
  "Mini boost 4",
  "AirPods"
};
#define NUM_DEVICES ((int)(sizeof(DEVICES) / sizeof(DEVICES[0])))
const char* targetDevice = "";     // select karne ke baad yahan set hota hai

#define SD_CS     5
#define BTN_PREV  25
#define BTN_PLAY  26
#define BTN_NEXT  27

#define OLED_ADDR 0x3C
#define SCREEN_W  128
#define SCREEN_H  64

#define LONG_PRESS_MS   500
#define VOL_REPEAT_MS   100
#define VOL_STEP        2
#define VOL_DEFAULT     50
#define DEBOUNCE_MS     30

#define CONNECT_WAIT_MS 60000    // itni der connect hone ka wait (Play dabao to skip)

// Lamba gaane ka naam scroll hota hai (Spotify jaisa)
#define MARQUEE_DELAY_MS  1500   // gaana shuru hone ke baad itni der naam ruka rahega
#define MARQUEE_STEP_MS   250    // itne ms me ek step (kam karoge to smooth, par audio atak sakta hai)
#define MARQUEE_STEP_PX   4      // ek step me kitne pixel khiskega

#define VOL_POPUP_MS      1500   // volume popup kitni der dikhe
// ===============================================

enum AppState {
  STATE_TRACK_LIST,
  STATE_PLAYER
};

AppState currentState = STATE_TRACK_LIST;

struct Btn {
  uint8_t pin;
  bool lastRead;
  bool stable;
  uint32_t tChange;
  uint32_t tDown;
  uint32_t tRepeat;
  bool longFired;
};

enum {
  EV_NONE = 0,
  EV_SHORT,
  EV_LONG_TICK,
  EV_LONG_RELEASE
};

Btn bPrev{BTN_PREV, HIGH, HIGH, 0, 0, 0, false};
Btn bPlay{BTN_PLAY, HIGH, HIGH, 0, 0, 0, false};
Btn bNext{BTN_NEXT, HIGH, HIGH, 0, 0, 0, false};

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
A2DPStream a2dp;
EncodedAudioStream decoder(&a2dp, new MP3DecoderHelix());
Preferences prefs;

std::vector<String> tracks;
int  current   = 0;
bool playing   = false;
bool btWasConn = false;
File mp3File;
uint8_t buf[2048];

int  vol = VOL_DEFAULT;
bool uiDirty = true;
uint32_t lastUI = 0;

uint32_t playedMs = 0;
uint32_t lastTick = 0;
uint32_t trackDurationSec = 180;
uint32_t connStartMs = 0;

uint32_t marqueeStart = 0;       // naya gaana shuru hone ka time (scroll yahin se gino)
bool     marqueeOn = false;      // abhi naam scroll ho raha hai ya nahi
uint32_t volShowUntil = 0;       // volume popup is time tak dikhega (0 = hidden)

// =====================================================
//                       HELPERS
// =====================================================
// ---------- UI helpers (sirf display ke liye, logic se koi lena-dena nahi) ----------
#define UI_W 128
#define UI_H 64

// Text ko screen ke beech me likho (har letter 6px chauda)
void drawCentered(const String& s, int y, uint16_t color = SSD1306_WHITE) {
  int w = (int)s.length() * 6;
  display.setTextColor(color);
  display.setCursor(max(0, (UI_W - w) / 2), y);
  display.print(s);
}

// Lamba text ho to ".." laga ke chhota karo
String fitText(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 2) + "..";
}

String fmtTime(uint32_t sec) {
  char b[12];
  snprintf(b, sizeof(b), "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
  return String(b);
}

// Bluetooth ka chhota icon (5x9). linked=false ho to upar se cut-line lagti hai
void drawBtIcon(int x, int y, uint16_t c, bool linked) {
  display.drawLine(x + 2, y, x + 2, y + 8, c);
  display.drawLine(x + 2, y, x + 4, y + 2, c);
  display.drawLine(x + 4, y + 2, x, y + 6, c);
  display.drawLine(x, y + 2, x + 4, y + 6, c);
  display.drawLine(x + 4, y + 6, x + 2, y + 8, c);
  if (!linked) display.drawLine(x - 1, y + 9, x + 5, y - 1, c);
}

// Speaker ka chhota icon
void drawSpeaker(int x, int y) {
  display.fillRect(x, y + 2, 3, 4, SSD1306_WHITE);
  display.fillTriangle(x + 3, y + 2, x + 7, y - 1, x + 7, y + 8, SSD1306_WHITE);
  display.fillTriangle(x + 3, y + 2, x + 3, y + 5, x + 7, y + 8, SSD1306_WHITE);
}

// Upar ki title bar (safed patti, kaala text)
void drawHeader(const String& title, bool showBt) {
  display.fillRect(0, 0, UI_W, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(3, 2);
  display.print(title);
  if (showBt) drawBtIcon(UI_W - 9, 1, SSD1306_BLACK, a2dp.source().is_connected());
  display.setTextColor(SSD1306_WHITE);
}

// Right side ka scrollbar (list lambi ho tab dikhta hai)
void drawScrollbar(int total, int first, int visible, int y0, int h) {
  if (total <= visible) return;
  display.drawFastVLine(UI_W - 2, y0, h, SSD1306_WHITE);
  int thumbH = max(4, h * visible / total);
  int thumbY = y0 + (h - thumbH) * first / (total - visible);
  display.fillRect(UI_W - 3, thumbY, 3, thumbH, SSD1306_WHITE);
}

void showMsg(const char* l1, const char* l2 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.drawRoundRect(0, 0, UI_W, UI_H, 4, SSD1306_WHITE);
  drawCentered(l1, 22);
  if (l2[0]) drawCentered(l2, 36);
  display.display();
}

int btnUpdate(Btn &b) {
  uint32_t now = millis();
  bool r = digitalRead(b.pin);
  if (r != b.lastRead) {
    b.lastRead = r;
    b.tChange = now;
  }

  if (now - b.tChange >= DEBOUNCE_MS && r != b.stable) {
    b.stable = r;
    if (r == LOW) {
      b.tDown = now;
      b.longFired = false;
    } else {
      return b.longFired ? EV_LONG_RELEASE : EV_SHORT;
    }
  }

  if (b.stable == LOW && now - b.tDown >= LONG_PRESS_MS) {
    if (!b.longFired) {
      b.longFired = true;
      b.tRepeat = now - VOL_REPEAT_MS;
    }
    if (now - b.tRepeat >= VOL_REPEAT_MS) {
      b.tRepeat = now;
      return EV_LONG_TICK;
    }
  }
  return EV_NONE;
}

void applyVolume() {
  if (vol == 0) {
    a2dp.source().set_volume(0);
  } else {
    uint8_t v = (uint8_t)map(vol, 1, 100, 5, 127);
    a2dp.source().set_volume(v);
  }
}

void saveVolume() {
  prefs.begin("player", false);
  prefs.putUChar("vol", (uint8_t)vol);
  prefs.end();
  Serial.printf("[VOL] Saved volume to flash: %d%%\n", vol);
}

void changeVolume(int delta) {
  int nv = constrain(vol + delta, 0, 100);
  if (nv != vol) {
    vol = nv;
    applyVolume();
  }
  volShowUntil = millis() + VOL_POPUP_MS;   // hold karte waqt popup dikhao (limit par bhi)
  uiDirty = true;
}

// ---------- Tracks (subfolder support) ----------
void scanTracks() {
  tracks.clear();
  File root = SD.open("/");
  if (!root) {
    Serial.println("[SD] Failed to open /");
    return;
  }

  Serial.println("[SD] Scanning for MP3 files...");
  while (true) {
    File f = root.openNextFile();
    if (!f) break;

    if (f.isDirectory()) {
      File sub = SD.open(f.path());
      if (sub) {
        while (true) {
          File sf = sub.openNextFile();
          if (!sf) break;
          if (!sf.isDirectory()) {
            String sn = sf.name();
            String slow = sn;
            slow.toLowerCase();
            if (slow.endsWith(".mp3") && sn.indexOf("._") == -1) {
              String sp = sf.path();
              tracks.push_back((sp.length() > 0) ? sp : String(f.path()) + "/" + sn);
              Serial.printf("[SD] Found [%d]: %s\n", (int)tracks.size(), tracks.back().c_str());
            }
          }
          sf.close();
          if (tracks.size() >= 100) break;
        }
        sub.close();
      }
    } else {
      String n = f.name();
      String low = n;
      low.toLowerCase();
      if (low.endsWith(".mp3") && n.indexOf("._") == -1) {
        String p = f.path();
        tracks.push_back((p.length() > 0) ? p : (n.startsWith("/") ? n : "/" + n));
        Serial.printf("[SD] Found [%d]: %s\n", (int)tracks.size(), tracks.back().c_str());
      }
    }
    f.close();
    if (tracks.size() >= 100) break;
  }
  root.close();
  Serial.printf("[SD] Scan complete. Total tracks: %d\n", (int)tracks.size());
}

void openTrack(int idx) {
  if (tracks.empty()) return;
  if (mp3File) mp3File.close();
  decoder.end();
  decoder.begin();
  int n = (int)tracks.size();
  current = ((idx % n) + n) % n;
  mp3File = SD.open(tracks[current].c_str());
  uint32_t dur = mp3File ? (uint32_t)(mp3File.size() / 16000) : 180;   // 128kbps estimate
  trackDurationSec = dur < 1 ? 1 : dur;
  playedMs = 0;
  marqueeStart = millis();                // naam scroll shuru se
  playing = true;
  uiDirty = true;
  Serial.printf("[PLAY] Playing track %d: %s\n", current + 1, tracks[current].c_str());
}

String getDisplayName(String path) {
  int lastSlash = path.lastIndexOf('/');
  String name = (lastSlash != -1) ? path.substring(lastSlash + 1) : path;
  String low = name;
  low.toLowerCase();
  if (low.endsWith(".mp3")) {
    name = name.substring(0, name.length() - 4);
  }
  return name;
}

// =====================================================
//                         UI
// =====================================================
void drawConnectingUI() {
  display.clearDisplay();
  display.setTextSize(1);
  drawHeader("Connecting", false);

  // Device ka naam box me
  display.drawRoundRect(2, 16, UI_W - 4, 16, 4, SSD1306_WHITE);
  drawCentered(fitText(String(targetDevice), 19), 20);

  // Chalti hui animated bar (aage-peeche)
  const int barX = 6, barW = UI_W - 12, blk = 28;
  display.drawRoundRect(barX, 37, barW, 7, 3, SSD1306_WHITE);
  int span = barW - blk - 4;
  int p = (int)((millis() / 10) % (unsigned long)(span * 2));
  if (p > span) p = span * 2 - p;
  display.fillRoundRect(barX + 2 + p, 39, blk, 3, 1, SSD1306_WHITE);

  uint32_t secs = (millis() - connStartMs) / 1000;
  drawCentered("Waiting... " + String(secs) + "s", 47);
  drawCentered("Play=Skip Prev=Back", 56);
  display.display();
}

void drawTrackListUI() {
  display.clearDisplay();
  display.setTextSize(1);
  int total = (int)tracks.size();

  String title = "Songs";
  if (total > 0) title = "Songs  " + String(current + 1) + "/" + String(total);
  drawHeader(title, true);

  if (total == 0) {
    drawCentered("No MP3 on SD", 26);
    drawCentered("Check card format", 40);
    display.display();
    return;
  }

  // 4 rows, chuna hua gaana highlight (safed patti, kaala text)
  const int rows = 4;
  int first = constrain(current - 1, 0, max(0, total - rows));
  for (int r = 0; r < rows && first + r < total; r++) {
    int i = first + r;
    int y = 14 + r * 12;
    String label = fitText(String(i + 1) + ". " + getDisplayName(tracks[i]), 19);
    if (i == current) {
      display.fillRoundRect(0, y, UI_W - 6, 11, 2, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(3, y + 2);
    display.print(label);
  }
  display.setTextColor(SSD1306_WHITE);
  drawScrollbar(total, first, rows, 14, 48);
  display.display();
}

void drawPlayerUI() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Upar: left me Bluetooth icon, right corner me chhota track number
  drawBtIcon(3, 0, SSD1306_WHITE, a2dp.source().is_connected());
  String tn = String(current + 1) + "/" + String((int)tracks.size());
  display.setCursor(UI_W - (int)tn.length() * 6 - 1, 1);
  display.print(tn);

  // Gaane ka naam - hamesha ek hi line me. Lamba ho to Spotify jaisa scroll hota hai
  String name = tracks.empty() ? String("No Track") : getDisplayName(tracks[current]);
  int textW = (int)name.length() * 6;
  marqueeOn = (textW > UI_W - 4);
  if (!marqueeOn) {
    drawCentered(name, 17);
  } else {
    const int gap = 36;                              // naam ke repeat ke beech ka khaali hissa
    int cycle = textW + gap;
    uint32_t t = millis() - marqueeStart;
    int off = 0;
    if (t > MARQUEE_DELAY_MS) {
      off = (int)((((t - MARQUEE_DELAY_MS) / MARQUEE_STEP_MS) * MARQUEE_STEP_PX) % (uint32_t)cycle);
    }
    display.setCursor(2 - off, 17);
    display.print(name);
    display.setCursor(2 - off + cycle, 17);          // peeche se naam dobara aata hua
    display.print(name);
  }

  // Controls: prev | play/pause | next
  const int cy = 37;
  display.fillRect(30, cy - 5, 2, 11, SSD1306_WHITE);
  display.fillTriangle(43, cy - 5, 43, cy + 5, 33, cy, SSD1306_WHITE);
  if (playing) {
    display.fillRect(57, cy - 5, 4, 11, SSD1306_WHITE);
    display.fillRect(67, cy - 5, 4, 11, SSD1306_WHITE);
  } else {
    display.fillTriangle(58, cy - 6, 58, cy + 6, 71, cy, SSD1306_WHITE);
  }
  display.fillTriangle(85, cy - 5, 85, cy + 5, 95, cy, SSD1306_WHITE);
  display.fillRect(96, cy - 5, 2, 11, SSD1306_WHITE);

  // Progress bar
  uint32_t elapsed = playedMs / 1000;
  uint32_t dur = trackDurationSec < 1 ? 1 : trackDurationSec;
  int fillW = constrain((int)((elapsed * (UI_W - 4)) / dur), 0, UI_W - 4);
  display.drawRoundRect(0, 47, UI_W, 5, 2, SSD1306_WHITE);
  if (fillW > 0) display.fillRect(2, 48, fillW, 3, SSD1306_WHITE);

  // Neeche: time (left) | total time (right, ~ = andaza)
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 55);
  display.print(fmtTime(elapsed));

  String tot = "~" + fmtTime(dur);
  display.setCursor(UI_W - (int)tot.length() * 6, 55);
  display.print(tot);

  // Volume popup - sirf V+/V- hold karne par dikhta hai, phir apne aap gayab (phone jaisa)
  if (volShowUntil != 0 && (int32_t)(millis() - volShowUntil) < 0) {
    display.fillRoundRect(8, 19, 112, 24, 6, SSD1306_BLACK);
    display.drawRoundRect(8, 19, 112, 24, 6, SSD1306_WHITE);
    drawSpeaker(15, 27);
    display.drawRoundRect(28, 28, 60, 7, 3, SSD1306_WHITE);
    int vw = (vol * 56) / 100;
    if (vw > 0) display.fillRect(30, 30, vw, 3, SSD1306_WHITE);
    String v = String(vol) + "%";
    display.setCursor(115 - (int)v.length() * 6, 28);
    display.print(v);
  }

  display.display();
}

// =====================================================
//                    SETUP / LOOP
// =====================================================
// Device list screen: Prev/Next = scroll, Play = select. Chuna hua index return karta hai.
int selectDevice() {
  int n = NUM_DEVICES;
  prefs.begin("player", true);
  int sel = constrain((int)prefs.getUChar("lastDev", 0), 0, n - 1);   // pichla device pehle se highlighted
  prefs.end();

  bool dirty = true;
  while (true) {
    if (btnUpdate(bPrev) == EV_SHORT) { sel = (sel - 1 + n) % n; dirty = true; }
    if (btnUpdate(bNext) == EV_SHORT) { sel = (sel + 1) % n; dirty = true; }
    if (btnUpdate(bPlay) == EV_SHORT) break;

    if (dirty) {
      dirty = false;
      display.clearDisplay();
      display.setTextSize(1);
      drawHeader("Select device", false);

      const int rows = 3;
      int first = constrain(sel - 1, 0, max(0, n - rows));
      for (int r = 0; r < rows && first + r < n; r++) {
        int i = first + r;
        int y = 15 + r * 13;
        uint16_t fg = SSD1306_WHITE;
        if (i == sel) {
          display.fillRoundRect(0, y, UI_W - 6, 12, 3, SSD1306_WHITE);
          fg = SSD1306_BLACK;
        }
        drawBtIcon(5, y + 1, fg, true);
        display.setTextColor(fg);
        display.setCursor(17, y + 2);
        display.print(fitText(String(DEVICES[i]), 17));
      }
      display.setTextColor(SSD1306_WHITE);
      drawScrollbar(n, first, rows, 15, 39);
      drawCentered("Prev/Next  Play=OK", 56);
      display.display();
    }
    delay(5);
  }

  prefs.begin("player", false);
  prefs.putUChar("lastDev", (uint8_t)sel);
  prefs.end();
  return sel;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n====================================");
  Serial.println("  ESP32 BT MP3 Player (AirPods fixed)");
  Serial.println("====================================");

  setCpuFrequencyMhz(240);

  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);

  prefs.begin("player", true);
  vol = constrain((int)prefs.getUChar("vol", VOL_DEFAULT), 0, 100);
  prefs.end();

  Wire.begin(21, 22);
  Wire.setClock(400000);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.setTextWrap(false);          // lamba text agli line me na toote (scroll ke liye zaroori)
  showMsg("MP3 Player", "Starting...");

  // SD (default speed pehle - v9.1 jaisa; fail ho to slow fallback)
  showMsg("Loading SD card...");
  bool sdOk = SD.begin(SD_CS);
  if (!sdOk) {
    Serial.println("[SD] default failed. Trying 8MHz...");
    sdOk = SD.begin(SD_CS, SPI, 8000000);
  }
  if (!sdOk) {
    Serial.println("[SD] 8MHz failed. Trying 4MHz safe mode...");
    sdOk = SD.begin(SD_CS, SPI, 4000000);
  }
  if (!sdOk) {
    Serial.println("[SD] SD init failed completely!");
    showMsg("SD init failed!", "Check card & wiring");
    delay(2000);
  } else {
    Serial.println("[SD] SD mounted successfully!");
    scanTracks();
    char sm[24];
    snprintf(sm, sizeof(sm), "%d songs found", (int)tracks.size());
    showMsg("SD card OK", sm);
  }

  // Device chuno (SD/songs ka kaam upar ho chuka hai, isliye select ke turant baad BT shuru hota hai)
  targetDevice = DEVICES[selectDevice()];

  // Bluetooth A2DP Source -> chune hue device se connect
  Serial.printf("[BT] Connecting to: %s\n", targetDevice);
  // (BLE mem_release wali line hata di - Bluetooth start ko hang kar sakti thi)
  showMsg("Starting Bluetooth", targetDevice);
  auto cfg = a2dp.defaultConfig(TX_MODE);
  cfg.name = targetDevice;
  cfg.auto_reconnect = false;
  cfg.silence_on_nodata = true;
  cfg.delay_ms = 0;
  a2dp.begin(cfg);
  showMsg("Bluetooth ready");
  Serial.println("[BT] a2dp.begin done");

  esp_err_t err = esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9);
  if (err == ESP_OK) {
    Serial.println("[BT] TX Power set to MAX (+9 dBm)");
  } else {
    Serial.printf("[BT] TX Power set returned: 0x%x\n", err);
  }

  applyVolume();
  Serial.printf("[MEM] Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());

  // Connect hone tak wait (Play dabao = skip)
  connStartMs = millis();
  while (millis() - connStartMs < CONNECT_WAIT_MS) {
    if (a2dp.source().is_connected()) break;
    if (btnUpdate(bPlay) == EV_SHORT) break;
    if (btnUpdate(bPrev) == EV_SHORT) {          // Back = device list par wapas (restart)
      showMsg("Restarting...");
      delay(300);
      ESP.restart();
    }
    drawConnectingUI();
    delay(100);
  }

  lastTick = millis();
  currentState = STATE_TRACK_LIST;
  playing = false;
  uiDirty = true;
}

void loop() {
  // 1. Buttons
  switch (currentState) {
    case STATE_TRACK_LIST: {
      if (btnUpdate(bPrev) == EV_SHORT) {
        if (!tracks.empty()) {
          current = (current - 1 + tracks.size()) % tracks.size();
          uiDirty = true;
        }
      }
      if (btnUpdate(bNext) == EV_SHORT) {
        if (!tracks.empty()) {
          current = (current + 1) % tracks.size();
          uiDirty = true;
        }
      }
      if (btnUpdate(bPlay) == EV_SHORT) {
        if (!tracks.empty()) {
          openTrack(current);
          currentState = STATE_PLAYER;
          uiDirty = true;
        }
      }
      break;
    }

    case STATE_PLAYER: {
      int pEv = btnUpdate(bPlay);
      if (pEv == EV_SHORT) {
        playing = !playing;
        uiDirty = true;
      } else if (pEv == EV_LONG_RELEASE) {
        currentState = STATE_TRACK_LIST;
        uiDirty = true;
        break;
      }

      int e = btnUpdate(bNext);
      if      (e == EV_SHORT)        { openTrack(current + 1); currentState = STATE_PLAYER; }
      else if (e == EV_LONG_TICK)    changeVolume(-VOL_STEP);
      else if (e == EV_LONG_RELEASE) saveVolume();

      e = btnUpdate(bPrev);
      if      (e == EV_SHORT)        { openTrack(current - 1); currentState = STATE_PLAYER; }
      else if (e == EV_LONG_TICK)    changeVolume(VOL_STEP);
      else if (e == EV_LONG_RELEASE) saveVolume();
      break;
    }
  }

  // 2. Played-time (pause me ruk jaata hai)
  uint32_t nowMs = millis();
  bool conn = a2dp.source().is_connected();
  if (playing && conn) playedMs += nowMs - lastTick;
  lastTick = nowMs;

  // 3. Playback engine (simple, v9.1)
  if (playing && conn && mp3File) {
    size_t got = mp3File.read(buf, sizeof(buf));
    if (got == 0) {
      openTrack(current + 1);            // track khatam -> next
    } else {
      size_t off = 0;
      while (off < got) {
        size_t w = decoder.write(buf + off, got - off);
        off += w;
        if (w == 0) {
          delay(1);                      // BT buffer full, radio ko time do
        }
      }
    }
  } else {
    delay(5);
  }

  // 4. Bluetooth connection monitor
  if (conn != btWasConn) {
    btWasConn = conn;
    uiDirty = true;
    if (conn) {
      Serial.println("[BT] Connected to Bluetooth device!");
      esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9);
      applyVolume();
    } else {
      Serial.println("[BT] Disconnected from Bluetooth device!");
    }
  }

  // 5. UI refresh
  uint32_t now = millis();
  if (volShowUntil != 0 && (int32_t)(now - volShowUntil) >= 0) {   // volume popup ka time khatam -> hatao
    volShowUntil = 0;
    uiDirty = true;
  }
  uint32_t period = marqueeOn ? MARQUEE_STEP_MS : 1000;            // scroll ho raha ho to jaldi refresh
  if (uiDirty || (currentState == STATE_PLAYER && playing && (now - lastUI >= period))) {
    if (currentState == STATE_TRACK_LIST) drawTrackListUI();
    else if (currentState == STATE_PLAYER) drawPlayerUI();
    lastUI = now;
    uiDirty = false;
  }
}
