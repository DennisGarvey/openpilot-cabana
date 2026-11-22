#include "custom_usb_serial_stream.h"

#include <QRandomGenerator>
#include <QSerialPort>
#include <QThread>
#include <stdexcept>

#include "custom_usb_serial_protocol.h"

CustomUsbSerialStream::CustomUsbSerialStream(QObject* parent, const QString& port, qint32 baud_rate)
    : LiveStream(parent, false), port_(port), baud_rate_(baud_rate) {
  if (port.isEmpty()) throw std::runtime_error("Choose a USB serial port");
}

void CustomUsbSerialStream::setError(const QString& error) {
  std::lock_guard lock(stats_mutex_);
  error_ = error;
  tx_capability_ = QDeadlineTimer(0);
  tx_request_.clear();
  tx_status_ = tx_in_flight_ ? tr("USB connection lost. Transmission outcome unknown.") : error;
  tx_state_ = TxState::Error;
  tx_in_flight_ = 0;
  repeat_supported_ = automatic_supported_ = false;
  clearRepeat();
}

QString CustomUsbSerialStream::captureStatus() const {
  std::lock_guard lock(stats_mutex_);
  if (!error_.isEmpty()) return QStringLiteral("Custom USB Serial: %1").arg(error_);
  if (!last_record_ms_) return QStringLiteral("Custom USB Serial: waiting for USB streaming firmware");
  if (QDateTime::currentMSecsSinceEpoch() - last_record_ms_ > 3000)
    return QStringLiteral("Custom USB Serial: no USB data for 3 seconds");
  return QStringLiteral("USB 0:%1 1:%2 | gaps:%3 Q:%4 FIFO:%5 bad:%6")
      .arg(frames_[0])
      .arg(frames_[1])
      .arg(gaps_[0] + gaps_[1])
      .arg(uint64_t(queue_drops_[0]) + queue_drops_[1])
      .arg(uint64_t(overflows_[0]) + overflows_[1])
      .arg(corrupt_);
}

QString CustomUsbSerialStream::captureDetails() const {
  std::lock_guard lock(stats_mutex_);
  return QStringLiteral(
             "Bus 0: %1 frames, %2 sequence gaps, %3 queue drops, %4 FIFO overflow events\n"
             "Bus 1: %5 frames, %6 sequence gaps, %7 queue drops, %8 FIFO overflow events\n"
             "USB aborted writes: %9; invalid records: %10; device restarts: %11\n"
             "FIFO events mean at least one lost frame each. Queue, USB and sequence loss overlap.\n"
             "Timestamps are device software receive times.")
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
      .arg(resets_);
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
  return repeat_supported_ && !tx_capability_.hasExpired();
}

bool CustomUsbSerialStream::automaticSupported() const {
  std::lock_guard lock(stats_mutex_);
  return automatic_supported_ && !tx_capability_.hasExpired();
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

QString CustomUsbSerialStream::txStatus(TxState* state) const {
  std::lock_guard lock(stats_mutex_);
  auto result = [state](TxState current, const QString& message) {
    if (state) *state = current;
    return message;
  };
  if (!error_.isEmpty()) return result(TxState::Error, tx_status_);
  if (!repeat_active_ && tx_in_flight_ && tx_deadline_.hasExpired())
    return result(TxState::Error, tr("Response timed out. Transmission outcome unknown."));
  if (tx_capability_.hasExpired() && !tx_in_flight_)
    return result(TxState::Waiting, tr("Waiting for USB transmission support."));
  return result(tx_state_, tx_status_);
}

bool CustomUsbSerialStream::sendFrame(uint8_t bus, uint32_t address, uint8_t flags, const QByteArray& data,
                                     std::optional<uint32_t> expected_boot, std::optional<RepeatTiming> repeat,
                                     QByteArray signal_mask) {
  if (data.size() > 64) return false;
  if (repeat && signal_mask.isEmpty()) signal_mask.fill(char(0xff), data.size());
  if (repeat && signal_mask.size() != data.size()) return false;
  std::lock_guard lock(stats_mutex_);
  if (expected_boot && *expected_boot != tx_boot_id_) return false;
  if (repeat_active_ || tx_capability_.hasExpired() ||
      (repeat && (repeat->automatic ? !automatic_supported_ : !repeat_supported_)) ||
      (tx_in_flight_ && !tx_deadline_.hasExpired())) return false;
  if (++tx_sequence_ == 0) ++tx_sequence_;
  const uint8_t type = !repeat ? DK_TX_REQUEST : (repeat->automatic ? DK_TX_AUTO : DK_TX_REPEAT);
  uint8_t raw[DK_MAX_RAW] = {DK_VERSION, type, bus, flags};
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
  uint8_t wire[DK_MAX_WIRE], decoded[DK_MAX_RAW];
  size_t size = dk_encode(raw, 25 + data.size() * (repeat ? 2 : 1), wire);
  if (!size || !dk_decode(wire, size - 1, decoded)) return false;
  tx_request_ = QByteArray(1, '\0') + QByteArray(reinterpret_cast<const char*>(wire), size);
  tx_in_flight_ = tx_sequence_;
  tx_deadline_ = QDeadlineTimer(1500);
  if (repeat) {
    repeat_active_ = true;
    repeat_automatic_ = repeat->automatic;
    repeat_bus_ = bus;
    repeat_duration_ms_ = repeat->duration_ms;
    repeat_deadline_ = tx_deadline_;
    repeat_keepalive_ = QDeadlineTimer(300);
  }
  tx_state_ = TxState::Waiting;
  tx_status_ = tr("Waiting for the USB adapter to respond.");
  return true;
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
  QByteArray pending;
  bool discarding = false, have_boot = false;
  uint32_t boot_id = 0, last_sequence[2] = {};
  bool have_sequence[2] = {};
  uint64_t last_ns = 0, time_offset = 0;
  uint64_t queue_base[2] = {}, overflow_base[2] = {}, usb_base = 0;
  while (!QThread::currentThread()->isInterruptionRequested()) {
    QByteArray request;
    {
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
    if (!serial.bytesAvailable() && !serial.waitForReadyRead(100)) {
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
      if (!n) {
        if (nonempty) {
          std::lock_guard lock(stats_mutex_);
          corrupt_++;
        }
        continue;
      }
      const uint8_t bus = raw[2];
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
          repeat_supported_ = automatic_supported_ = false;
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
        const bool repeat_result = raw[1] == DK_TX_REPEAT_RESULT;
        const bool automatic_result = raw[1] == DK_TX_AUTO_RESULT;
        const bool ongoing_result = repeat_result || automatic_result;
        if (ongoing_result || raw[1] == DK_TX_RESULT) {
          if (sequence == 0 && raw[20] == DK_TX_READY) {
            tx_capability_ = QDeadlineTimer(2500);
            if (automatic_result) automatic_supported_ = true;
            else repeat_supported_ = bus == DK_TX_REPEAT_CAPABILITY;
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
            tx_in_flight_ = 0;
            clearRepeat();
          }
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
          time_offset = last_ns && device_ns <= last_ns ? last_ns + 1 - device_ns : 0;
        }
        if (raw[1] == DK_STATUS) {
          queue_drops_[bus] = queue_base[bus] + dk_get(raw + 20, 4);
          overflows_[bus] = overflow_base[bus] + dk_get(raw + 24, 4);
          usb_aborts_ = usb_base + dk_get(raw + 28, 4);
          continue;
        }
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
      // Playback resumes strictly after its cursor. Give tied or late USB
      // records distinct times so a later batch can still update playback.
      last_ns = std::max(last_ns + 1, device_ns + time_offset);
      MessageBuilder message;
      auto event = message.initEvent();
      event.setLogMonoTime(last_ns);
      auto can = event.initCan(1);
      can[0].setSrc(bus);
      can[0].setAddress(dk_get(raw + 20, 4));
      // RTR carries no payload.
      can[0].setDat(kj::arrayPtr(raw + 25, (raw[3] & 2) ? 0 : raw[24]));
      handleEvent(capnp::messageToFlatArray(message));
    }
  }
}
