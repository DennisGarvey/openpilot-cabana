#include "custom_usb_serial_stream.h"

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

void CustomUsbSerialStream::streamThread() {
  QSerialPort serial;
  serial.setPortName(port_);
  serial.setBaudRate(baud_rate_);  // Use a high host line setting for the native USB device.
  if (!serial.open(QIODevice::ReadOnly)) {
    setError(serial.errorString());
    return;
  }
  QByteArray pending;
  bool discarding = false, have_boot = false;
  uint32_t boot_id = 0, last_sequence[2] = {};
  bool have_sequence[2] = {};
  uint64_t last_ns = 0, time_offset = 0;
  uint64_t queue_base[2] = {}, overflow_base[2] = {}, usb_base = 0;
  while (!QThread::currentThread()->isInterruptionRequested()) {
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
      }
      // Two RX tasks may enqueue close timestamps out of order.
      // Keep the Cabana playback clock nondecreasing.
      last_ns = std::max(last_ns, device_ns + time_offset);
      MessageBuilder message;
      auto event = message.initEvent();
      event.setLogMonoTime(std::max(uint64_t(1), last_ns));
      auto can = event.initCan(1);
      can[0].setSrc(bus);
      can[0].setAddress(dk_get(raw + 20, 4));
      // RTR carries no payload.
      can[0].setDat(kj::arrayPtr(raw + 25, (raw[3] & 2) ? 0 : raw[24]));
      handleEvent(capnp::messageToFlatArray(message));
    }
  }
}
