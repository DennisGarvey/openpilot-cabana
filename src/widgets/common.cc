#include "common.h"

#include <QApplication>
#include <QStyle>
#include <QWheelEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <cmath>

#include "modules/settings/settings.h"
#include "tool_button.h"
#include "utils/util.h"

// TabBar

int TabBar::addTab(const QString& text) {
  int index = QTabBar::addTab(text);
  QToolButton* btn = new ToolButton("x", tr("Close Tab"));
  int width = style()->pixelMetric(QStyle::PM_TabCloseIndicatorWidth, nullptr, btn);
  int height = style()->pixelMetric(QStyle::PM_TabCloseIndicatorHeight, nullptr, btn);
  btn->setFixedSize({width, height});
  setTabButton(index, QTabBar::RightSide, btn);
  connect(btn, &QToolButton::clicked, this, &TabBar::closeTabClicked);
  return index;
}

void TabBar::closeTabClicked() {
  QObject* object = sender();
  for (int i = 0; i < count(); ++i) {
    if (tabButton(i, QTabBar::RightSide) == object) {
      emit tabCloseRequested(i);
      break;
    }
  }
}

namespace {
class TabScroller : public QScrollArea {
 public:
  TabScroller(TabBar* tabs, QWidget* parent) : QScrollArea(parent), tabs_(tabs) {
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWidgetResizable(false);
    viewport()->setAutoFillBackground(true);
    tabs_->setAutoFillBackground(true);
    tabs_->setUsesScrollButtons(false);
    setWidget(tabs_);
    tabs_->installEventFilter(this);
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
      QTimer::singleShot(0, this, [this, index] {
        resizeTabs();
        if (index >= 0) ensureVisible(tabs_->tabRect(index).center().x(), 0, 40, 0);
      });
    });
    resizeTabs();
  }
 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show ||
        event->type() == QEvent::ShowToParent || event->type() == QEvent::Hide) resizeTabs();
    if (event->type() == QEvent::Wheel) {
      auto* wheel = static_cast<QWheelEvent*>(event);
      const bool pixels = !wheel->pixelDelta().isNull();
      const QPoint delta = pixels ? wheel->pixelDelta() : wheel->angleDelta();
      const bool shift = wheel->modifiers().testFlag(Qt::ShiftModifier);
      const int distance = shift && delta.x() == 0 ? delta.y() : delta.x();
      auto* bar = horizontalScrollBar();
      // Do not forward to QScrollArea::wheelEvent: it sends the event back
      // through this filter, including zero-delta trackpad phase events.
      const int movement = pixels ? distance : qRound(distance / 120.0 * QApplication::wheelScrollLines() * bar->singleStep());
      bar->setValue(bar->value() - 2 * movement);
      wheel->accept();
      return true;
    }
    return QScrollArea::eventFilter(object, event);
  }
  void scrollContentsBy(int dx, int dy) override {
    QScrollArea::scrollContentsBy(dx, dy);
    // Repaint the whole strip after moving the native child widgets. Partial
    // backing-store copies can leave stale tab text and close buttons on macOS.
    viewport()->update();
    tabs_->update(QRect(tabs_->mapFrom(viewport(), QPoint()), viewport()->size()));
  }
 private:
  void resizeTabs() {
    const QSize hint = tabs_->sizeHint();
    tabs_->resize(hint);
    setFixedHeight(hint.height());
    setVisible(tabs_->count() > 1);
  }
  TabBar* tabs_;
};
}

QWidget* createScrollableTabBar(TabBar* tabs, QWidget* parent) { return new TabScroller(tabs, parent); }

// LogSlider

void LogSlider::setRange(double min, double max) {
  log_min = factor * std::log10(min);
  log_max = factor * std::log10(max);
  QSlider::setRange(min, max);
  setValue(QSlider::value());
}

int LogSlider::value() const {
  double v = log_min + (log_max - log_min) * ((QSlider::value() - minimum()) / double(maximum() - minimum()));
  return std::lround(std::pow(10, v / factor));
}

void LogSlider::setValue(int v) {
  double log_v = std::clamp(factor * std::log10(v), log_min, log_max);
  v = minimum() + (maximum() - minimum()) * ((log_v - log_min) / (log_max - log_min));
  QSlider::setValue(v);
}

// ElidedLabel

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  setMinimumWidth(1);
}

void ElidedLabel::paintEvent(QPaintEvent* event) {
  QPainter painter(this);
  QString elidedText = fontMetrics().elidedText(text(), Qt::ElideRight, width());
  painter.drawText(rect(), alignment(), elidedText);
}

QFrame* createVLine(QWidget* parent) {
  QFrame* v_line = new QFrame();
  v_line->setFixedHeight(16);
  v_line->setFrameShape(QFrame::VLine);
  v_line->setFrameShadow(QFrame::Plain);
  v_line->setStyleSheet("color: palette(mid);");
  return v_line;
}
