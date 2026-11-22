#include "chart_signal.h"

#include <array>
#include <cmath>
#include <QtCharts/QChart>
#include <QtCharts/QLegend>
#include <QtCharts/QLegendMarker>

#include "core/streams/custom_usb_serial_stream.h"
#include "modules/system/stream_manager.h"

static void appendCanEvents(const dbc::Signal* sig, const std::vector<const CanEvent*>& events,
                            std::vector<QPointF>& vals, std::vector<QPointF>& step_vals, SeriesBounds& series_bounds) {

  auto* can = StreamManager::stream();
  for (const CanEvent* e : events) {
    if (auto value = sig->parse(e->dat, e->size)) {
      const double ts = can->toSeconds(e->mono_ns);
      vals.emplace_back(ts, *value);

      series_bounds.addPoint(*value);

      if (!step_vals.empty()) step_vals.emplace_back(ts, step_vals.back().y());
      step_vals.emplace_back(ts, *value);
    }
  }
}

void ChartSignal::prepareData(const MessageEventsMap* msg_new_events, double min_x, double max_x) {
  // If no new events provided, we are doing a full refresh/clear
  if (!msg_new_events) {
    acknowledged_points_.clear();
    acknowledged_dirty_ = true;
    last_range_ = {-1, -1};
    vals.clear();
    step_vals.clear();
    series_bounds.clear();
    series_dirty_ = true;
  }

  auto* can = StreamManager::stream();
  auto events = msg_new_events ? msg_new_events : &can->eventsMap();
  std::vector<const CanEvent*> sent, merged;
  if (acknowledged_series) {
    const int source = msg_id.source + CustomUsbSerialStream::TxSourceOffset;
    if (auto tx = events->find(MessageId(source, msg_id.address)); tx != events->end()) {
      for (const auto* event : tx->second) {
        if (!event->size) continue;
        if (auto value = sig->parse(event->dat, event->size)) {
          sent.push_back(event);
          acknowledged_points_.emplace_back(can->toSeconds(event->mono_ns), *value);
          acknowledged_dirty_ = true;
        }
      }
    }
  }

  const auto received = events->find(msg_id);
  const std::vector<const CanEvent*>* trace = &sent;
  if (received != events->end()) {
    if (sent.empty()) {
      trace = &received->second;
    } else {
      merged.reserve(received->second.size() + sent.size());
      std::ranges::merge(received->second, sent, std::back_inserter(merged), {},
                         &CanEvent::mono_ns, &CanEvent::mono_ns);
      trace = &merged;
    }
  }
  if (trace->empty()) return;

  if (vals.empty() || can->toSeconds(trace->back()->mono_ns) > vals.back().x()) {
    appendCanEvents(sig, *trace, vals, step_vals, series_bounds);
  } else {
    std::vector<QPointF> tmp_vals, tmp_step_vals;
    appendCanEvents(sig, *trace, tmp_vals, tmp_step_vals, series_bounds);

    if (tmp_vals.empty()) return;

    auto insert_pos = std::ranges::lower_bound(vals, tmp_vals.front().x(), {}, &QPointF::x);
    vals.insert(insert_pos, tmp_vals.begin(), tmp_vals.end());

    auto step_insert_pos = std::ranges::lower_bound(step_vals, tmp_step_vals.front().x(), {}, &QPointF::x);
    step_vals.insert(step_insert_pos, tmp_step_vals.begin(), tmp_step_vals.end());

    // Rebuild the bounds cache to ensure hierarchy is correct after insertion
    series_bounds.clear();
    for (const auto& p : vals) series_bounds.addPoint(p.y());
  }

  series_dirty_ = true;
  last_range_ = {-1.0, -1.0};
  updateRange(min_x, max_x);
}

void ChartSignal::updateSeries(SeriesType series_type, double min_x, double max_x, int pixel_width) {
  const std::pair<double, double> range{min_x, max_x};
  if (acknowledged_series && (acknowledged_dirty_ || acknowledged_range_ != range)) {
    const auto first = std::ranges::lower_bound(acknowledged_points_, min_x, {}, &QPointF::x);
    const auto last = std::ranges::upper_bound(first, acknowledged_points_.end(), max_x, {}, &QPointF::x);
    acknowledged_series->replace(QList<QPointF>(first, last));
    acknowledged_range_ = range;
    acknowledged_dirty_ = false;
    if (auto* chart = acknowledged_series->chart()) {
      for (auto* marker : chart->legend()->markers(acknowledged_series))
        marker->setVisible(!acknowledged_points_.empty() && series->isVisible());
    }
  }
  if (!series_dirty_ && last_series_ == series && last_series_type_ == series_type && rendered_range_ == range && rendered_width_ == pixel_width) return;
  const auto& points = series_type == SeriesType::StepLine ? step_vals : vals;
  auto first = std::ranges::lower_bound(points, min_x, {}, &QPointF::x);
  auto last = std::ranges::upper_bound(points, max_x, {}, &QPointF::x);
  // Keep boundary neighbors so lines and steps cross the viewport correctly.
  if (first != points.begin()) --first;
  if (last != points.end()) ++last;
  QList<QPointF> rendered;
  if (series_type != SeriesType::Scatter && pixel_width > 0 && std::isfinite(min_x) && std::isfinite(max_x) &&
      max_x > min_x && std::distance(first, last) > pixel_width * 4) {
    // Retain first, extrema and last samples in time order for each pixel.
    // Raw values remain intact for inspection and zooming.
    rendered.reserve(pixel_width * 4 + 8);
    auto bucket = [&](const QPointF& p) {
      return std::clamp(std::floor((p.x() - min_x) * pixel_width / (max_x - min_x)), -1.0, double(pixel_width));
    };
    for (auto begin = first; begin != last;) {
      auto end = begin + 1, low = begin, high = begin;
      const auto column = bucket(*begin);
      while (end != last && bucket(*end) == column) {
        if (end->y() < low->y()) low = end;
        if (end->y() > high->y()) high = end;
        ++end;
      }
      std::array selected{begin, low, high, end - 1};
      std::sort(selected.begin(), selected.end());
      auto unique_end = std::unique(selected.begin(), selected.end());
      for (auto it = selected.begin(); it != unique_end; ++it) rendered.append(**it);
      begin = end;
    }
  } else {
    rendered = QList<QPointF>(first, last);
  }
  series->replace(rendered);
  rendered_width_ = pixel_width;
  rendered_range_ = range;
  series_dirty_ = false;
  last_series_ = series;
  last_series_type_ = series_type;
}

void ChartSignal::updateRange(double min_x, double max_x) {
  if (min_x == last_range_.first && max_x == last_range_.second) {
    return;
  }
  last_range_ = {min_x, max_x};

  min_value = std::numeric_limits<double>::max();
  max_value = std::numeric_limits<double>::lowest();
  const auto first = std::ranges::lower_bound(vals, min_x, {}, &QPointF::x);
  const auto last = std::ranges::upper_bound(first, vals.end(), max_x, {}, &QPointF::x);
  if (first != last) {
    const auto node = series_bounds.query(first - vals.begin(), last - vals.begin() - 1, vals);
    min_value = node.min;
    max_value = node.max;
  }
}

void ChartSignal::updatePointsVisible(double sec_per_px) {
  if (vals.size() < 2) return;

  // Average time between data points
  double avg_period = (vals.back().x() - vals.front().x()) / vals.size();

  if (dynamic_cast<QScatterSeries*>(series)) {
    // Scale dot size by DPR so it looks consistent on all screens
    qreal size = std::clamp(avg_period / sec_per_px / 2.0, 2.0, 8.0);
    static_cast<QScatterSeries*>(series)->setMarkerSize(size);
    // Hide points for scatter series to improve performance; markers are still visible
    series->setPointsVisible(false);
  } else {
    // Threshold: Hide points if they are closer than 15 logical pixels
    series->setPointsVisible(avg_period > (sec_per_px * 15.0));
  }
}

std::tuple<double, double, int> getNiceAxisNumbers(qreal min, qreal max, int tick_count) {
  qreal range = niceNumber((max - min), true);  // range with ceiling
  qreal step = niceNumber(range / (tick_count - 1), false);
  min = std::floor(min / step);
  max = std::ceil(max / step);
  tick_count = int(max - min) + 1;
  return {min * step, max * step, tick_count};
}

// nice numbers can be expressed as form of 1*10^n, 2* 10^n or 5*10^n
qreal niceNumber(qreal x, bool ceiling) {
  qreal z = std::pow(10, std::floor(std::log10(x))); //find corresponding number of the form of 10^n than is smaller than x
  qreal q = x / z; //q<10 && q>=1;
  if (ceiling) {
    if (q <= 1.0) q = 1;
    else if (q <= 2.0) q = 2;
    else if (q <= 5.0) q = 5;
    else q = 10;
  } else {
    if (q < 1.5) q = 1;
    else if (q < 3.0) q = 2;
    else if (q < 7.0) q = 5;
    else q = 10;
  }
  return q * z;
}
