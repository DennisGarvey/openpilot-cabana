#pragma once

#include <QDeadlineTimer>
#include <deque>
#include <mutex>

#include "live_stream.h"

class CustomUsbSerialStream : public LiveStream {
 public:
  static constexpr uint8_t TxSourceOffset = 128;
  CustomUsbSerialStream(QObject* parent, const QString& port, qint32 baud_rate = 8000000);
  ~CustomUsbSerialStream() override { stop(); }
  QString routeName() const override { return QStringLiteral("Custom USB Serial: %1").arg(port_); }
  QString captureStatus() const;
  QString captureDetails() const;
  struct Utilization {
    uint16_t serial;
    uint16_t cpu[2];
    uint16_t spi;
    uint16_t can[2];
  };
  std::optional<Utilization> utilization() const;

  struct CapturedFrame {
    QByteArray data;
    uint8_t flags;
    uint32_t boot_id;
  };
  std::optional<CapturedFrame> latestDataFrame(const MessageId& id, const dbc::Signal* signal = nullptr) const;
  struct RepeatTiming {
    unsigned frequency_hz;
    unsigned duration_ms;
    bool automatic = false;
  };
  struct Counter {
    uint16_t start_bit;
    uint8_t size;
    bool little_endian;
  };
  struct Checksum {
    uint8_t byte_index;
  };
  bool txReady() const;
  bool repeatSupported() const;
  int fixedRateLimit() const;
  bool transmitFeedbackSupported() const;
  bool automaticSupported() const;
  bool counterSupported() const;
  bool repeatPending() const;
  void cancelRepeat();
  bool sendFrame(uint8_t bus, uint32_t address, uint8_t flags, const QByteArray& data,
                 std::optional<uint32_t> expected_boot = std::nullopt,
                 std::optional<RepeatTiming> repeat = std::nullopt,
                 QByteArray signal_mask = {}, QByteArray selector_mask = {},
                 std::optional<Counter> counter = std::nullopt,
                 std::optional<Checksum> checksum = std::nullopt);
  enum class TxState { Ready, Waiting, Queued, Acknowledged, Error };
  QString txStatus(TxState* state = nullptr, bool include_result = true) const;

 private:
  void streamThread() override;
  void setError(const QString& error);
  void clearRepeat();
  QByteArray repeatControl(uint8_t opcode) const;

  QString port_;
  qint32 baud_rate_;
  mutable std::mutex stats_mutex_;
  QString error_;
  QDeadlineTimer tx_capability_{0}, tx_deadline_{0};
  uint32_t tx_boot_id_ = 0, tx_sequence_ = 0, tx_in_flight_ = 0;
  QByteArray tx_request_;
  uint8_t tx_bus_ = 0;
  unsigned tx_acknowledged_ = 0;
  QString tx_feedback_error_;
  bool repeat_active_ = false, repeat_cancelled_ = false, repeat_automatic_ = false;
  uint8_t repeat_bus_ = 0;
  QDeadlineTimer repeat_keepalive_{0}, repeat_deadline_{0};
  unsigned repeat_duration_ms_ = 0;
  QString tx_status_;
  TxState tx_state_ = TxState::Ready;
  std::unordered_map<MessageId, std::deque<CapturedFrame>> recent_data_frames_;
  Utilization utilization_{};
  QDeadlineTimer utilization_expiry_{0};
  uint64_t frames_[2] = {}, gaps_[2] = {};
  uint64_t queue_drops_[2] = {}, overflows_[2] = {}, usb_aborts_ = 0;
  uint64_t corrupt_ = 0, resets_ = 0, last_record_ms_ = 0;
};
