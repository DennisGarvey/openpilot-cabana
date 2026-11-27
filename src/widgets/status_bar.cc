#include "status_bar.h"

#include <chrono>
#include <utility>
#include <unistd.h>

#include <QFontDatabase>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>

#include "modules/system/stream_manager.h"
#include "core/streams/abstract_stream.h"
#include "core/streams/device_stream.h"
#include "core/streams/custom_usb_serial_stream.h"
#ifdef __linux__
#include <unistd.h>

#include <fstream>
#include <numeric>
#elif __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif
#include "modules/settings/settings.h"
#include "replay/include/util.h"

StatusBar::StatusBar(QWidget* parent) : QStatusBar(parent) {
  progress_bar_ = new QProgressBar(this);
  progress_bar_->setRange(0, 100);
  progress_bar_->setFixedSize({230, 16});
  progress_bar_->setTextVisible(true);
  progress_bar_->setVisible(false);

  // Left-aligned Hint
  QLabel* help_label = new QLabel(tr("For Help, Press F1"), this);
  addWidget(help_label);

  // Right-aligned Metrics
  status_label_ = new QLabel(this);
  cpu_label_ = new QLabel(this);
  mem_label_ = new QLabel(this);
  live_stats_label_ = new QLabel(this);

  QFont mono_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono_font.setPixelSize(12);

  progress_bar_->setFont(mono_font);
  status_label_->setFont(mono_font);
  cpu_label_->setFont(mono_font);
  mem_label_->setFont(mono_font);
  live_stats_label_->setFont(mono_font);

  // Add in order (Right to Left)
  addPermanentWidget(progress_bar_);
  addPermanentWidget(status_label_);
  addPermanentWidget(cpu_label_);
  addPermanentWidget(mem_label_);
  addPermanentWidget(live_stats_label_);

  setStyleSheet("QStatusBar::item { border: none; padding-left: 10px; }");

  connect(&StreamManager::instance(), &StreamManager::streamChanged, this, &StatusBar::monitorLiveStream);

  connect(&settings, &Settings::usbStatusMetricsChanged, this, &StatusBar::updateUsbMetrics);
  timer_ = new QTimer(this);
  connect(timer_, &QTimer::timeout, this, &StatusBar::updateMetrics);
  timer_->setTimerType(Qt::PreciseTimer);
  timer_->start(1000);
}

void StatusBar::monitorLiveStream() {
  total_event_count_ = last_event_count_ = 0;
  total_data_size_ = last_data_size_ = 0;
  last_time_ = std::chrono::steady_clock::now();
  live_stats_label_->clear();
  live_stats_label_->setToolTip("");
  updateUsbMetrics();

  if (auto* stream = dynamic_cast<DeviceStream*>(StreamManager::stream())) {
    connect(stream, &AbstractStream::eventsMerged, this, [this](const MessageEventsMap& batch) {
      for (const auto& [id, events] : batch) {
        total_event_count_ += events.size();
        for (const auto* event : events) {
          total_data_size_ += sizeof(CanEvent) + event->size;
        }
      }
    });
  }
}

void StatusBar::updateUsbMetrics() {
  if (auto* stream = dynamic_cast<CustomUsbSerialStream*>(StreamManager::stream())) {
    live_stats_label_->setText(stream->captureStatus(settings.usb_status_metrics));
    live_stats_label_->setToolTip(stream->captureDetails());
  }
}

void StatusBar::updateMetrics() {
  double cpu_percent = 0.0;
  double mem_mb = 0.0;

  if (dynamic_cast<DeviceStream*>(StreamManager::stream())) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double, std::milli>(now - std::exchange(last_time_, now)).count();
    const auto events = total_event_count_ - std::exchange(last_event_count_, total_event_count_);
    const auto bytes = total_data_size_ - std::exchange(last_data_size_, total_data_size_);
    const double avg_interval = events > 0 ? elapsed / events : 0.0;
    live_stats_label_->setText(QString("Avg: %1ms | Last 1s: %2 events (%3) | Total: %4 events (%5)")
                                  .arg(avg_interval, 6, 'f', 2)
                                  .arg(events, 6)
                                  .arg(QString::fromStdString(formattedDataSize(bytes)))
                                  .arg(total_event_count_, 6)
                                  .arg(QString::fromStdString(formattedDataSize(total_data_size_))));
  } else if (dynamic_cast<CustomUsbSerialStream*>(StreamManager::stream())) {
    updateUsbMetrics();
  } else {
    live_stats_label_->setText("");
    live_stats_label_->setToolTip("");
  }

#ifdef __linux__
  uint64_t proc_utime, proc_stime;
  std::ifstream proc_stat("/proc/self/stat");
  std::string dummy;
  for (int i = 0; i < 13; ++i) proc_stat >> dummy;
  proc_stat >> proc_utime >> proc_stime;
  uint64_t proc_total = proc_utime + proc_stime;

  std::ifstream sys_stat("/proc/stat");
  sys_stat >> dummy;
  uint64_t sys_total = 0, val;
  while (sys_stat >> val) sys_total += val;

  if (last_sys_time_ > 0 && sys_total > last_sys_time_) {
    cpu_percent =
        (100.0 * (proc_total - last_proc_time_) / (sys_total - last_sys_time_)) * sysconf(_SC_NPROCESSORS_ONLN);
  }
  last_proc_time_ = proc_total;
  last_sys_time_ = sys_total;

  std::ifstream stat_m("/proc/self/statm");
  long pages;
  if (stat_m >> pages >> pages) mem_mb = (pages * sysconf(_SC_PAGESIZE)) / 1024.0 / 1024.0;

#elif __APPLE__
  static const auto timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return info;
  }();
  task_absolutetime_info_data_t cpu_times;
  mach_msg_type_number_t count = TASK_ABSOLUTETIME_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_ABSOLUTETIME_INFO, (task_info_t)&cpu_times, &count) == KERN_SUCCESS) {
    // Task totals include CPU time from threads that have exited.
    const uint64_t total = cpu_times.total_user + cpu_times.total_system;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double, std::nano>(now - last_cpu_sample_).count();
    if (last_cpu_sample_ != std::chrono::steady_clock::time_point{} && total >= last_proc_time_ &&
        elapsed > 0 && timebase.denom != 0) {
      const double cpu_ns = static_cast<double>(total - last_proc_time_) * timebase.numer / timebase.denom;
      cpu_percent = 100.0 * cpu_ns / elapsed;
    }
    last_proc_time_ = total;
    last_cpu_sample_ = now;
  }

  struct mach_task_basic_info info;
  mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount) == KERN_SUCCESS) {
    mem_mb = info.resident_size / 1024.0 / 1024.0;
  }
#endif

  QString cpu_val = QString::number(cpu_percent, 'f', 1);
  cpu_label_->setText(tr("CPU:%1%").arg(cpu_val, 5));

  QString mem_val = QString::number(mem_mb, 'f', 0);
  mem_label_->setText(tr("MEM:%1 MB").arg(mem_val, 4));

  status_label_->setText(tr("Cache: %1m | FPS: %2").arg(settings.max_cached_minutes).arg(settings.fps));
}

void StatusBar::updateDownloadProgress(uint64_t cur, uint64_t total, bool success) {
  if (success && total > 0 && cur < total) {
    double p = (static_cast<double>(cur) / total) * 100.0;
    progress_bar_->setValue(static_cast<int>(p));

    // Show current size / total size in format
    QString size_str = QString::fromStdString(formattedDataSize(total));
    progress_bar_->setFormat(tr("Downloading %1 (%p%)").arg(size_str));

    if (!progress_bar_->isVisible()) progress_bar_->show();
  } else {
    progress_bar_->hide();
  }
}
