// Arduino.h จำลองบน PC: เวลา ขา และ Serial ถูกควบคุมโดย harness.cpp
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Print.h"

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

class __FlashStringHelper;
#define F(text) (reinterpret_cast<const __FlashStringHelper*>(text))

unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t level);
int digitalRead(uint8_t pin);
float temperatureRead();

class HostSerial : public Stream {
 public:
  void begin(unsigned long) {}
  size_t write(uint8_t byte) override;
  using Print::write;
  int available() override;
  int read() override;
};
extern HostSerial Serial;
