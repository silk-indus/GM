#include <Arduino.h>

#include <LiquidCrystal.h>

// Create An LCD Object. Signals: [ RS, EN, D4, D5, D6, D7 ]
LiquidCrystal My_LCD(13, 12, 14, 27, 26, 25);

#define Button_pin 0
#define Buttons_pin 33
#define GM_pin 15
#define LED_pin 2

uint16_t RIGHT = 500;
uint16_t UP = 1500;
uint16_t DOWN = 2200;
uint16_t LEFT = 3200;

uint8_t page = 0;
const uint8_t PAGES = 4;
uint8_t page_m = 0;
const uint8_t PAGES_M = 2;
bool menu = false;
bool edit = false;
int counter;
int counter_interval = 0;
float last_interval_cpm = 0;
unsigned long msec_interval = 10000;
unsigned long time_interval = 0;
const float STEP_msec = 1000;
int lastbutton = 1;
char let;
char let_last;
uint16_t button_stat;
float c_uSv = 154;
const float STEP_CuSV = 2;
const unsigned int BUFFER = 10;
static unsigned long times[BUFFER];
static unsigned starttime;
float cpm;

void IRAM_ATTR count_impulse()
{
  digitalWrite(LED_pin, !digitalRead(LED_pin));
  counter++;
  byte c0;
  c0 = counter % 10;
  times[c0] = millis();
}

void setup()
{
  counter = 0;
  starttime = millis();
  Serial.begin(115200);
  Serial.println("GM test");
  pinMode(LED_pin, OUTPUT);
  pinMode(Button_pin, INPUT);
  pinMode(Buttons_pin, INPUT);
  pinMode(GM_pin, INPUT);
  attachInterrupt(GM_pin, count_impulse, FALLING);

  My_LCD.begin(16, 2);
  My_LCD.clear();
  My_LCD.print("Geiger Muller c.");
  My_LCD.setCursor(0, 1);
  My_LCD.print(" KFMT FHPV 2023 ");
}

void loop()
{
  if ((millis() - time_interval) > msec_interval)
  {
    last_interval_cpm = 60000.0 * (counter - counter_interval) / (millis() - time_interval);
    counter_interval = counter;
    time_interval = millis();
  }

  cpm = counter > 9 ? (600000.0 / (millis() - times[(counter + 1) % 10])) : counter * 60000.0 / (millis() - times[0]);
  Serial.printf("CPM: %.0f uSv/h %.3f Avg %.3f Status: %d counter: %d\n", cpm, cpm / c_uSv, float(counter * 60000) / (float(millis() - starttime) * c_uSv), digitalRead(LED_pin), counter);
  if (digitalRead(Button_pin) == 0)
  {
    if (lastbutton == 1)
    {
      counter = 0;
      starttime = millis();
      lastbutton = 0;
    }
  }
  else
    lastbutton = 1;

  button_stat = analogRead(Buttons_pin);

  let_last = let;
  let = ' ';
  if (button_stat < LEFT)
    let = 'L';
  if (button_stat < DOWN)
    let = 'D';
  if (button_stat < UP)
    let = 'U';
  if (button_stat < RIGHT)
    let = 'R';

  if (let_last != let)
  {
    if (!menu)
    {
      if (let == 'U')
        page = (page + 1) % PAGES;
      if (let == 'D')
        page = page == 0 ? PAGES - 1 : (page - 1) % PAGES;
      if (let == 'R')
        menu = true;
      if (let == 'L')
      {
        counter = 0;
        starttime = millis();
      }
    }
    else
    {
      if (let == 'U')
        if (!edit)
          page_m = (page_m + 1) % PAGES_M;
      if (let == 'D')
        if (!edit)
          page_m = page_m == 0 ? PAGES_M - 1 : (page_m - 1) % PAGES_M;
      if (let == 'R')
      {
        if (!edit)
          edit = true;
        else
          edit = edit;
      }
      if (let == 'L')
      {
        if (!edit)
          menu = false;
        else
          edit = false;
      }
    }
  }

  if (!menu)
  {
    if (page == 0)
    {
      My_LCD.setCursor(0, 0);
      My_LCD.printf("CPM: %.0f          ", cpm);
    }
    if (page == 1)
    {
      My_LCD.setCursor(0, 0);
      My_LCD.printf("uSv/h: %.3f          ", cpm / c_uSv);
    }
    if (page == 2)
    {
      My_LCD.setCursor(0, 0);
      My_LCD.printf("Avg uSv/h: %.3f          ", float(counter * 60000) / (float(millis() - starttime) * c_uSv));
    }
    if (page == 3)
    {
      My_LCD.setCursor(0, 0);
      My_LCD.printf("C/%ds:%d(%d)          ", msec_interval / 1000, int(last_interval_cpm), counter - counter_interval);
    }
  }

  if (menu)
  {
    if (page_m == 0)
    {
      My_LCD.setCursor(0, 0);
      My_LCD.printf("Count/uSv:           ");
      if (edit)
      {
        if (let == 'U')
          c_uSv += STEP_CuSV;
        if (let == 'D')
          c_uSv -= STEP_CuSV;
      }
      My_LCD.setCursor(0, 1);
      My_LCD.printf("%3.1f             ", c_uSv);
    }
    if (page_m == 1)
    {
      My_LCD.setCursor(0, 0);
      My_LCD.printf("Time interval s:");
      My_LCD.setCursor(0, 1);
      My_LCD.printf("%d                   ",msec_interval);
      if (edit)
      {
        if (let == 'U')
          msec_interval += STEP_msec;
        if (let == 'D')
          msec_interval -= STEP_msec;
      }
    }
    if (edit)
    {
      My_LCD.setCursor(15, 0);
      My_LCD.print("*");
    }
  }

  if (!menu)
  {
    My_LCD.setCursor(0, 1);
    My_LCD.printf("Counts: %d          ", counter);
  }
  My_LCD.setCursor(15, 1);
  My_LCD.print(let);

  delay(200);
}