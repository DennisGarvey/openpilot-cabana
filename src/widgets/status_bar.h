#pragma once

#include <QStatusBar>
#include <chrono>

class QLabel;
class QProgressBar;
class QTimer;

class StatusBar : public QStatusBar {
  Q_OBJECT
 public:
  explicit StatusBar(QWidget* parent = nullptr);
  void updateDownloadProgress(uint64_t cur, uint64_t total, bool success);
  void updateMetrics();

  // Live stream monitoring
  void monitorLiveStream();

 private:
  void updateUsbMetrics();
  QProgressBar* progress_bar_;
  QLabel* status_label_;
  QLabel* cpu_label_;
  QLabel* mem_label_;
  QTimer* timer_;

  // Live stream stats labels
  QLabel* live_stats_label_;

  uint64_t last_proc_time_ = 0;
  uint64_t last_sys_time_ = 0;

  // Live stream monitoring data
  int64_t last_event_count_ = 0;
  uint64_t last_data_size_ = 0;
  std::chrono::steady_clock::time_point last_time_ = std::chrono::steady_clock::now();
  int64_t total_event_count_ = 0;
  uint64_t total_data_size_ = 0;
#ifdef __APPLE__
  std::chrono::steady_clock::time_point last_cpu_sample_;
#endif
};
