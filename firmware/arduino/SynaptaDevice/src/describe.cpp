// descriptor ของ capability/event: เขียนจาก flash ลงสายตรง ๆ, แบ่งหน้า describe, และตรวจ schema ตอน begin()
#include "SynaptaDevice.h"
#include "sdp/json.h"

using sdplib::CborRef;
using sdplib::CborWriter;

namespace {

// หัวของ describe_r ยาวสุด: flags+addr 2, [4, id, {0:[..], 1:[..], 2:page}] ≤ 18
constexpr size_t kPageOverhead = 24;

const char kFieldName[] SDP_FLASH = "name";
const char kFieldInput[] SDP_FLASH = "input_schema";
const char kFieldOutput[] SDP_FLASH = "output_schema";
const char kFieldData[] SDP_FLASH = "data_schema";
const char kFieldSize[] SDP_FLASH = "descriptor";
const char kEmptyName[] SDP_FLASH = "name must not be empty";
const char kDuplicateName[] SDP_FLASH = "name used twice";
const char kTooLarge[] SDP_FLASH = "larger than one describe page - shorten descriptions or schema";
const char kNoLinkLost[] SDP_FLASH = "actuator capability but onLinkLost() was not set before begin() - calls will be refused";
const char kLogPrefix[] SDP_FLASH = "SDP: ";
const char kAtChar[] SDP_FLASH = " (at char ";

bool sameFlashText(const char* a, const char* b) {
  for (size_t i = 0;; i++) {
    const uint8_t ca = sdplib::flashByte(a + i);
    if (ca != sdplib::flashByte(b + i)) return false;
    if (ca == 0) return true;
  }
}

const char* nameOf(const SdpCapability* const* caps, size_t capCount, const SdpEvent* const* events, size_t index) {
  return index < capCount ? sdplib::loadCapability(caps[index]).name : sdplib::loadEvent(events[index - capCount]).name;
}

}  // namespace

void SynaptaDevice::writeDescriptor(sdplib::Sink& sink, size_t index) const {
  CborWriter w(sink);
  if (index < capCount_) {
    const SdpCapability c = sdplib::loadCapability(caps_[index]);
    const bool hasInput = sdplib::hasText(c.inputSchema);
    const bool hasOutput = sdplib::hasText(c.outputSchema);
    const bool hasDanger = c.danger != SdpDanger::None;  // ไม่มี = 0 ตามสเปก จึงไม่ส่ง
    w.map(2u + hasInput + hasOutput + hasDanger);
    w.uint(0);
    w.flashText(c.name);
    w.uint(1);
    w.flashText(c.description);
    if (hasInput) {
      w.uint(2);
      sdplib::json::emit(c.inputSchema, w);
    }
    if (hasOutput) {
      w.uint(3);
      sdplib::json::emit(c.outputSchema, w);
    }
    if (hasDanger) {
      w.uint(4);
      w.uint(static_cast<uint8_t>(c.danger));
    }
    return;
  }
  const SdpEvent e = sdplib::loadEvent(events_[index - capCount_]);
  const bool hasData = sdplib::hasText(e.dataSchema);
  w.map(2u + hasData);
  w.uint(0);
  w.flashText(e.name);
  w.uint(1);
  w.flashText(e.description);
  if (hasData) {
    w.uint(2);
    sdplib::json::emit(e.dataSchema, w);
  }
}

size_t SynaptaDevice::descriptorSize(size_t index) const {
  sdplib::CountSink count;
  writeDescriptor(count, index);
  return count.count;
}

// หน้า describe: ใส่ descriptor ตามลำดับ (caps ก่อน events) จนเต็ม — ตัวแรกของหน้าใส่เสมอแม้ใหญ่เกิน
size_t SynaptaDevice::pageEnd(size_t start, size_t budget) const {
  const size_t total = capCount_ + eventCount_;
  size_t used = 0;
  size_t i = start;
  for (; i < total; i++) {
    const size_t size = descriptorSize(i);
    if (i > start && used + size > budget) break;
    used += size;
  }
  return i;
}

void SynaptaDevice::handleDescribe(uint16_t id, CborRef m) {
  const CborRef pageRef = m.mapGetUint(0);
  if (pageRef.valid() && (pageRef.major() != sdplib::kUint || pageRef.arg() > 0xFFFF)) {
    return sendError(id, SDP_INVALID_INPUT);
  }
  const uint16_t page = pageRef.valid() ? static_cast<uint16_t>(pageRef.arg()) : 0;
  const size_t total = capCount_ + eventCount_;
  const size_t budget = config_.describePageBytes > kPageOverhead ? config_.describePageBytes - kPageOverhead : 1;

  size_t start = 0;
  for (uint16_t p = 0; p < page; p++) {
    if (start >= total) return sendError(id, SDP_INVALID_INPUT);  // ขอหน้าที่ไม่มี
    start = pageEnd(start, budget);
  }
  if (page > 0 && start >= total) return sendError(id, SDP_INVALID_INPUT);
  const size_t end = pageEnd(start, budget);

  const size_t capEnd = end < capCount_ ? end : capCount_;
  const size_t capsOnPage = start < capEnd ? capEnd - start : 0;
  const size_t eventsOnPage = end - start - capsOnPage;
  const bool hasNext = end < total;

  CborWriter w = startReply(4, id, (capsOnPage > 0) + (eventsOnPage > 0) + hasNext);
  if (capsOnPage) {
    w.uint(0);
    w.array(capsOnPage);
    for (size_t i = start; i < start + capsOnPage; i++) writeDescriptor(frame_, i);
  }
  if (eventsOnPage) {
    w.uint(1);
    w.array(eventsOnPage);
    for (size_t i = start + capsOnPage; i < end; i++) writeDescriptor(frame_, i);
  }
  if (hasNext) {
    w.uint(2);
    w.uint(page + 1u);
  }
  finishReply();
}

void SynaptaDevice::logProblem(size_t index, const char* field, const char* schema, const sdplib::Problem& problem) {
  if (!port_) return;
  logFlash(kLogPrefix);
  logFlash(nameOf(caps_, capCount_, events_, index));
  port_->write(' ');
  logFlash(field);
  port_->write(':');
  port_->write(' ');
  logFlash(problem.message);
  if (schema && problem.where) {
    logFlash(kAtChar);
    port_->print(static_cast<unsigned long>(problem.where - schema));
    port_->write(')');
  }
  port_->write('\n');
}

bool SynaptaDevice::lintDescriptors() {
  bool ok = true;
  const size_t total = capCount_ + eventCount_;
  const size_t budget = config_.describePageBytes > kPageOverhead ? config_.describePageBytes - kPageOverhead : 1;
  for (size_t i = 0; i < total; i++) {
    sdplib::Problem problem;
    const char* name = nameOf(caps_, capCount_, events_, i);
    if (!sdplib::hasText(name)) {
      problem.message = kEmptyName;
      logProblem(i, kFieldName, nullptr, problem);
      ok = false;
      continue;  // ข้ามตัวที่ไม่มีชื่อ — ตรวจต่อไม่ได้ความหมาย
    }
    for (size_t j = 0; j < i; j++) {
      if ((j < capCount_) == (i < capCount_) && sameFlashText(name, nameOf(caps_, capCount_, events_, j))) {
        problem.message = kDuplicateName;
        logProblem(i, kFieldName, nullptr, problem);
        ok = false;
      }
    }

    bool schemasOk = true;
    if (i < capCount_) {
      const SdpCapability c = sdplib::loadCapability(caps_[i]);
      if (sdplib::hasText(c.inputSchema) && !sdplib::lintSchema(c.inputSchema, true, &problem)) {
        logProblem(i, kFieldInput, c.inputSchema, problem);
        schemasOk = false;
      }
      if (sdplib::hasText(c.outputSchema) && !sdplib::lintSchema(c.outputSchema, false, &problem)) {
        logProblem(i, kFieldOutput, c.outputSchema, problem);
        schemasOk = false;
      }
      if (c.danger != SdpDanger::None && !linkLost_) {
        problem.message = kNoLinkLost;
        logProblem(i, kFieldName, nullptr, problem);  // เตือนอย่างเดียว — call จะถูกปฏิเสธเอง
      }
    } else {
      const SdpEvent e = sdplib::loadEvent(events_[i - capCount_]);
      if (sdplib::hasText(e.dataSchema) && !sdplib::lintSchema(e.dataSchema, false, &problem)) {
        logProblem(i, kFieldData, e.dataSchema, problem);
        schemasOk = false;
      }
    }
    // วัดขนาดได้เฉพาะตัวที่ schema ถูก — JSON ผิดรูปเขียนเป็น CBOR ไม่ได้
    if (schemasOk && descriptorSize(i) > budget) {
      problem.message = kTooLarge;
      logProblem(i, kFieldSize, nullptr, problem);
      schemasOk = false;
    }
    ok = ok && schemasOk;
  }
  return ok;
}
