#pragma once

#include <QDeadlineTimer>
#include <deque>
#include <mutex>

#include "live_stream.h"

class CustomUsbSerialStream : public LiveStream {
 public:
  CustomUsbSerialStream(QObject* parent, const QString& port, qint32 baud_rate = 8000000);
  ~CustomUsbSerialStream() override { stop(); }
  QString routeName() const override { return QStringLiteral("Custom USB Serial: %1").arg(port_); }
  QString captureStatus() const;
  QString captureDetails() const;
  struct CapturedFrame {
    QByteArray data;
    uint8_t flags;
    uint32_t boot_id;
  };
  std::optional<CapturedFrame> latestDataFrame(const MessageId& id, const dbc::Signal* signal = nullptr) const;
  bool txReady() const;
  bool sendFrame(uint8_t bus, uint32_t address, uint8_t flags, const QByteArray& data,
                 std::optional<uint32_t> expected_boot = std::nullopt);
  enum class TxState { Ready, Waiting, Queued, Error };
  QString txStatus(TxState* state = nullptr) const;

 private:
  void streamThread() override;
  void setError(const QString& error);

  QString port_;
  qint32 baud_rate_;
  mutable std::mutex stats_mutex_;
  QString error_;
  QDeadlineTimer tx_capability_{0}, tx_deadline_{0};
  uint32_t tx_boot_id_ = 0, tx_sequence_ = 0, tx_in_flight_ = 0;
  QByteArray tx_request_;
  QString tx_status_;
  TxState tx_state_ = TxState::Ready;
  std::unordered_map<MessageId, std::deque<CapturedFrame>> recent_data_frames_;
  uint64_t frames_[2] = {}, gaps_[2] = {};
  uint64_t queue_drops_[2] = {}, overflows_[2] = {}, usb_aborts_ = 0;
  uint64_t corrupt_ = 0, resets_ = 0, last_record_ms_ = 0;
};
