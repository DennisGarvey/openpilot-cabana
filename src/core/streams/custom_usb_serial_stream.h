#pragma once

#include <mutex>

#include "live_stream.h"

class CustomUsbSerialStream : public LiveStream {
 public:
  CustomUsbSerialStream(QObject* parent, const QString& port, qint32 baud_rate = 8000000);
  ~CustomUsbSerialStream() override { stop(); }
  QString routeName() const override { return QStringLiteral("Custom USB Serial: %1").arg(port_); }
  QString captureStatus() const;
  QString captureDetails() const;

 private:
  void streamThread() override;
  void setError(const QString& error);

  QString port_;
  qint32 baud_rate_;
  mutable std::mutex stats_mutex_;
  QString error_;
  uint64_t frames_[2] = {}, gaps_[2] = {};
  uint64_t queue_drops_[2] = {}, overflows_[2] = {}, usb_aborts_ = 0;
  uint64_t corrupt_ = 0, resets_ = 0, last_record_ms_ = 0;
};
