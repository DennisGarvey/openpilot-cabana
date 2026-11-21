#include "message_table.h"

#include <QScrollBar>

#include <cmath>

#include "message_model.h"

MessageTable::MessageTable(QWidget* parent) : QTreeView(parent) {
  setSortingEnabled(true);
  sortByColumn(MessageModel::Column::NAME, Qt::AscendingOrder);
  setAllColumnsShowFocus(true);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  setItemsExpandable(false);
  setIndentation(0);
  setRootIsDecorated(false);
  setAlternatingRowColors(true);
  setUniformRowHeights(true);
}

void MessageTable::dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QVector<int>& roles) {
  // Bypass the slow call to QTreeView::dataChanged.
  // QTreeView::dataChanged will invalidate the height cache and that's what we don't need in MessageTable.
  QAbstractItemView::dataChanged(topLeft, bottomRight, roles);
}

void MessageTable::wheelEvent(QWheelEvent* event) {
  const bool has_pixels = !event->pixelDelta().isNull();
  const QPoint delta = has_pixels ? event->pixelDelta() : event->angleDelta();
  const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
  if (event->phase() == Qt::ScrollBegin || event->phase() == Qt::ScrollEnd) horizontal_scroll_remainder_ = 0.0;

  if (!shift && std::abs(delta.x()) <= std::abs(delta.y())) {
    QTreeView::wheelEvent(event);
    return;
  }

  const int horizontal = shift && std::abs(delta.y()) > std::abs(delta.x()) ? delta.y() : delta.x();
  auto* bar = horizontalScrollBar();
  if (!horizontal || (horizontal > 0 && bar->value() == bar->minimum()) ||
      (horizontal < 0 && bar->value() == bar->maximum())) {
    horizontal_scroll_remainder_ = 0.0;
    event->ignore();
    return;
  }

  constexpr double pixel_scale = 0.35;
  constexpr double pixels_per_wheel_step = 24.0;
  const double distance = horizontal * (has_pixels ? pixel_scale : pixels_per_wheel_step / 120.0);
  horizontal_scroll_remainder_ += distance;
  const int pixels = static_cast<int>(horizontal_scroll_remainder_);
  horizontal_scroll_remainder_ -= pixels;
  bar->setValue(bar->value() - pixels);
  event->accept();
}
