#include "charts_toolbar.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QSignalBlocker>
#include <QStyle>

#include "modules/settings/settings.h"

ChartsToolBar::ChartsToolBar(QWidget* parent) : QWidget(parent) {
  QHBoxLayout* hl = new QHBoxLayout(this);
  // Match Cabana standard header margins
  int h_margin = style()->pixelMetric(QStyle::PM_ToolBarItemMargin);
  hl->setContentsMargins(h_margin, 2, h_margin, 2);
  hl->setSpacing(style()->pixelMetric(QStyle::PM_ToolBarItemSpacing));

  createActions(hl);

  setChartDuration(settings.chart_range, true);
  setIsDocked(true);
  updateState(0);

  // Signal Connections
  connect(reset_zoom_btn, &QToolButton::clicked, this, &ChartsToolBar::zoomReset);
  connect(&settings, &Settings::changed, this, &ChartsToolBar::settingChanged);
  connect(range_slider, &QSlider::valueChanged, this, [this](int seconds) { setChartDuration(seconds, false); });
  connect(range_input, &QSpinBox::valueChanged, this, [this](int seconds) { setChartDuration(seconds, true); });
}

void ChartsToolBar::setChartDuration(int seconds, bool move_slider) {
  seconds = std::clamp(seconds, range_input->minimum(), range_input->maximum());
  const QSignalBlocker input_blocker(range_input);
  range_input->setValue(seconds);
  if (move_slider) {
    const QSignalBlocker slider_blocker(range_slider);
    range_slider->setValue(seconds);
  }
  if (settings.chart_range != seconds) {
    settings.chart_range = seconds;
    emit rangeChanged(seconds);
  }
}

void ChartsToolBar::createActions(QHBoxLayout* hl) {
  hl->addWidget(new_plot_btn = new ToolButton("plus", tr("New Chart")));
  hl->addWidget(new_tab_btn = new ToolButton("layer-plus", tr("New Tab")));

  hl->addWidget(title_label = new QLabel());
  title_label->setStyleSheet("font-weight: bold;");

  hl->addSpacing(4);
  hl->addWidget(createVLine(this), 0, Qt::AlignCenter);
  hl->addSpacing(4);

  // Column Menu (Preserving InstantPopup feature)
  columns_btn = new ToolButton("layout-grid");
  columns_btn->setPopupMode(QToolButton::InstantPopup);
  columns_btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  columns_btn->setToolTip(tr("Grid Layout\nSet number of columns."));
  createColumnMenu();
  hl->addWidget(columns_btn);

  // Chart Type Menu (Preserving InstantPopup feature)
  chart_type_btn = new ToolButton();
  chart_type_btn->setPopupMode(QToolButton::InstantPopup);
  chart_type_btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  chart_type_btn->setToolTip(tr("Chart Drawing Style\nChange how signals are visualized."));
  createTypeMenu();
  hl->addWidget(chart_type_btn);

  hl->addStretch(1);

  setupZoomControls(hl);

  hl->addWidget(createVLine(this), 0, Qt::AlignCenter);

  hl->addWidget(remove_all_btn = new ToolButton("eraser", tr("Remove All Charts")));
  hl->addWidget(dock_btn = new ToolButton("external-link"));
}

void ChartsToolBar::createTypeMenu() {
  QMenu* menu = new QMenu(this);
  struct ChartStyle {
    QString name;
    QString icon;
  };
  auto styles = std::array{
      ChartStyle{tr("Line"), "chart-line"},
      ChartStyle{tr("Step"), "chart-network"},
      ChartStyle{tr("Scatter"), "chart-scatter"},
  };

  for (int i = 0; i < styles.size(); ++i) {
    QAction* action = menu->addAction(utils::icon(styles[i].icon), styles[i].name);
    connect(action, &QAction::triggered, this, [=]() {
      settings.chart_series_type = i;
      chart_type_btn->setIcon(styles[i].icon);
      chart_type_btn->setText(styles[i].name);
      emit seriesTypeChanged(i);
    });
  }

  chart_type_btn->setMenu(menu);
  // Initialize with current icon and text
  chart_type_btn->setIcon(styles[settings.chart_series_type].icon);
  chart_type_btn->setText(styles[settings.chart_series_type].name);
}

void ChartsToolBar::createColumnMenu() {
  QMenu* menu = new QMenu(this);
  for (int i = 0; i < MAX_COLUMN_COUNT; ++i) {
    menu->addAction(QString::number(i + 1), [=, count = i + 1]() {
      settings.chart_column_count = count;
      columns_btn->setText(QString::number(count));
      emit columnCountChanged(count);
    });
  }
  columns_btn->setMenu(menu);
  columns_btn->setText(QString::number(settings.chart_column_count));
}

void ChartsToolBar::setupZoomControls(QHBoxLayout* hl) {
  const int max_seconds = std::max(1, settings.max_cached_minutes * 60);
  range_input = new QSpinBox(this);
  range_input->setRange(1, max_seconds);
  range_input->setSuffix(tr(" s"));
  range_input->setButtonSymbols(QAbstractSpinBox::NoButtons);
  range_input->setKeyboardTracking(false);
  range_input->setAccessibleName(tr("Chart duration"));
  range_input->setToolTip(tr("Type the chart window duration in seconds."));
  hl->addWidget(range_input);

  range_slider = new QSlider(Qt::Horizontal, this);
  range_slider->setFocusPolicy(Qt::NoFocus);
  range_slider->setToolTip(tr("Chart Range\nAdjust the time window of data displayed."));
  range_slider->setMaximumWidth(200);
  range_slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  range_slider->setRange(1, max_seconds);
  range_slider->setSingleStep(1);
  range_slider->setPageStep(60);
  hl->addWidget(range_slider, 2);

  zoom_undo_stack = new QUndoStack(this);

  // Undo/Redo Buttons
  hl->addWidget(undo_btn = new ToolButton("undo-2", tr("Undo last zoom")));
  hl->addWidget(redo_btn = new ToolButton("redo-2", tr("Redo last zoom")));

  undo_btn->setEnabled(zoom_undo_stack->canUndo());
  redo_btn->setEnabled(zoom_undo_stack->canRedo());

  connect(zoom_undo_stack, &QUndoStack::canUndoChanged, undo_btn, &QToolButton::setEnabled);
  connect(zoom_undo_stack, &QUndoStack::canRedoChanged, redo_btn, &QToolButton::setEnabled);
  connect(undo_btn, &QToolButton::clicked, zoom_undo_stack, &QUndoStack::undo);
  connect(redo_btn, &QToolButton::clicked, zoom_undo_stack, &QUndoStack::redo);

  // Reset Zoom (Text Beside Icon config)
  hl->addWidget(reset_zoom_btn = new ToolButton("refresh-ccw", tr("Reset Zoom")));
  reset_zoom_btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
}

void ChartsToolBar::updateState(int chart_count) {
  title_label->setText(tr("Charts: %1").arg(chart_count));
  columns_btn->setText(QString::number(settings.chart_column_count));
  auto* stream = StreamManager::stream();
  auto time_range = stream->timeRange();
  bool is_zoomed = time_range.has_value();
  range_input->setVisible(!is_zoomed);
  range_slider->setVisible(!is_zoomed);
  undo_btn->setVisible(is_zoomed);
  redo_btn->setVisible(is_zoomed);
  reset_zoom_btn->setVisible(is_zoomed);
  reset_zoom_btn->setText(
      is_zoomed ? tr("%1-%2").arg(time_range->first, 0, 'f', 2).arg(time_range->second, 0, 'f', 2)
                : "");
  remove_all_btn->setEnabled(chart_count > 0);
}

void ChartsToolBar::setIsDocked(bool docked) {
  is_docked = docked;
  dock_btn->setIcon(is_docked ? "external-link" : "dock");
  dock_btn->setToolTip(is_docked ? tr("Float Window") : tr("Dock Window"));
}

void ChartsToolBar::zoomReset() {
  StreamManager::stream()->setTimeRange(std::nullopt);
  zoom_undo_stack->clear();
}

void ChartsToolBar::settingChanged() {
  undo_btn->setIcon("undo-2");
  redo_btn->setIcon("redo-2");
  const int max_sec = std::max(1, settings.max_cached_minutes * 60);
  if (range_slider->maximum() != max_sec) {
    const QSignalBlocker slider_blocker(range_slider);
    const QSignalBlocker input_blocker(range_input);
    range_slider->setRange(1, max_sec);
    range_input->setRange(1, max_sec);
    setChartDuration(settings.chart_range, true);
  }
}
