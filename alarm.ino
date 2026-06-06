#include <Wire.h>

// PINY 
const int RS = 7, E = 6, D4 = 8, D5 = 3, D6 = 4, D7 = 5;
const int pinBuzzer  = 9;   // D9 zarezerwowany dla piezo - nie dostepny jako czujnik
const int pinLedRed  = 11;
const int pinLedBlue = 13;
const int pcfAddr    = 0x20;

#define LED_ON  LOW
#define LED_OFF HIGH

//  TYPY CZUJNIKOW 
#define SENSOR_NONE      0
#define SENSOR_PIR       1
#define SENSOR_DOOR      2  // czujnik ultradzwiekowy PING1
#define SENSOR_VIBRATION 3

//  STANY ALARMU 
#define ALARM_DISARMED   0
#define ALARM_ARMED      1
#define ALARM_EXIT_DELAY 2
#define ALARM_TRIGGERED  3
#define SENSOR_VIOLATED  4

//  CZUJNIK ULTRADZWIEKOWY
#define PIN_ULTRASONIC          10
#define ULTRASONIC_THRESHOLD_CM 50

//  CZUJNIKI 
// Zajete: D3-D8 (LCD), D9 (buzzer), D11/D13 (LED), A4/A5 (I2C)
// Dostepne: D2, D10, D12, A0, A1, A2
#define MAX_SENSORS    6
const byte AVAIL_PINS[]  = {2, 10, 12, A0, A1, A2};
const byte N_AVAIL_PINS  = 6;

struct Sensor {
  byte pin;
  byte type;
  bool active;
  bool violated;
};

Sensor sensors[MAX_SENSORS];
int sensorCount = 0;

//  STAN SYSTEMU
int           systemState    = ALARM_DISARMED;
unsigned long stateTimer     = 0;
const long    EXIT_DELAY_MS  = 2000UL; // 2s w symulatorze ~= 10s rzeczywiscie
int           lastShownSec   = -1;
int           triggeredSensor = -1;

//  MENU 
// menuLevel: 0=glowne 1=podmenu_czujniki 2=dodaj_pin 3=dodaj_typ
//            4=lista_czujniki 5=ustawienia 6=usun_czujnik
int menuLevel     = 0;
int menuIdx       = 0;
int addPinIdx     = 0;
int delSensorIdx  = 0;  // indeks czujnika do usuniecia
int addTypeIdx    = 0;
int viewSensorIdx = 0;

//  PRZYCISKI
unsigned long lastBtnMs    = 0;
const long    BTN_DEBOUNCE = 300;

// -----------------------------------------------------
// LCD HD44780
// 

void lcd_pulse() {
  digitalWrite(E, HIGH); delayMicroseconds(1);
  digitalWrite(E, LOW);  delayMicroseconds(100);
}

void lcd_send4bit(byte val) {
  digitalWrite(D4, (val >> 0) & 1);
  digitalWrite(D5, (val >> 1) & 1);
  digitalWrite(D6, (val >> 2) & 1);
  digitalWrite(D7, (val >> 3) & 1);
  lcd_pulse();
}

void lcd_command(byte cmd) {
  digitalWrite(RS, LOW);
  lcd_send4bit(cmd >> 4);
  lcd_send4bit(cmd & 0x0F);
  delay(2);
}

void lcd_char(char c) {
  digitalWrite(RS, HIGH);
  lcd_send4bit(c >> 4);
  lcd_send4bit(c & 0x0F);
  delayMicroseconds(50);
}

void lcd_print(const char* s)       { while (*s) lcd_char(*s++); }
void lcd_setCursor(byte col, byte row) { lcd_command((row == 0 ? 0x80 : 0xC0) + col); }
void lcd_clear()                    { lcd_command(0x01); delay(5); }

void lcd_printInt(int v) {
  char buf[8]; itoa(v, buf, 10); lcd_print(buf);
}

void lcd_printPad(const char* s, byte len = 16) {
  byte i = 0;
  for (; s[i] && i < len; i++) lcd_char(s[i]);
  for (; i < len; i++) lcd_char(' ');
}

void lcd_init() {
  pinMode(RS, OUTPUT); pinMode(E, OUTPUT);
  pinMode(D4, OUTPUT); pinMode(D5, OUTPUT);
  pinMode(D6, OUTPUT); pinMode(D7, OUTPUT);
  delay(50);
  lcd_send4bit(0x03); delay(5);
  lcd_send4bit(0x03); delay(1);
  lcd_send4bit(0x03);
  lcd_send4bit(0x02);
  lcd_command(0x28);
  lcd_command(0x0C);
  lcd_command(0x01);
  delay(5);
}

// -----------------------------------------
// WYSWIETLACZ PANELOWY - CD4511 przez PCF8574
// Wyswietlacz 7-seg
// 

void updatePanel(int state) {
  byte bcd  = (byte)(state & 0x0F);
  byte data = (bcd << 4) | 0x0F;
  Wire.beginTransmission(pcfAddr);
  Wire.write(data);
  Wire.endTransmission();
}

// ----------------------------------------
// CZUJNIK ULTRADZWIEKOWY
// 

long readUltrasonic() {
  pinMode(PIN_ULTRASONIC, OUTPUT);
  digitalWrite(PIN_ULTRASONIC, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_ULTRASONIC, HIGH); delayMicroseconds(5);
  digitalWrite(PIN_ULTRASONIC, LOW);
  pinMode(PIN_ULTRASONIC, INPUT);
  long dur = pulseIn(PIN_ULTRASONIC, HIGH, 30000);
  return dur / 58;
}

// -------------------------------------
// PRZYCISKI (UP=P0 DOWN=P1 OK=P2) - zwraca 0/1/2/3
// 

byte readButton() {
  if (millis() - lastBtnMs < BTN_DEBOUNCE) return 0;
  Wire.requestFrom(pcfAddr, 1);
  if (!Wire.available()) return 0;
  byte b = Wire.read();
  // Jesli PCF8574 nie odpowiada (0x00 = wszystkie LOW), ignoruj - to blad I2C
  if (b == 0x00) return 0;
  byte pressed = 0;
  if      (!(b & 0x01)) pressed = 1; // UP
  else if (!(b & 0x02)) pressed = 2; // DOWN
  else if (!(b & 0x04)) pressed = 3; // OK
  if (pressed) lastBtnMs = millis();
  return pressed;
}

// 
// POMOCNICZE
// 

const char* typeNameShort(byte t) {
  switch (t) {
    case SENSOR_PIR:       return "PIR";
    case SENSOR_DOOR:      return "ULTRADZ";
    case SENSOR_VIBRATION: return "WIBRACJA";
    default:               return "BRAK";
  }
}

bool isSensorTriggered(int i) {
  if (!sensors[i].active) return false;
  switch (sensors[i].type) {
    case SENSOR_PIR:  return digitalRead(sensors[i].pin) == HIGH;
    case SENSOR_DOOR: { long d = readUltrasonic(); return (d > 0 && d < ULTRASONIC_THRESHOLD_CM); }
    case SENSOR_VIBRATION: return digitalRead(sensors[i].pin) == LOW;
    default: return false;
  }
}

// ------------------------------------
// EKRANY LCD
// 

const char* MAIN_MENU[]    = {"Uzbroj alarm", "Rozbroj alarm", "Czujniki", "Ustawienia"};
const int   MAIN_MENU_SIZE = 4;

void showMainMenu() {
  lcd_clear();
  lcd_setCursor(0, 0); lcd_print("> "); lcd_printPad(MAIN_MENU[menuIdx], 14);
  lcd_setCursor(0, 1);
  switch (systemState) {
    case ALARM_DISARMED:   lcd_print("[ROZBROJONY]    "); break;
    case ALARM_ARMED:      lcd_print("[UZBROJONY]     "); break;
    case ALARM_EXIT_DELAY: lcd_print("[WYCHODZENIE]   "); break;
    case ALARM_TRIGGERED:  lcd_print("[!! ALARM !!]   "); break;
    case SENSOR_VIOLATED:  lcd_print("[NARUSZENIE]    "); break;
  }
}

const char* SENSOR_MENU[]    = {"Dodaj czujnik", "Lista czujnik.", "< Wroc"};
const int   SENSOR_MENU_SIZE = 3;

void showSensorMenu() {
  lcd_clear();
  lcd_setCursor(0, 0); lcd_print("-- CZUJNIKI --  ");
  lcd_setCursor(0, 1); lcd_print("> "); lcd_printPad(SENSOR_MENU[menuIdx], 14);
}

void showAddPin() {
  lcd_clear();
  lcd_setCursor(0, 0); lcd_print("Wybierz pin:    ");
  lcd_setCursor(0, 1); lcd_print("Pin: D");
  char buf[4]; itoa(AVAIL_PINS[addPinIdx], buf, 10); lcd_printPad(buf, 10);
}

const char* TYPE_NAMES[] = {"PIR", "ULTRADZ", "WIBRACJA"};

void showAddType() {
  lcd_clear();
  lcd_setCursor(0, 0); lcd_print("Typ czujnika:   ");
  lcd_setCursor(0, 1); lcd_print("> "); lcd_printPad(TYPE_NAMES[addTypeIdx], 14);
}

void showSensorList() {
  lcd_clear();
  if (sensorCount == 0) {
    lcd_setCursor(0, 0); lcd_print("Brak czujnikow  ");
    lcd_setCursor(0, 1); lcd_print("OK = wroc       ");
    return;
  }
  Sensor& s = sensors[viewSensorIdx];
  lcd_setCursor(0, 0);
  lcd_print("#"); lcd_printInt(viewSensorIdx + 1);
  lcd_print(" D"); lcd_printInt(s.pin);
  lcd_print(" "); lcd_printPad(typeNameShort(s.type), 8);
  lcd_setCursor(0, 1);
  if (isSensorTriggered(viewSensorIdx)) lcd_print("Stan: NARUSZONY ");
  else                                  lcd_print("Stan: OK        ");
}

// Podmenu ustawienia
const char* SETTINGS_MENU[]    = {"Usun czujnik", "< Wroc"};
const int   SETTINGS_MENU_SIZE = 2;

void showSettingsMenu() {
  lcd_clear();
  lcd_setCursor(0, 0); lcd_print("-- USTAWIENIA --");
  lcd_setCursor(0, 1); lcd_print("> "); lcd_printPad(SETTINGS_MENU[menuIdx], 14);
}

// Ekran usuwania czujnika
void showDeleteSensor() {
  lcd_clear();
  if (sensorCount == 0) {
    lcd_setCursor(0, 0); lcd_print("Brak czujnikow  ");
    lcd_setCursor(0, 1); lcd_print("OK = wroc       ");
    return;
  }
  Sensor& s = sensors[delSensorIdx];
  lcd_setCursor(0, 0);
  lcd_print("Usun #"); lcd_printInt(delSensorIdx + 1);
  lcd_print(" D"); lcd_printInt(s.pin); lcd_print("        ");
  lcd_setCursor(0, 1);
  lcd_print("OK=usun UP/DN=nav");
}

void showAlarmScreen(int sensorIdx) {
  lcd_clear();
  lcd_setCursor(0, 0); lcd_print("!! ALARM !!     ");
  lcd_setCursor(0, 1); lcd_print("CZUJNIK: ");
  if (sensorIdx >= 0) lcd_printPad(typeNameShort(sensors[sensorIdx].type), 7);
  else                lcd_print("NIEZNANY");
}

// --------------------------------------------
// OBSLUGA MENU
// 

void handleMenu(byte btn) {

  if (menuLevel == 0) {
    if (btn == 1) { menuIdx = (menuIdx - 1 + MAIN_MENU_SIZE) % MAIN_MENU_SIZE; showMainMenu(); }
    if (btn == 2) { menuIdx = (menuIdx + 1) % MAIN_MENU_SIZE;                  showMainMenu(); }
    if (btn == 3) {
      if (menuIdx == 0) {
        // Uzbroj - tylko gdy sa dodane czujniki
        if (systemState == ALARM_DISARMED && sensorCount > 0) {
          systemState = ALARM_EXIT_DELAY;
          stateTimer  = millis();
        } else if (sensorCount == 0) {
          lcd_clear();
          lcd_setCursor(0, 0); lcd_print("Brak czujnikow! ");
          lcd_setCursor(0, 1); lcd_print("Dodaj czujnik.  ");
          delay(1500);
          showMainMenu();
        }
      }
      else if (menuIdx == 1) {
        systemState = ALARM_DISARMED;
        noTone(pinBuzzer);
        for (int i = 0; i < MAX_SENSORS; i++) sensors[i].violated = false;
        showMainMenu();
      }
      else if (menuIdx == 2) { menuLevel = 1; menuIdx = 0; showSensorMenu(); }
      else if (menuIdx == 3) {
        menuLevel = 5; menuIdx = 0; showSettingsMenu();
      }
    }
  }

  else if (menuLevel == 1) {
    if (btn == 1) { menuIdx = (menuIdx - 1 + SENSOR_MENU_SIZE) % SENSOR_MENU_SIZE; showSensorMenu(); }
    if (btn == 2) { menuIdx = (menuIdx + 1) % SENSOR_MENU_SIZE;                    showSensorMenu(); }
    if (btn == 3) {
      if      (menuIdx == 0) { menuLevel = 2; addPinIdx = 0; showAddPin(); }
      else if (menuIdx == 1) { menuLevel = 4; viewSensorIdx = 0; showSensorList(); }
      else                   { menuLevel = 0; menuIdx = 0; showMainMenu(); }
    }
  }

  else if (menuLevel == 2) {
    if (btn == 1) { addPinIdx = (addPinIdx - 1 + N_AVAIL_PINS) % N_AVAIL_PINS; showAddPin(); }
    if (btn == 2) { addPinIdx = (addPinIdx + 1) % N_AVAIL_PINS;                showAddPin(); }
    if (btn == 3) { menuLevel = 3; addTypeIdx = 0; showAddType(); }
  }

  else if (menuLevel == 3) {
    if (btn == 1) { addTypeIdx = (addTypeIdx - 1 + 3) % 3; showAddType(); }
    if (btn == 2) { addTypeIdx = (addTypeIdx + 1) % 3;     showAddType(); }
    if (btn == 3) {
      if (sensorCount < MAX_SENSORS) {
        sensors[sensorCount].pin      = AVAIL_PINS[addPinIdx];
        sensors[sensorCount].type     = addTypeIdx + 1;
        sensors[sensorCount].active   = true;
        sensors[sensorCount].violated = false;
        if (addTypeIdx + 1 == SENSOR_PIR)
          pinMode(AVAIL_PINS[addPinIdx], INPUT);
        else if (addTypeIdx + 1 == SENSOR_VIBRATION)
          pinMode(AVAIL_PINS[addPinIdx], INPUT_PULLUP);
          sensorCount++;
        lcd_clear();
        lcd_setCursor(0, 0); lcd_print("Dodano czujnik! ");
        lcd_setCursor(0, 1); lcd_print("Czujnikow: "); lcd_printInt(sensorCount); lcd_print("     ");
        delay(1500);
      } else {
        lcd_clear();
        lcd_setCursor(0, 0); lcd_print("Brak miejsca!   ");
        lcd_setCursor(0, 1); lcd_print("Max=6 czujniki. ");
        delay(1500);
      }
      menuLevel = 0; menuIdx = 0; showMainMenu();
    }
  }

  else if (menuLevel == 4) {
    if (btn == 1 && sensorCount > 0) { viewSensorIdx = (viewSensorIdx - 1 + sensorCount) % sensorCount; showSensorList(); }
    if (btn == 2 && sensorCount > 0) { viewSensorIdx = (viewSensorIdx + 1) % sensorCount;               showSensorList(); }
    if (btn == 3) { menuLevel = 1; menuIdx = 0; showSensorMenu(); }
  }

  // POZIOM 5: USTAWIENIA
  else if (menuLevel == 5) {
    if (btn == 1) { menuIdx = (menuIdx - 1 + SETTINGS_MENU_SIZE) % SETTINGS_MENU_SIZE; showSettingsMenu(); }
    if (btn == 2) { menuIdx = (menuIdx + 1) % SETTINGS_MENU_SIZE;                      showSettingsMenu(); }
    if (btn == 3) {
      if (menuIdx == 0) {              // Usun czujnik
        menuLevel    = 6;
        delSensorIdx = 0;
        showDeleteSensor();
      }
      else {                           // Wroc
        menuLevel = 0; menuIdx = 0; showMainMenu();
      }
    }
  }

  // POZIOM 6: USUWANIE CZUJNIKA
  else if (menuLevel == 6) {
    if (sensorCount == 0) {
      if (btn == 3) { menuLevel = 5; menuIdx = 0; showSettingsMenu(); }
      return;
    }
    if (btn == 1) { delSensorIdx = (delSensorIdx - 1 + sensorCount) % sensorCount; showDeleteSensor(); }
    if (btn == 2) { delSensorIdx = (delSensorIdx + 1) % sensorCount;               showDeleteSensor(); }
    if (btn == 3) {
      for (int i = delSensorIdx; i < sensorCount - 1; i++)
        sensors[i] = sensors[i + 1];
      sensorCount--;
      sensors[sensorCount] = {0, SENSOR_NONE, false, false};
      if (delSensorIdx >= sensorCount && delSensorIdx > 0) delSensorIdx--;
      lcd_clear();
      lcd_setCursor(0, 0); lcd_print("Usunieto!       ");
      lcd_setCursor(0, 1); lcd_print("Czujnikow: "); lcd_printInt(sensorCount); lcd_print("     ");
      delay(1500);
      showDeleteSensor(); // pokaz kolejny lub "Brak czujnikow"
    }
  }
}

// --------------------------------
// LOGIKA ALARMU
// 

void manageAlarm() {
  updatePanel(systemState);

  if (systemState == ALARM_EXIT_DELAY) {
    unsigned long elapsed = millis() - stateTimer;
    if (elapsed >= EXIT_DELAY_MS) {
      systemState = ALARM_ARMED;
      lastShownSec = -1;
      showMainMenu();
      return;
    }
    int remaining = (int)((EXIT_DELAY_MS - elapsed) / 200) + 1;
    if (remaining != lastShownSec) {
      lastShownSec = remaining;
      lcd_setCursor(0, 0); lcd_print("WYJDZ!          ");
      lcd_setCursor(0, 1); lcd_print("Pozostalo: "); lcd_printInt(remaining); lcd_print("s  ");
    }
    return;
  }

  if (systemState == ALARM_ARMED) {
    for (int i = 0; i < sensorCount; i++) {
      if (isSensorTriggered(i)) {
        systemState      = ALARM_TRIGGERED;
        triggeredSensor  = i;
        showAlarmScreen(i);
        return;
      }
    }
  }

  if (systemState == SENSOR_VIOLATED) {
    for (int i = 0; i < sensorCount; i++)
      sensors[i].violated = isSensorTriggered(i);
  }
}

// -------------------------------------
// LED + BUZZER
// 

void handleSignals() {
  bool blink = (millis() / 300) % 2;

  switch (systemState) {
    case ALARM_TRIGGERED:
      digitalWrite(pinLedRed, blink ? LED_ON : LED_OFF);
      if (triggeredSensor >= 0) {
        if (blink) tone(pinBuzzer, 1000); else noTone(pinBuzzer);
      }
      digitalWrite(pinLedBlue, LED_OFF);
      break;
    case ALARM_ARMED:
      digitalWrite(pinLedBlue, LED_ON);
      digitalWrite(pinLedRed,  LED_OFF);
      noTone(pinBuzzer);
      break;
    case ALARM_EXIT_DELAY:
      digitalWrite(pinLedBlue, blink ? LED_ON : LED_OFF);
      digitalWrite(pinLedRed,  LED_OFF);
      noTone(pinBuzzer);
      break;
    case SENSOR_VIOLATED: {
      bool any = false;
      for (int i = 0; i < sensorCount; i++) if (sensors[i].violated) { any = true; break; }
      digitalWrite(pinLedRed,  any ? LED_ON : LED_OFF);
      digitalWrite(pinLedBlue, LED_OFF);
      noTone(pinBuzzer);
      break;
    }
    default:
      digitalWrite(pinLedRed,  LED_OFF);
      digitalWrite(pinLedBlue, LED_OFF);
      noTone(pinBuzzer);
      break;
  }
}

// -------------------------------------
// SETUP & LOOP
// 

void setup() {
  Wire.begin();
  lcd_init();

  pinMode(pinBuzzer,  OUTPUT);
  pinMode(pinLedRed,  OUTPUT);
  pinMode(pinLedBlue, OUTPUT);
  digitalWrite(pinLedRed,  LED_OFF);
  digitalWrite(pinLedBlue, LED_OFF);
  noTone(pinBuzzer); // wyciszenie przy starcie

  // Ekran startowy
  lcd_setCursor(0, 0); lcd_print("SYSTEM ALARMOWY ");
  lcd_setCursor(0, 1); lcd_print("START...        ");
  delay(2000);

  showMainMenu();
  updatePanel(ALARM_DISARMED);
}

void loop() {
  byte btn = readButton();

  if (systemState == ALARM_TRIGGERED) {
    if (btn == 3) {
      systemState      = ALARM_DISARMED;
      triggeredSensor  = -1;
      noTone(pinBuzzer);
      for (int i = 0; i < sensorCount; i++) sensors[i].violated = false;
      menuLevel = 0; menuIdx = 0;
      showMainMenu();
    }
  }
  else if (systemState == ALARM_EXIT_DELAY) {
    if (btn == 3) {
      systemState  = ALARM_DISARMED;
      lastShownSec = -1;
      noTone(pinBuzzer);
      menuLevel = 0; menuIdx = 0;
      showMainMenu();
    }
  }
  else {
    if (btn) handleMenu(btn);
  }

  manageAlarm();
  handleSignals();
}

