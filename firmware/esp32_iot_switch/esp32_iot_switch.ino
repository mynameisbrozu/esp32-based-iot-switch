#include <Arduino.h>

#define TOUCH_BUTTON 9
#define WIFI_LED 8
#define SW_LEDB 11
#define SW_LEDR 12

bool switchON = false;

void ARDUINO_ISR_ATTR touch() {
  if(switchON){
    digitalWrite(SW_LEDB, LOW);
    digitalWrite(SW_LEDR, HIGH);
    switchON = false;
  } else{
    digitalWrite(SW_LEDR, LOW);
    digitalWrite(SW_LEDB, HIGH);
    switchON = true;
  }
}

void setup() {
  pinMode(TOUCH_BUTTON, INPUT);
  pinMode(WIFI_LED, OUTPUT);
  pinMode(SW_LEDB, OUTPUT);
  pinMode(SW_LEDR, OUTPUT);
  digitalWrite(SW_LEDB, LOW);
  digitalWrite(SW_LEDR, HIGH);
  attachInterrupt(TOUCH_BUTTON, touch, RISING);
}

void loop() {
  // put your main code here, to run repeatedly:
  digitalWrite(WIFI_LED,HIGH);
  delay(2000);
  digitalWrite(WIFI_LED,LOW);
  delay(2000);
}
