#pragma once

#include <limits>

#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>

#include "core/dbc/dbc_manager.h"
#include "core/streams/abstract_stream.h"
#include "utils/segment_tree.h"
#include "utils/series_bounds.h"

// Define a small value of epsilon to compare double values
const float EPSILON = 0.000001;

enum class SeriesType { Line = 0, StepLine, Scatter };

class ChartSignal {
 public:
  MessageId msg_id;
  const dbc::Signal* sig = nullptr;
  QXYSeries* series = nullptr;
  std::vector<QPointF> vals;
  std::vector<QPointF> step_vals;
  QPointF track_pt{};
  double min_value = 0;
  double max_value = 0;


  ChartSignal(const MessageId& id, const dbc::Signal* s, QXYSeries* ser) : msg_id(id), sig(s), series(ser) {}
  void prepareData(const MessageEventsMap* msg_new_events, double min_x, double max_x);
  void updateRange(double main_x, double max_x);
  void updateSeries(SeriesType series_type, double min_x = -std::numeric_limits<double>::infinity(),
                    double max_x = std::numeric_limits<double>::infinity(), int pixel_width = 0);
  void updatePointsVisible(double sec_per_px);

 private:
  int rendered_width_ = 0;
  std::pair<double, double> rendered_range_{0, 0};
  bool series_dirty_ = true;
  QXYSeries* last_series_ = nullptr;
  SeriesType last_series_type_ = SeriesType::Line;
  SeriesBounds series_bounds;
  std::pair<double, double> last_range_{0, 0};
};

qreal niceNumber(qreal x, bool ceiling);
std::tuple<double, double, int> getNiceAxisNumbers(qreal min, qreal max, int tick_count);
