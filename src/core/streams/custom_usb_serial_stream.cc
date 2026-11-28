#include "custom_usb_serial_stream.h"

#include <QRandomGenerator>
#include <QSerialPort>
#include <QThread>
#include <queue>
#include <stdexcept>
#include <tuple>

#include "custom_usb_serial_protocol.h"

CustomUsbSerialStream::CustomUsbSerialStream(QObject* parent, const QString& port, qint32 baud_rate)
    : LiveStream(parent, false), port_(port), baud_rate_(baud_rate) {
  if (port.isEmpty()) throw std::runtime_error("Choose a USB serial port");
}

void CustomUsbSerialStream::setError(const QString& error) {
  std::lock_guard lock(stats_mutex_);
  error_ = error;
  utilization_expiry_ = QDeadlineTimer(0);
  for (auto& latency : tx_latency_) latency.reset();
  tx_capability_ = QDeadlineTimer(0);
  tx_request_.clear();
  tx_status_ = tx_in_flight_ ? tr("USB connection lost. Transmission outcome unknown.") : error;
  tx_state_ = TxState::Error;
  tx_in_flight_ = 0;
  clearRepeat();
}

QString CustomUsbSerialStream::captureStatus(int metrics) const {
  std::lock_guard lock(stats_mutex_);
  if (!error_.isEmpty()) return QStringLiteral("Custom USB Serial: %1").arg(error_);
  if (!last_record_ms_) return tr("Custom USB Serial: waiting for capture session reply");
  if (QDateTime::currentMSecsSinceEpoch() - last_record_ms_ > 3000)
    return QStringLiteral("Custom USB Serial: no USB data for 3 seconds");
  const auto frame_counts = QStringLiteral("USB 0:%1 1:%2").arg(frames_[0]).arg(frames_[1]);
  QStringList parts;
  auto count = [&](StatusMetric flag, const QString& label, uint64_t value) {
    if (metrics & flag) parts += QStringLiteral("%1:%2").arg(label).arg(value);
  };
  auto utilization = [&](StatusMetric flag, const QString& label, uint16_t value) {
    if (!(metrics & flag)) return;
    const QString percent = utilization_expiry_.hasExpired() ? QStringLiteral("—") :
        QStringLiteral("%1%").arg(value / 10.0, 0, 'f', 1);
    parts += QStringLiteral("%1:%2").arg(label, percent);
  };
  utilization(SerialUtil, tr("Serial"), utilization_.serial);
  utilization(Cpu0Util, tr("CPU0"), utilization_.cpu[0]);
  utilization(Cpu1Util, tr("CPU1"), utilization_.cpu[1]);
  utilization(SpiUtil, tr("SPI"), utilization_.spi);
  utilization(Bus0Util, tr("CAN0"), utilization_.can[0]);
  utilization(Bus1Util, tr("CAN1"), utilization_.can[1]);
  count(SequenceGaps, tr("USB seq gaps"), gaps_[0] + gaps_[1]);
  count(QueueDrops, tr("Capture drops"), queue_drops_[0] + queue_drops_[1]);
  count(FifoA, tr("FIFO A"), overflows_[0]);
  count(FifoB, tr("FIFO B"), overflows_[1]);
  count(InvalidRecords, tr("Invalid USB"), corrupt_);
  return parts.isEmpty() ? frame_counts : frame_counts + QStringLiteral(" | ") + parts.join(QStringLiteral("  "));
}

CustomUsbSerialStream::CaptureCounters CustomUsbSerialStream::captureCounters() const {
  std::lock_guard lock(stats_mutex_);
  return {gaps_[0] + gaps_[1], queue_drops_[0] + queue_drops_[1], {overflows_[0], overflows_[1]}, corrupt_};
}

QString CustomUsbSerialStream::captureDetails() const {
  std::lock_guard lock(stats_mutex_);
  return QStringLiteral(
             "Bus 0: %1 frames, %2 sequence gaps, %3 queue drops, %4 FIFO overflow events\n"
             "Bus 1: %5 frames, %6 sequence gaps, %7 queue drops, %8 FIFO overflow events\n"
             "USB aborted writes: %9; invalid records: %10; device restarts: %11\n"
             "FIFO events mean at least one lost frame each. Queue, USB and sequence loss overlap.\n"
             "Late CAN timestamps: %12")
      .arg(frames_[0])
      .arg(gaps_[0])
      .arg(queue_drops_[0])
      .arg(overflows_[0])
      .arg(frames_[1])
      .arg(gaps_[1])
      .arg(queue_drops_[1])
      .arg(overflows_[1])
      .arg(usb_aborts_)
      .arg(corrupt_)
      .arg(resets_)
      .arg(late_timestamps_);
}

std::optional<CustomUsbSerialStream::Utilization> CustomUsbSerialStream::utilization() const {
  std::lock_guard lock(stats_mutex_);
  if (utilization_expiry_.hasExpired()) return std::nullopt;
  return utilization_;
}

std::optional<CustomUsbSerialStream::TxLatency> CustomUsbSerialStream::txLatency(uint8_t bus) const {
  std::lock_guard lock(stats_mutex_);
  if (bus >= 2) return std::nullopt;
  return tx_latency_[bus];
}

std::optional<CustomUsbSerialStream::CapturedFrame> CustomUsbSerialStream::latestDataFrame(
    const MessageId& id, const dbc::Signal* signal) const {
  std::lock_guard lock(stats_mutex_);
  const auto it = recent_data_frames_.find(id);
  if (it == recent_data_frames_.end()) return std::nullopt;
  for (auto frame = it->second.rbegin(); frame != it->second.rend(); ++frame) {
    if (!signal || signal->isPresent(reinterpret_cast<const uint8_t*>(frame->data.constData()), frame->data.size())) {
      return *frame;
    }
  }
  return std::nullopt;
}

bool CustomUsbSerialStream::txReady() const {
  std::lock_guard lock(stats_mutex_);
  return !repeat_active_ && !tx_capability_.hasExpired() &&
         (!tx_in_flight_ || tx_deadline_.hasExpired());
}

bool CustomUsbSerialStream::repeatSupported() const {
  std::lock_guard lock(stats_mutex_);
  return !tx_capability_.hasExpired();
}

int CustomUsbSerialStream::fixedRateLimit() const {
  return transmitFeedbackSupported() ? DK_TX_MAX_HZ : DK_TX_LEGACY_MAX_HZ;
}

bool CustomUsbSerialStream::transmitFeedbackSupported() const {
  std::lock_guard lock(stats_mutex_);
  return !tx_capability_.hasExpired();
}

bool CustomUsbSerialStream::automaticSupported() const {
  std::lock_guard lock(stats_mutex_);
  return !tx_capability_.hasExpired();
}

bool CustomUsbSerialStream::counterSupported() const {
  std::lock_guard lock(stats_mutex_);
  return !tx_capability_.hasExpired();
}

bool CustomUsbSerialStream::repeatPending() const {
  std::lock_guard lock(stats_mutex_);
  return repeat_active_;
}

void CustomUsbSerialStream::clearRepeat() {
  repeat_active_ = repeat_cancelled_ = repeat_automatic_ = false;
}

QByteArray CustomUsbSerialStream::repeatControl(uint8_t opcode) const {
  uint8_t raw[DK_MAX_RAW] = {DK_VERSION, DK_TX_REPEAT_CONTROL, repeat_bus_};
  dk_put(raw + 4, tx_boot_id_, 4);
  dk_put(raw + 8, tx_in_flight_, 4);
  raw[20] = opcode;
  uint8_t wire[DK_MAX_WIRE];
  const size_t size = dk_encode(raw, 21, wire);
  return QByteArray(1, '\0') + QByteArray(reinterpret_cast<const char*>(wire), size);
}

void CustomUsbSerialStream::cancelRepeat() {
  std::lock_guard lock(stats_mutex_);
  if (!repeat_active_ || repeat_cancelled_) return;
  if (!tx_request_.isEmpty()) {
    const bool automatic = repeat_automatic_;
    tx_request_.clear();
    tx_in_flight_ = 0;
    clearRepeat();
    tx_state_ = TxState::Ready;
    tx_status_ = automatic ? tr("Automatic send cancelled.") : tr("Fixed-rate send cancelled.");
  } else {
    repeat_cancelled_ = true;
    repeat_deadline_ = QDeadlineTimer(1500);
    repeat_keepalive_ = QDeadlineTimer(0);
    tx_status_ = repeat_automatic_ ? tr("Cancelling automatic send…") : tr("Cancelling fixed-rate send…");
  }
}

QString CustomUsbSerialStream::txStatus(TxState* state, bool include_result,
                                        std::optional<TxErrorDetails>* error_details) const {
  std::lock_guard lock(stats_mutex_);
  auto result = [state](TxState current, const QString& message) {
    if (state) *state = current;
    return message;
  };
  if (!error_.isEmpty()) return result(TxState::Error, include_result ? tx_status_ : error_);
  if (!include_result) {
    if (repeat_active_ || (tx_in_flight_ && !tx_deadline_.hasExpired()))
      return result(TxState::Waiting, tr("Waiting for the previous send to finish."));
    if (tx_capability_.hasExpired())
      return result(TxState::Waiting, tr("Waiting for USB transmission support."));
    return result(TxState::Ready, {});
  }
  if (!repeat_active_ && tx_in_flight_ && tx_deadline_.hasExpired())
    return result(TxState::Error, tr("Response timed out. Transmission outcome unknown."));
  if (tx_capability_.hasExpired() && !tx_in_flight_)
    return result(TxState::Waiting, tr("Waiting for USB transmission support."));
  if (error_details && tx_state_ == TxState::Error) *error_details = tx_error_details_;
  return result(tx_state_, tx_status_);
}

bool CustomUsbSerialStream::sendFrame(uint8_t bus, uint32_t address, uint8_t flags, const QByteArray& data,
                                     std::optional<uint32_t> expected_boot, std::optional<RepeatTiming> repeat,
                                     QByteArray signal_mask, QByteArray selector_mask,
                                     std::optional<Counter> counter, std::optional<Checksum> checksum) {
  if (data.size() > 64) return false;
  if (repeat && signal_mask.size() != data.size()) return false;
  if (!selector_mask.isEmpty() && (!repeat || selector_mask.size() != data.size())) return false;
  std::lock_guard lock(stats_mutex_);
  if (expected_boot && *expected_boot != tx_boot_id_) return false;
  if (repeat_active_ || tx_capability_.hasExpired() ||
      (tx_in_flight_ && !tx_deadline_.hasExpired())) return false;
  if (++tx_sequence_ == 0) ++tx_sequence_;
  const uint8_t type = !repeat ? DK_TX_REQUEST : (repeat->automatic ? DK_TX_AUTO : DK_TX_REPEAT);
  uint8_t raw[DK_MAX_RAW] = {DK_VERSION, type, bus,
                             static_cast<uint8_t>(flags | (counter ? DK_TX_INCREMENT_COUNTER : 0) |
                                                  (checksum ? DK_TX_RECALCULATE_CHECKSUM : 0))};
  dk_put(raw + 4, tx_boot_id_, 4);
  dk_put(raw + 8, tx_sequence_, 4);
  if (repeat) {
    dk_put(raw + 12, repeat->frequency_hz, 4);
    dk_put(raw + 16, repeat->duration_ms, 4);
  }
  dk_put(raw + 20, address, 4);
  raw[24] = data.size();
  memcpy(raw + 25, data.constData(), data.size());
  if (repeat) memcpy(raw + 25 + data.size(), signal_mask.constData(), signal_mask.size());
  if (!selector_mask.isEmpty()) memcpy(raw + 25 + 2 * data.size(), selector_mask.constData(), selector_mask.size());
  const int descriptor = 25 + data.size() * (repeat ? 2 : 1) + selector_mask.size();
  if (counter) {
    dk_put(raw + descriptor, counter->start_bit, 2);
    raw[descriptor + 2] = counter->size | (counter->little_endian ? 0x80 : 0);
  }
  if (checksum) raw[descriptor + (counter ? 3 : 0)] = checksum->byte_index;
  uint8_t wire[DK_MAX_WIRE], decoded[DK_MAX_RAW];
  size_t size = dk_encode(raw, descriptor + (counter ? 3 : 0) + (checksum ? 1 : 0), wire);
  if (!size || !dk_decode(wire, size - 1, decoded)) return false;
  tx_request_ = QByteArray(1, '\0') + QByteArray(reinterpret_cast<const char*>(wire), size);
  tx_in_flight_ = tx_sequence_;
  tx_bus_ = bus;
  tx_acknowledged_ = 0;
  tx_feedback_error_.clear();
  tx_error_details_.reset();
  tx_deadline_ = QDeadlineTimer(1500);
  if (repeat) {
    repeat_active_ = true;
    repeat_automatic_ = repeat->automatic;
    if (repeat_automatic_) tx_latency_[bus].reset();
    repeat_bus_ = bus;
    repeat_duration_ms_ = repeat->duration_ms;
    repeat_deadline_ = tx_deadline_;
    repeat_keepalive_ = QDeadlineTimer(300);
  }
  tx_state_ = TxState::Waiting;
  tx_status_ = tr("Waiting for the USB adapter to respond.");
  return true;
}

QString CustomUsbSerialStream::firmwareLogSince(uint64_t& cursor) const {
  std::lock_guard lock(stats_mutex_);
  const uint64_t first = firmware_log_count_ - firmware_log_.size();
  QString text;
  if (cursor < first && cursor != 0) text = tr("[Older firmware logs were discarded.]\n");
  for (uint64_t index = std::max(cursor, first); index < firmware_log_count_; ++index) {
    text += firmware_log_[index - first];
  }
  cursor = firmware_log_count_;
  return text;
}

void CustomUsbSerialStream::streamThread() {
  QSerialPort serial;
  serial.setPortName(port_);
  serial.setBaudRate(baud_rate_);  // Use a high host line setting for the native USB device.
  if (!serial.open(QIODevice::ReadWrite)) {
    setError(serial.errorString());
    return;
  }
  {
    std::lock_guard lock(stats_mutex_);
    tx_sequence_ = QRandomGenerator::global()->generate();
  }
  auto newSessionId = [] {
    uint64_t id;
    do {
      id = QRandomGenerator::global()->generate64();
    } while (!id);
    return id;
  };
  uint64_t session_id = newSessionId();
  bool session_ready = false;
  QDeadlineTimer session_control_due(0), session_activity(0);
  auto sessionControl = [&](uint8_t opcode) {
    uint8_t raw[DK_MAX_RAW] = {DK_VERSION, DK_SESSION};
    dk_put(raw + 12, session_id, 8);
    raw[20] = opcode;
    uint8_t wire[DK_MAX_WIRE];
    const size_t size = dk_encode(raw, 21, wire);
    return QByteArray(1, '\0') + QByteArray(reinterpret_cast<const char*>(wire), size);
  };
  QByteArray pending;
  bool discarding = false, have_boot = false;
  uint32_t boot_id = 0, last_sequence[2] = {};
  bool have_sequence[2] = {};
  constexpr int reorder_ms = 10;
  struct PendingFrame {
    uint64_t timestamp;
    uint64_t order;
    QDeadlineTimer deadline;
    uint8_t source;
    uint32_t address;
    QByteArray data;

    bool operator>(const PendingFrame& other) const {
      return std::tie(timestamp, order) > std::tie(other.timestamp, other.order);
    }
  };
  std::priority_queue<PendingFrame, std::vector<PendingFrame>, std::greater<PendingFrame>> reorder;
  uint64_t last_source_ns = 0, newest_ns = 0, time_offset = 0, order = 0;
  auto publishReady = [&](bool flush = false) {
    uint64_t late = 0;
    while (!reorder.empty()) {
      const auto& frame = reorder.top();
      if (!flush && !frame.deadline.hasExpired()) break;
      late += frame.timestamp < last_source_ns;
      last_source_ns = std::max(last_source_ns, frame.timestamp);
      MessageBuilder message;
      auto event = message.initEvent();
      event.setLogMonoTime(frame.timestamp);
      auto can = event.initCan(1);
      can[0].setSrc(frame.source);
      can[0].setAddress(frame.address);
      can[0].setDat(kj::arrayPtr(reinterpret_cast<const uint8_t*>(frame.data.constData()), frame.data.size()));
      handleEvent(capnp::messageToFlatArray(message));
      reorder.pop();
    }
    if (late) {
      std::lock_guard lock(stats_mutex_);
      late_timestamps_ += late;
    }
  };
  uint64_t queue_base[2] = {}, overflow_base[2] = {}, usb_base = 0;
  while (!QThread::currentThread()->isInterruptionRequested()) {
    if (session_ready && session_activity.hasExpired()) {
      session_ready = false;
      session_id = newSessionId();
      session_control_due = QDeadlineTimer(0);
      std::lock_guard lock(stats_mutex_);
      tx_capability_ = QDeadlineTimer(0);
      utilization_expiry_ = QDeadlineTimer(0);
      for (auto& latency : tx_latency_) latency.reset();
      recent_data_frames_.clear();
      clearRepeat();
      tx_request_.clear();
      tx_in_flight_ = 0;
      tx_state_ = TxState::Waiting;
      tx_status_.clear();
    }
    QByteArray request;
    if (session_control_due.hasExpired()) {
      request = sessionControl(session_ready ? DK_SESSION_KEEPALIVE : DK_SESSION_START);
      session_control_due = QDeadlineTimer(session_ready ? 1000 : 500);
    }
    if (session_ready && request.isEmpty()) {
      std::lock_guard lock(stats_mutex_);
      if (repeat_active_) {
        if (repeat_deadline_.hasExpired() || tx_capability_.hasExpired()) {
          const bool automatic = repeat_automatic_;
          clearRepeat();
          tx_request_.clear();
          tx_in_flight_ = 0;
          tx_state_ = TxState::Error;
          tx_status_ = automatic ? tr("Automatic send response timed out. Transmission outcome unknown.")
                                 : tr("Fixed-rate send response timed out. Transmission outcome unknown.");
        } else if (!tx_request_.isEmpty()) {
          request.swap(tx_request_);
        } else if (repeat_keepalive_.hasExpired()) {
          request = repeatControl(repeat_cancelled_ ? DK_TX_CANCEL : DK_TX_KEEPALIVE);
          repeat_keepalive_ = QDeadlineTimer(300);
        }
      } else if (!tx_deadline_.hasExpired()) {
        request.swap(tx_request_);
      } else {
        tx_request_.clear();
      }
    }
    if (!request.isEmpty()) {
      if (serial.write(request) != request.size()) {
        setError(serial.errorString());
        break;
      }
      QDeadlineTimer write_deadline(100);
      while (serial.bytesToWrite() && !write_deadline.hasExpired()) {
        if (!serial.waitForBytesWritten(write_deadline.remainingTime())) break;
      }
      if (serial.bytesToWrite()) {
        setError(tr("USB transmit timed out"));
        break;
      }
    }
    publishReady();
    const int read_wait = reorder.empty() ? 100 : std::max(1, int(reorder.top().deadline.remainingTime()));
    if (!serial.bytesAvailable() && !serial.waitForReadyRead(read_wait)) {
      if (serial.error() != QSerialPort::NoError && serial.error() != QSerialPort::TimeoutError) {
        setError(serial.errorString());
        break;
      }
      continue;
    }
    const QByteArray bytes = serial.read(65536);
    for (char byte : bytes) {
      if (byte != 0) {
        if (!discarding) {
          pending.append(byte);
          if (pending.size() >= DK_MAX_WIRE) {
            pending.clear();
            discarding = true;
          }
        }
        continue;
      }
      uint8_t raw[DK_MAX_RAW];
      size_t n = discarding ? 0 : dk_decode(reinterpret_cast<const uint8_t*>(pending.constData()), pending.size(), raw);
      bool nonempty = discarding || !pending.isEmpty();
      pending.clear();
      discarding = false;
      if (n && raw[1] == DK_SESSION) {
        if (!session_ready && raw[20] == DK_SESSION_READY && dk_get(raw + 12, 8) == session_id) {
          session_ready = true;
          session_activity = QDeadlineTimer(2500);
          session_control_due = QDeadlineTimer(1000);
          have_sequence[0] = have_sequence[1] = false;
          std::lock_guard lock(stats_mutex_);
          last_record_ms_ = QDateTime::currentMSecsSinceEpoch();
        }
        continue;
      }
      if (!session_ready) continue;
      if (!n) {
        if (nonempty) {
          std::lock_guard lock(stats_mutex_);
          corrupt_++;
        }
        continue;
      }
      session_activity = QDeadlineTimer(2500);
      const uint8_t bus = raw[2];
      const bool tx_frame = raw[1] == DK_TX_FRAME;
      const uint32_t boot = dk_get(raw + 4, 4), sequence = dk_get(raw + 8, 4);
      const uint64_t device_ns = dk_get(raw + 12, 8) * 1000;
      {
        std::lock_guard lock(stats_mutex_);
        last_record_ms_ = QDateTime::currentMSecsSinceEpoch();
        if (raw[1] == DK_TX_REQUEST || raw[1] == DK_TX_REPEAT || raw[1] == DK_TX_AUTO ||
            raw[1] == DK_TX_REPEAT_CONTROL) continue;
        if (boot != tx_boot_id_) {
          tx_capability_ = QDeadlineTimer(0);
          tx_boot_id_ = boot;
          utilization_expiry_ = QDeadlineTimer(0);
          for (auto& latency : tx_latency_) latency.reset();
          clearRepeat();
          tx_request_.clear();
          recent_data_frames_.clear();
          if (tx_in_flight_) {
            tx_state_ = TxState::Error;
            tx_status_ = tr("USB adapter restarted. Transmission outcome unknown.");
          } else {
            tx_state_ = TxState::Ready;
            tx_status_.clear();
          }
          tx_in_flight_ = 0;
        }
        if (raw[1] == DK_TX_FEEDBACK) {
          if (sequence && sequence == tx_in_flight_ && bus == tx_bus_) {
            if (raw[20] == DK_TX_ACKNOWLEDGED) {
              ++tx_acknowledged_;
              tx_state_ = TxState::Acknowledged;
              if (!repeat_active_) {
                tx_status_ = tr("Transmitted and acknowledged by another CAN node.");
              } else if (!repeat_cancelled_) {
                tx_status_ = tr("Acknowledged frames (session total): %1").arg(tx_acknowledged_);
              }
            } else {
              tx_error_details_.reset();
              tx_feedback_error_ = raw[20] == DK_TX_UNCONFIRMED
                  ? tr("Could not confirm the send on CAN bus %1.").arg(bus)
                  : tr("Could not determine whether the frame was sent on CAN bus %1.").arg(bus);
              if (n == DK_TX_FEEDBACK_DIAGNOSTICS_SIZE + 2) {
                const auto fifo = dk_get(raw + 21, 4);
                const auto bus_diag = dk_get(raw + 25, 4);
                const auto counters = dk_get(raw + 29, 4);
                QStringList details;
                if (fifo & (1u << 5)) details += tr("The controller reported a transmit error.");
                if (fifo & (1u << 6)) details += tr("The controller flagged lost arbitration (another frame had priority).");
                if (counters & (1u << 21)) details += tr("The controller stopped transmitting after too many errors (bus-off).");
                if (bus_diag & (1u << 18))
                  details += tr("The controller has recorded a missing acknowledgment. This may be from an earlier send.");
                tx_error_details_ = TxErrorDetails{details, static_cast<uint32_t>(fifo),
                    static_cast<uint32_t>(bus_diag), static_cast<uint32_t>((counters >> 8) & 0xff),
                    static_cast<uint32_t>(counters & 0xff), tx_acknowledged_};
                details += tr("Controller error counters at failure — transmit %L1, receive %L2")
                               .arg((counters >> 8) & 0xff).arg(counters & 0xff);
                details += tr("Raw codes — FIFO 0x%1, bus diagnostics 0x%2").arg(fifo, 0, 16).arg(bus_diag, 0, 16);
                tx_feedback_error_ += QLatin1Char('\n') + details.join(QLatin1Char('\n'));
              }
              tx_state_ = TxState::Error;
              tx_status_ = tx_feedback_error_;
            }
            if (!repeat_active_) tx_in_flight_ = 0;
          }
          continue;
        }
        const bool repeat_result = raw[1] == DK_TX_REPEAT_RESULT;
        const bool automatic_result = raw[1] == DK_TX_AUTO_RESULT;
        const bool ongoing_result = repeat_result || automatic_result;
        if (ongoing_result || raw[1] == DK_TX_RESULT) {
          if (sequence == 0 && raw[20] == DK_TX_READY) {
            if (!ongoing_result && bus == DK_TX_CAPABILITY) tx_capability_ = QDeadlineTimer(2500);
          } else if (sequence && (sequence == tx_in_flight_ ||
                     (!ongoing_result && !repeat_active_ && sequence == tx_sequence_)) &&
                     ongoing_result == repeat_active_ &&
                     (!ongoing_result || (bus == repeat_bus_ && automatic_result == repeat_automatic_))) {
            if (raw[20] == DK_TX_REPEAT_STARTED) {
              if (!repeat_cancelled_) {
                repeat_deadline_ = QDeadlineTimer(repeat_duration_ms_ + 1500);
                tx_state_ = TxState::Waiting;
                tx_status_ = repeat_automatic_ ? tr("Automatically restoring changed frames…")
                                               : tr("Sending at a fixed rate…");
              }
              continue;
            }
            tx_state_ = TxState::Error;
            switch (raw[20]) {
              case DK_TX_QUEUED:
                tx_state_ = TxState::Queued;
                if (!ongoing_result) {
                  tx_status_ = tr("Queued by the USB adapter. Waiting for controller feedback.");
                  continue;
                }
                tx_status_ = tr("Queued by the USB adapter.");
                break;
              case DK_TX_RATE_LIMIT:
                tx_status_ = tr("Requests are arriving too quickly. Wait before sending again.");
                break;
              case DK_TX_FIFO_FULL:
                tx_status_ = tr("CAN bus %1 transmit queue is full. The frame was rejected.").arg(bus);
                break;
              case DK_TX_CONTROLLER_ERROR:
                tx_status_ = tr("USB adapter could not access the CAN controller on bus %1.").arg(bus);
                break;
              case DK_TX_PREVIOUS_FAILED:
                tx_status_ = tr("The previous transmission failed on CAN bus %1. Its queue was cleared.").arg(bus);
                break;
              case DK_TX_REPEAT_FINISHED:
                tx_state_ = TxState::Queued;
                tx_status_ = repeat_automatic_ ? tr("Automatic send finished.") : tr("Fixed-rate send finished.");
                break;
              case DK_TX_REPEAT_CANCELLED:
                tx_state_ = TxState::Ready;
                tx_status_ = repeat_automatic_ ? tr("Automatic send cancelled.") : tr("Fixed-rate send cancelled.");
                break;
              case DK_TX_REPEAT_BUSY:
                tx_status_ = tr("The USB adapter already has a timed send in progress.");
                break;
              case DK_TX_REPEAT_LEASE_EXPIRED:
                tx_status_ = repeat_automatic_ ? tr("Automatic send stopped after losing its USB connection.")
                                               : tr("Fixed-rate send stopped after losing its USB connection.");
                break;
              default:
                tx_status_ = tr("The USB adapter rejected the request without giving a reason.");
                break;
            }
            if (ongoing_result) {
              if (tx_state_ == TxState::Queued && tx_acknowledged_) tx_state_ = TxState::Acknowledged;
              if (!tx_feedback_error_.isEmpty()) {
                tx_state_ = TxState::Error;
                tx_status_ = tx_feedback_error_;
              }
              tx_status_ += QStringLiteral("\n") + tr("Acknowledged frames (session total): %1").arg(tx_acknowledged_);
            }
            tx_in_flight_ = 0;
            clearRepeat();
          }
          continue;
        }
        if (raw[1] == DK_LOG) {
          constexpr size_t log_history_limit = 2000;
          if (firmware_log_.size() == log_history_limit) firmware_log_.pop_front();
          firmware_log_.push_back(QString::fromUtf8(reinterpret_cast<const char*>(raw + 20), n - 22));
          ++firmware_log_count_;
          continue;
        }
        if (raw[1] == DK_TX_LATENCY) {
          tx_latency_[bus] = TxLatency{
              static_cast<uint32_t>(dk_get(raw + 20, 4)),
              static_cast<uint32_t>(dk_get(raw + 24, 4)),
              static_cast<uint32_t>(dk_get(raw + 28, 4)),
              static_cast<uint32_t>(dk_get(raw + 32, 4)),
              static_cast<uint32_t>(dk_get(raw + 36, 4)),
              static_cast<uint32_t>(dk_get(raw + 40, 4)),
              static_cast<uint32_t>(dk_get(raw + 44, 4)),
              static_cast<uint32_t>(dk_get(raw + 48, 4)),
              static_cast<uint32_t>(dk_get(raw + 52, 4)),
              static_cast<uint32_t>(dk_get(raw + 56, 4)),
              static_cast<uint32_t>(dk_get(raw + 60, 4)),
              static_cast<uint32_t>(dk_get(raw + 64, 4)),
              static_cast<uint32_t>(dk_get(raw + 68, 4)),
              static_cast<uint32_t>(dk_get(raw + 72, 4)),
              static_cast<uint32_t>(dk_get(raw + 76, 4)),
              static_cast<uint32_t>(dk_get(raw + 80, 4)),
              static_cast<uint32_t>(dk_get(raw + 84, 4)),
              static_cast<uint32_t>(dk_get(raw + 88, 4)),
              static_cast<uint32_t>(dk_get(raw + 92, 4)),
              static_cast<uint32_t>(dk_get(raw + 96, 4)),
              static_cast<uint32_t>(dk_get(raw + 100, 4)),
              static_cast<uint32_t>(dk_get(raw + 104, 4)),
          };
          continue;
        }
        if (raw[1] == DK_UTILIZATION) {
          utilization_ = {
              static_cast<uint16_t>(dk_get(raw + 20, 2)),
              {static_cast<uint16_t>(dk_get(raw + 22, 2)), static_cast<uint16_t>(dk_get(raw + 24, 2))},
              static_cast<uint16_t>(dk_get(raw + 26, 2)),
              {static_cast<uint16_t>(dk_get(raw + 28, 2)), static_cast<uint16_t>(dk_get(raw + 30, 2))},
          };
          utilization_expiry_ = QDeadlineTimer(3000);
          continue;
        }
        if (!have_boot || boot != boot_id) {
          if (have_boot) resets_++;
          have_boot = true;
          boot_id = boot;
          have_sequence[0] = have_sequence[1] = false;
          for (int i = 0; i < 2; i++) {
            queue_base[i] = queue_drops_[i];
            overflow_base[i] = overflows_[i];
          }
          usb_base = usb_aborts_;
          time_offset = newest_ns && device_ns <= newest_ns ? newest_ns + 1 - device_ns : 0;
        }
        if (raw[1] == DK_STATUS) {
          queue_drops_[bus] = queue_base[bus] + dk_get(raw + 20, 4);
          overflows_[bus] = overflow_base[bus] + dk_get(raw + 24, 4);
          usb_aborts_ = usb_base + dk_get(raw + 28, 4);
          continue;
        }
        if (!tx_frame) {
          if (have_sequence[bus]) {
            const uint32_t delta = sequence - last_sequence[bus];
            if (delta == 0 || delta >= 0x80000000U) {
              corrupt_++;
              continue;
            }
            gaps_[bus] += delta - 1;
          }
          have_sequence[bus] = true;
          last_sequence[bus] = sequence;
          frames_[bus]++;
          if (!(raw[3] & 2)) {
            constexpr size_t recent_frame_limit = 64;
            auto& recent = recent_data_frames_[MessageId(bus, dk_get(raw + 20, 4))];
            if (recent.size() == recent_frame_limit) recent.pop_front();
            recent.push_back({QByteArray(reinterpret_cast<const char*>(raw + 25), raw[24]), raw[3], boot});
          }
        }
      }
      const uint64_t timestamp = device_ns + time_offset;
      newest_ns = std::max(newest_ns, timestamp);
      // RTR carries no payload.
      reorder.push({timestamp, order++, QDeadlineTimer(reorder_ms, Qt::PreciseTimer),
                    static_cast<uint8_t>(tx_frame ? bus + TxSourceOffset : bus), static_cast<uint32_t>(dk_get(raw + 20, 4)),
                    QByteArray(reinterpret_cast<const char*>(raw + 25), (raw[3] & 2) ? 0 : raw[24])});
    }
  }
  publishReady(true);
  if (QThread::currentThread()->isInterruptionRequested() && serial.isOpen()) {
    const auto stop = sessionControl(DK_SESSION_STOP);
    if (serial.write(stop) == stop.size()) serial.waitForBytesWritten(50);
  }
}
