#include "file_stream.h"

#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>

#include "common/timing.h"
#include "modules/settings/settings.h"

FileStream::FileStream(QObject* parent, const QStringList& file_paths)
    : AbstractStream(parent), file_paths_(file_paths) {
  begin_mono_ns_ = nanos_since_boot();
}

FileStream::~FileStream() {
  if (playback_thread_) {
    {
      std::lock_guard lk(pause_mutex_);
      playback_thread_->requestInterruption();
    }
    pause_cv_.notify_all();
    playback_thread_->wait();
    delete playback_thread_;
  }
}

QString FileStream::routeName() const {
  return file_paths_.size() == 1 ? file_paths_.first() : tr("%1 files").arg(file_paths_.size());
}

void FileStream::loadParsedFiles() {
  std::vector<ParsedCanFrame> all_parsed;

  for (const QString& file_path : file_paths_) {
    auto file_frames = parseFile(file_path);
    if (file_frames.empty()) continue;

    // Stitch: if this file's timestamps restart (overlap with already-parsed frames),
    // shift the new frames to follow the previous file's end by 1 ms.
    if (!all_parsed.empty() && file_frames.front().rel_ns <= all_parsed.back().rel_ns) {
      uint64_t shift = all_parsed.back().rel_ns + 1'000'000ULL - file_frames.front().rel_ns;
      for (auto& f : file_frames) f.rel_ns += shift;
    }

    all_parsed.insert(all_parsed.end(),
                      std::make_move_iterator(file_frames.begin()),
                      std::make_move_iterator(file_frames.end()));
  }

  // Sort handles files provided out of order.
  std::sort(all_parsed.begin(), all_parsed.end(),
            [](const ParsedCanFrame& a, const ParsedCanFrame& b) { return a.rel_ns < b.rel_ns; });

  std::vector<const CanEvent*> events;
  events.reserve(all_parsed.size());
  for (const auto& f : all_parsed) {
    events.push_back(newEvent(begin_mono_ns_ + f.rel_ns, f.bus, f.address, f.data, f.size));
  }

  if (!events.empty()) {
    duration_s_ = (events.back()->mono_ns - begin_mono_ns_) / 1e9;
    mergeEvents(events);
  }
}

void FileStream::start() {
  if (all_events_.empty()) return;

  auto* timer = new QTimer(this);
  timer->setInterval(1000 / settings.fps);
  connect(timer, &QTimer::timeout, this, [this]() { commitSnapshots(); });
  connect(&settings, &Settings::changed, this, [timer]() { timer->setInterval(1000 / settings.fps); });
  timer->start();

  playback_thread_ = QThread::create([this]() { playbackThread(); });
  playback_thread_->start();
}

void FileStream::seekTo(double sec) {
  sec = std::clamp(sec, 0.0, duration_s_);
  {
    std::lock_guard lk(pause_mutex_);
    seek_to_.store(sec);
  }
  pause_cv_.notify_all();
  emit seeking(sec);
}

void FileStream::pause(bool pause) {
  {
    std::lock_guard lk(pause_mutex_);
    paused_.store(pause);
  }
  pause_cv_.notify_all();
  emit(pause ? paused() : resume());
}

void FileStream::setSpeed(float speed) {
  speed_.store(speed);
  pause_cv_.notify_all();
}

void FileStream::playbackThread() {
  size_t idx = 0;
  uint64_t anchor_wall_ns = nanos_since_boot();  // wall-clock at last anchor
  uint64_t anchor_file_ns = 0;                    // file-time progress (ns from begin_mono_ns_) at last anchor
  uint64_t position_ns = 0;
  const uint64_t duration_ns = all_events_.back()->mono_ns - begin_mono_ns_;
  float prev_speed = 1.0f;

  // Re-anchor wall clock and file-time base at the current playback position.
  // Must be called after any discontinuity: seek, pause/unpause, speed change.
  auto reanchor = [&]() {
    anchor_wall_ns = nanos_since_boot();
    anchor_file_ns = position_ns;
  };

  auto applySeek = [&](double sec) {
    position_ns = std::min(duration_ns, static_cast<uint64_t>(std::llround(sec * 1e9)));
    const uint64_t target_ns = toMonoNs(sec);
    // The seek snapshot already includes events at the requested time.
    auto it = std::ranges::upper_bound(all_events_, target_ns, {}, &CanEvent::mono_ns);
    idx = std::distance(all_events_.begin(), it);
    emit seekedTo(sec);
    waitForSeekFinished();
    reanchor();
  };

  applySeek(0.0);

  while (!QThread::currentThread()->isInterruptionRequested()) {
    double requested = seek_to_.exchange(-1.0);
    if (requested >= 0.0) {
      applySeek(requested);
      continue;
    }

    // Block while paused — wakes on unpause, seek, or destruction
    if (paused_.load()) {
      std::unique_lock lk(pause_mutex_);
      pause_cv_.wait(lk, [&] {
        return !paused_.load() || seek_to_.load() >= 0.0 || QThread::currentThread()->isInterruptionRequested();
      });
      reanchor();
      continue;
    }

    if (idx >= all_events_.size()) {
      // End of file — block until seek or destruction
      std::unique_lock lk(pause_mutex_);
      pause_cv_.wait(lk, [&] { return seek_to_.load() >= 0.0 || QThread::currentThread()->isInterruptionRequested(); });
      continue;
    }

    // Re-anchor on speed change to avoid time discontinuity
    const float spd = std::max(speed_.load(), 0.001f);
    if (spd != prev_speed) {
      reanchor();
      prev_speed = spd;
    }

    // How far into the file we should be (in ns from begin_mono_ns_)
    const uint64_t file_time_ns = std::min(duration_ns, anchor_file_ns +
        static_cast<uint64_t>((nanos_since_boot() - anchor_wall_ns) * spd));
    position_ns = file_time_ns;
    const uint64_t event_file_ns = all_events_[idx]->mono_ns - begin_mono_ns_;

    if (event_file_ns > file_time_ns) {
      setPlaybackTime(position_ns / 1e9);
      uint64_t wait_ns = std::min<uint64_t>(static_cast<uint64_t>((event_file_ns - file_time_ns) / spd), 50'000'000ULL);
      QThread::usleep(wait_ns / 1000);
      continue;
    }

    while (idx < all_events_.size()) {
      if (all_events_[idx]->mono_ns - begin_mono_ns_ > file_time_ns) break;
      const CanEvent* e = all_events_[idx++];
      processNewMessage({e->src, e->address}, e->mono_ns, e->dat, e->size);
    }
    setPlaybackTime(position_ns / 1e9);
  }
}
