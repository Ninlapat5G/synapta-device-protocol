// LedDemo — อุปกรณ์ตัวอย่างของ SDP: สั่ง LED, อ่านอุณหภูมิ, แจ้งเมื่อกดปุ่ม
// บน ESP32 ตอบ byte ตรงกับอุปกรณ์ตัวอย่างใน spec/vectors ทุกข้อความ (ใช้เป็นตัวทดสอบของไลบรารีด้วย)
#include <SynaptaDevice.h>

#if defined(ARDUINO_ARCH_AVR)
const uint8_t LED_PIN = 13;    // LED บนบอร์ด Uno
const uint8_t BUTTON_PIN = 2;  // ปุ่มต่อระหว่างขา 2 กับ GND
#define BOARD_MODEL "uno"
#define BUTTON_TEXT "Button on pin 2 pressed"
#else
const uint8_t LED_PIN = 2;     // LED บน ESP32 DevKit
const uint8_t BUTTON_PIN = 0;  // ปุ่ม BOOT
#define BOARD_MODEL "esp32-devkit"
#define BUTTON_TEXT "BOOT button pressed"
#endif

SDP_CAPABILITY(ledSet, "led.set", "Turn the on-board LED on or off", SDP_ACTUATOR,
               R"({"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]})",
               SDP_NO_SCHEMA) {
  const bool on = call.input().getBool("on");
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  call.output().add("on", on);
}

// อุณหภูมิในชิป — Uno ใช้เซนเซอร์ใน ATmega328P (คลาดได้ราว ±10 °C ถ้าไม่ปรับเทียบ)
float readCelsius() {
#if defined(ARDUINO_ARCH_AVR)
  ADMUX = _BV(REFS1) | _BV(REFS0) | _BV(MUX3);  // อ้างอิง 1.1 V, ช่อง 8 = เซนเซอร์อุณหภูมิ
  delay(2);
  ADCSRA |= _BV(ADSC);
  while (bit_is_set(ADCSRA, ADSC)) {
  }
  return (ADCW - 324.31f) / 1.22f;
#else
  return temperatureRead();
#endif
}

SDP_CAPABILITY(tempRead, "temp.read", "Read board temperature in Celsius", SDP_NONE, SDP_NO_SCHEMA,
               R"({"type":"object","properties":{"celsius":{"type":"number"}}})") {
  call.output().add("celsius", readCelsius());
}

SDP_EVENT(buttonPressed, "button.pressed", BUTTON_TEXT,
          R"({"type":"object","properties":{"count":{"type":"integer","minimum":0}}})");

const SdpCapability* const CAPS[] = {&ledSet, &tempRead};
const SdpEvent* const EVENTS[] = {&buttonPressed};

SynaptaDevice sdp;

// PC หลุดหรือโปรแกรมฝั่ง PC ปิด → ปิดทุกอย่างที่ agent อาจสั่งค้างไว้
void allOff() { digitalWrite(LED_PIN, LOW); }

void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);

  SdpConfig config;
  config.model = BOARD_MODEL;
  config.firmware = "led-demo 1.0.0";
  sdp.onLinkLost(allOff);
  sdp.begin(Serial, config, CAPS, EVENTS);
}

// นับการกดแบบกันเด้ง: สถานะต้องนิ่ง 30 ms ก่อนนับ
void checkButton() {
  static bool lastStable = HIGH;
  static bool lastRead = HIGH;
  static unsigned long changedAt = 0;
  static unsigned long presses = 0;

  const bool now = digitalRead(BUTTON_PIN);
  if (now != lastRead) {
    lastRead = now;
    changedAt = millis();
  }
  if (now != lastStable && millis() - changedAt >= 30) {
    lastStable = now;
    if (now == LOW) {
      presses++;
      sdp.beginEvent(buttonPressed).add("count", presses);
      sdp.sendEvent();
    }
  }
}

void loop() {
  sdp.loop();
  checkButton();
}
