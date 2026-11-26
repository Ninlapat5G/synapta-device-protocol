// ตัวทดสอบบน PC: รันไลบรารี + examples/LedDemo จริง โดยจำลองเวลา ขา และ Serial
// run_vectors.py สั่งงานทีละบรรทัดทาง stdin แล้วอ่านผลทาง stdout (ดูรายการคำสั่งใน runCommand)
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "Arduino.h"

// ---------- โลกจำลอง ----------

static unsigned long g_now = 0;
static uint8_t g_pins[64];
static std::deque<uint8_t> g_rx;
static std::vector<uint8_t> g_tx;

unsigned long millis() { return g_now; }
unsigned long micros() { return g_now * 1000; }
void delay(unsigned long ms) { g_now += ms; }
void pinMode(uint8_t pin, uint8_t mode) {
  if (mode == INPUT_PULLUP) g_pins[pin] = HIGH;
}
void digitalWrite(uint8_t pin, uint8_t level) { g_pins[pin] = level; }
int digitalRead(uint8_t pin) { return g_pins[pin]; }
float temperatureRead() { return 23.5f; }

size_t HostSerial::write(uint8_t byte) {
  g_tx.push_back(byte);
  return 1;
}
int HostSerial::available() { return static_cast<int>(g_rx.size()); }
int HostSerial::read() {
  if (g_rx.empty()) return -1;
  const uint8_t b = g_rx.front();
  g_rx.pop_front();
  return b;
}
HostSerial Serial;

// device_id / boot_id ของอุปกรณ์ตัวอย่างใน spec/vectors
#include "../../SynaptaDevice/src/sdp/platform.h"
namespace sdplib {
bool loadDeviceId(uint8_t id[8]) {
  const uint8_t fixed[8] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18};
  memcpy(id, fixed, 8);
  return true;
}
void randomBytes(uint8_t* out, uint8_t len) {
  const uint8_t bootId[4] = {0x0b, 0xad, 0xf0, 0x0d};
  for (uint8_t i = 0; i < len; i++) out[i] = bootId[i % 4];
}
}  // namespace sdplib

// ---------- sketch ที่ทดสอบ ----------

#include "../../SynaptaDevice/examples/LedDemo/LedDemo.ino"

// capability ทดสอบที่ LedDemo ไม่มี: output ใหญ่, key ผิดลำดับ, schema ผิด
SDP_CAPABILITY(bigOutput, "test.big", "Array longer than 23 items and nested objects", SDP_NONE, SDP_NO_SCHEMA,
               SDP_NO_SCHEMA) {
  SdpOutput& out = call.output();
  out.beginArray("list");
  for (int i = 0; i < 30; i++) out.item(i);
  out.end();
  out.beginObject("nested").add("ok", true).add("neg", -5).add("ratio", 0.1).end();
}
SDP_CAPABILITY(misordered, "test.order", "Keys added out of canonical order", SDP_NONE, SDP_NO_SCHEMA,
               SDP_NO_SCHEMA) {
  call.output().add("long", 1).add("a", 2);
}
SDP_CAPABILITY(tooBig, "test.overflow", "Output larger than the buffer", SDP_NONE, SDP_NO_SCHEMA, SDP_NO_SCHEMA) {
  SdpOutput& out = call.output();
  out.beginArray("x");
  for (int i = 0; i < 600; i++) out.item(i);
  out.end();
}
SDP_CAPABILITY(failing, "test.fail", "Handler that reports failure", SDP_NONE, SDP_NO_SCHEMA, SDP_NO_SCHEMA) {
  call.fail(SDP_BUSY, "motor still moving");
}
SDP_CAPABILITY(ranged, "test.range", "Checks numbers, text and bytes", SDP_NONE,
               R"({"type":"object","properties":{
                    "duty":{"type":"integer","minimum":0,"maximum":255},
                    "label":{"type":"string","maxLength":4},
                    "blob":{"type":"string","contentEncoding":"base64","maxLength":8},
                    "list":{"type":"array","maxItems":2,"items":{"type":"number"}}},
                  "required":["duty"]})",
               SDP_NO_SCHEMA) {
  char label[8];
  const size_t len = call.input().getText("label", label, sizeof label);
  call.output().add("duty", call.input().getInt("duty")).add("label", len ? label : "");
}
const SdpCapability* const TEST_CAPS[] = {&ledSet, &tempRead, &bigOutput, &misordered, &tooBig, &failing, &ranged};

SDP_CAPABILITY(badJson, "bad.json", "Broken schema", SDP_NONE, R"({"type":"object",)", SDP_NO_SCHEMA) { (void)call; }
SDP_CAPABILITY(badKeyword, "bad.keyword", "Unsupported keyword", SDP_NONE,
               R"({"type":"object","properties":{"x":{"type":"integer","multipleOf":2}}})", SDP_NO_SCHEMA) { (void)call; }
SDP_CAPABILITY(badDepth, "bad.depth", "Too deep", SDP_NONE,
               R"({"type":"object","properties":{"a":{"type":"object","properties":{"b":{"type":"array","items":{"type":"integer"}}}}}})",
               SDP_NO_SCHEMA) { (void)call; }
SDP_CAPABILITY(badTop, "bad.top", "Input not an object", SDP_NONE, R"({"type":"integer"})", SDP_NO_SCHEMA) { (void)call; }
const SdpCapability* const BAD_CAPS[] = {&badJson, &badKeyword, &badDepth, &badTop};

// ---------- คำสั่ง ----------

static std::vector<uint8_t> fromHex(const std::string& hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}

static std::string toHex(const std::vector<uint8_t>& data) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (uint8_t b : data) {
    out += digits[b >> 4];
    out += digits[b & 15];
  }
  return out;
}

static SdpConfig demoConfig(uint16_t pageBytes) {
  SdpConfig c;
  c.model = "esp32-devkit";
  c.firmware = "led-demo 1.0.0";
  c.describePageBytes = pageBytes;
  return c;
}

static bool g_enrollmentOpen = true;
static bool g_requireCode = false;

static uint8_t provisionTest(const uint8_t*, const uint8_t* code) {
  if (!g_enrollmentOpen || (g_requireCode && (!code || memcmp(code, "01234567", 8) != 0))) return SDP_AUTH_REQUIRED;
  return 0;
}

// ถอดด้วยตัวอ่านของไลบรารีแล้วเขียนกลับแบบ canonical (เรียง key) — ทดสอบ CborRef กับทุกรูปที่ผู้ส่งอาจส่งมา
class VectorSink : public sdplib::Sink {
 public:
  void put(uint8_t b) override { bytes.push_back(b); }
  std::vector<uint8_t> bytes;
};

static std::vector<uint8_t> canonical(sdplib::CborRef v) {
  VectorSink s;
  sdplib::CborWriter w(s);
  switch (v.major()) {
    case 0:
    case 1:
      w.head(v.major(), v.arg());
      break;
    case 2:
    case 3:
      w.head(v.major(), v.arg());
      s.write(v.payload(), v.arg());
      break;
    case 4: {
      w.array(v.arg());
      sdplib::CborRef item = v.first();
      for (uint32_t i = 0; i < v.arg(); i++, item = item.next()) {
        const auto e = canonical(item);
        s.write(e.data(), e.size());
      }
      break;
    }
    case 5: {
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> pairs;
      sdplib::CborRef k = v.first();
      for (uint32_t i = 0; i < v.arg(); i++) {
        const sdplib::CborRef value = k.next();
        pairs.emplace_back(canonical(k), canonical(value));
        k = value.next();
      }
      std::sort(pairs.begin(), pairs.end());
      w.map(pairs.size());
      for (const auto& p : pairs) {
        s.write(p.first.data(), p.first.size());
        s.write(p.second.data(), p.second.size());
      }
      break;
    }
    default:
      if (v.isFloat()) {
        w.number(v.asDouble());
      } else {
        s.put(v.p[0]);
      }
  }
  return s.bytes;
}

static const char* rxName(sdplib::RxResult r) {
  switch (r) {
    case sdplib::RxResult::kTooLarge: return "too_large";
    case sdplib::RxResult::kCobsInvalid: return "cobs_invalid";
    case sdplib::RxResult::kTooShort: return "too_short";
    case sdplib::RxResult::kCrcMismatch: return "crc_mismatch";
    case sdplib::RxResult::kReservedFlag: return "reserved_flag";
    default: return "?";
  }
}

// T ms        ตั้งนาฬิกา
// R hex       ป้อน byte เข้า Serial แล้วเรียก loop() หนึ่งรอบ
// L           เรียก loop() หนึ่งรอบ
// P pin lvl   ตั้งระดับขา input        Q pin   อ่านระดับขา
// U           ยกเลิก onLinkLost
// PAGE n      begin ใหม่ด้วย describePageBytes = n
// TESTCAPS    begin ใหม่ด้วย capability ทดสอบ       BADCAPS  begin ด้วย schema ผิด (พิมพ์ผล begin)
// X max hex   ป้อน byte ให้ FrameReceiver เปล่า ๆ แล้วพิมพ์เหตุการณ์
// V hex       ถอดแล้วเขียนกลับแบบ canonical
// F hex       ส่ง inner (flags|body) ออกเป็นเฟรม serial ด้วย FrameSink
// K hex       พิมพ์ CRC-16 และ CRC-32 ของข้อมูล
// ทุกคำสั่งพิมพ์ "S <hex>" = byte ที่ส่งออก Serial ตามด้วย "." ปิดท้าย
static void runCommand(const std::string& cmd, const std::string& a, const std::string& b) {
  if (cmd == "T") {
    g_now = strtoul(a.c_str(), nullptr, 10);
  } else if (cmd == "R") {
    for (uint8_t byte : fromHex(a)) g_rx.push_back(byte);
    loop();
  } else if (cmd == "L") {
    loop();
  } else if (cmd == "P") {
    g_pins[atoi(a.c_str())] = static_cast<uint8_t>(atoi(b.c_str()));
  } else if (cmd == "Q") {
    printf("PIN %d\n", g_pins[atoi(a.c_str())]);
  } else if (cmd == "U") {
    sdp.onLinkLost(nullptr);
  } else if (cmd == "BOOTSTATE") {
    sdp.setBootState(static_cast<SdpBootState>(atoi(a.c_str())));
  } else if (cmd == "PAGE") {
    sdp.begin(Serial, demoConfig(static_cast<uint16_t>(atoi(a.c_str()))), CAPS, EVENTS);
  } else if (cmd == "BUILDIDS" || cmd == "PROVISION") {
    SdpConfig metadata = demoConfig(4096);
    metadata.projectId = "led-demo";
    metadata.buildId = "demo-build-a";
    if (cmd == "PROVISION") {
      g_enrollmentOpen = b != "closed";
      g_requireCode = a == "code";
      metadata.usbPairing = g_requireCode ? SdpUsbPairing::Code : SdpUsbPairing::Confirm;
      metadata.usbProvision = provisionTest;
    }
    sdp.begin(Serial, metadata, CAPS, EVENTS);
  } else if (cmd == "TESTCAPS") {
    printf("BEGIN %d\n", sdp.begin(Serial, demoConfig(4096), TEST_CAPS, EVENTS) ? 1 : 0);
  } else if (cmd == "BADCAPS") {
    printf("BEGIN %d\n", sdp.begin(Serial, demoConfig(4096), BAD_CAPS, EVENTS) ? 1 : 0);
  } else if (cmd == "X") {
    const uint16_t maxFrame = static_cast<uint16_t>(atoi(a.c_str()));
    std::vector<uint8_t> buf(maxFrame + 2u);
    sdplib::FrameReceiver rx(buf.data(), maxFrame);
    for (uint8_t byte : fromHex(b)) {
      const sdplib::RxResult r = rx.push(byte);
      if (r == sdplib::RxResult::kFrame) {
        printf("FRAME %s\n", toHex(std::vector<uint8_t>(rx.frame(), rx.frame() + rx.frameLen())).c_str());
      } else if (r != sdplib::RxResult::kNone) {
        printf("DROP %s\n", rxName(r));
      }
    }
    printf("PENDING %u\n", rx.pendingLen());
  } else if (cmd == "F") {
    static uint8_t block[sdplib::kCobsBlock];
    sdplib::FrameSink frame(block);
    frame.begin(Serial);
    for (uint8_t byte : fromHex(a)) frame.put(byte);
    frame.end();
  } else if (cmd == "K") {
    uint16_t crc16 = 0xFFFF;
    uint32_t crc32 = 0xFFFFFFFFUL;
    for (uint8_t byte : fromHex(a)) {
      crc16 = sdplib::crc16Update(crc16, byte);
      crc32 = sdplib::crc32Update(crc32, byte);
    }
    printf("CRC %04x %08lx\n", crc16, static_cast<unsigned long>(~crc32));
  } else if (cmd == "V") {
    const std::vector<uint8_t> body = fromHex(a);
    const uint8_t* next;
    if (body.empty() || !sdplib::wellFormed(body.data(), body.data() + body.size(), &next) || next != body.data() + body.size()) {
      printf("INVALID\n");
    } else {
      printf("CANON %s\n", toHex(canonical(sdplib::CborRef(body.data()))).c_str());
    }
  } else {
    printf("ERR unknown command %s\n", cmd.c_str());
  }
}

int main() {
  setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
  setup();
  printf("S %s\n.\n", toHex(g_tx).c_str());  // ข้อความตอนบูต (ควรว่าง)
  fflush(stdout);
  g_tx.clear();

  char line[1 << 16];
  while (fgets(line, sizeof line, stdin)) {
    char cmd[16] = {0};
    static char a[1 << 15], b[1 << 15];
    a[0] = b[0] = 0;
    if (sscanf(line, "%15s %32767s %32767s", cmd, a, b) < 1) continue;
    runCommand(cmd, a, b);
    printf("S %s\n.\n", toHex(g_tx).c_str());
    fflush(stdout);
    g_tx.clear();
  }
  return 0;
}
