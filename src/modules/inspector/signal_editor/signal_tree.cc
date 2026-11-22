#include "signal_tree.h"

#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QScrollBar>

#include "signal_editor.h"
#include "signal_tree_delegate.h"

namespace {
constexpr auto kPinDragMime = "application/x-cabana-pinned-signal";
}

SignalTree::SignalTree(QWidget* parent, bool reorder_pins) : QTreeView(parent) {
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  setMouseTracking(true);
  setExpandsOnDoubleClick(true);
  setUniformRowHeights(true);
  setEditTriggers(QAbstractItemView::EditKeyPressed);
  if (reorder_pins) {
    viewport()->setAcceptDrops(true);
    pin_scroll_timer.setInterval(75);
    connect(&pin_scroll_timer, &QTimer::timeout, this, [this] {
      auto* bar = verticalScrollBar();
      const int edge = std::min(24, viewport()->height() / 4);
      const int y = pin_drop_pos.y();
      const int previous = bar->value();
      if (y < edge) bar->setValue(previous - bar->singleStep());
      else if (y >= viewport()->height() - edge) bar->setValue(previous + bar->singleStep());
      if (bar->value() != previous) {
        pin_drop_row = pinDropRow(pin_drop_pos);
        viewport()->update();
      }
    });
  }

  viewport()->setMouseTracking(true);
  viewport()->setAttribute(Qt::WA_AlwaysShowToolTips, true);
  setToolTipDuration(1000);

  // Use a distinctive background for the whole row containing a QSpinBox or QLineEdit
  QString nodeBgColor = palette().color(QPalette::AlternateBase).name(QColor::HexArgb);
  setStyleSheet(QString("QSpinBox{background-color:%1;border:none;} QLineEdit{background-color:%1;}").arg(nodeBgColor));
}

void SignalTree::dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QVector<int>& roles) {
  // Bypass the slow call to QTreeView::dataChanged.
  QAbstractItemView::dataChanged(topLeft, bottomRight, roles);
}

void SignalTree::leaveEvent(QEvent* event) {
  QTreeView::leaveEvent(event);
  viewport()->unsetCursor();
  updateHighlight(nullptr);
  static_cast<SignalTreeDelegate*>(itemDelegate())->clearHoverState();
}

void SignalTree::mouseMoveEvent(QMouseEvent* event) {
  if (dragged_pin.isValid() && (event->buttons() & Qt::LeftButton)) {
    if ((event->pos() - pin_press_pos).manhattanLength() >= QApplication::startDragDistance()) {
      auto* drag = new QDrag(this);
      auto* mime = new QMimeData;
      mime->setData(kPinDragMime, {});
      drag->setMimeData(mime);
      drag->exec(Qt::MoveAction);
      dragged_pin = QPersistentModelIndex{};
      pin_scroll_timer.stop();
      pin_drop_row = -1;
      viewport()->unsetCursor();
      viewport()->update();
    }
    return;
  }

  if (pinHandleAt(event->pos()).isValid()) viewport()->setCursor(Qt::OpenHandCursor);
  else viewport()->unsetCursor();
  QTreeView::mouseMoveEvent(event);

  const dbc::Signal* current_sig = nullptr;
  bool current_message = false;
  QModelIndex idx = indexAt(event->pos());

  if (idx.isValid()) {
    if (auto m = qobject_cast<SignalTreeModel*>(model())) {
      if (auto item = m->itemFromIndex(idx)) {
        current_sig = item->sig;
        current_message = item->id == m->messageId();
      }
    }
  } else {
    static_cast<SignalTreeDelegate*>(itemDelegate())->clearHoverState();
  }

  updateHighlight(current_sig, current_message);
}

QModelIndex SignalTree::pinHandleAt(const QPoint& point) const {
  const auto index = indexAt(point);
  return index.data(PinDragHandleRole).toBool() ? index : QModelIndex{};
}

void SignalTree::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    dragged_pin = pinHandleAt(event->pos());
    if (dragged_pin.isValid()) {
      pin_press_pos = event->pos();
      viewport()->setCursor(Qt::ClosedHandCursor);
      return;
    }
  }
  QTreeView::mousePressEvent(event);
}

void SignalTree::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && dragged_pin.isValid()) {
    dragged_pin = QPersistentModelIndex{};
    viewport()->setCursor(Qt::OpenHandCursor);
    return;
  }
  QTreeView::mouseReleaseEvent(event);
}

int SignalTree::pinDropRow(const QPoint& point) const {
  const auto index = indexAt(QPoint(viewport()->width() - 1, point.y()));
  if (!index.isValid()) return model()->rowCount();
  auto parent = index;
  while (parent.parent().isValid()) parent = parent.parent();
  const bool below = index.parent().isValid() || point.y() > visualRect(index).center().y();
  return parent.row() + below;
}

void SignalTree::dragEnterEvent(QDragEnterEvent* event) {
  if (event->source() == this && dragged_pin.isValid() && event->mimeData()->hasFormat(kPinDragMime)) {
    event->acceptProposedAction();
  }
}

void SignalTree::dragMoveEvent(QDragMoveEvent* event) {
  if (event->source() != this || !dragged_pin.isValid()) return;
  pin_drop_pos = event->position().toPoint();
  pin_drop_row = pinDropRow(pin_drop_pos);
  if (!pin_scroll_timer.isActive()) pin_scroll_timer.start();
  viewport()->update();
  event->acceptProposedAction();
}

void SignalTree::dragLeaveEvent(QDragLeaveEvent* event) {
  pin_scroll_timer.stop();
  pin_drop_row = -1;
  viewport()->update();
  event->accept();
}

void SignalTree::dropEvent(QDropEvent* event) {
  pin_scroll_timer.stop();
  pin_drop_row = -1;
  viewport()->update();
  if (event->source() != this || !dragged_pin.isValid()) return;
  const int from = dragged_pin.row();
  const int destination = pinDropRow(event->position().toPoint());
  const int to = destination - (destination > from);
  static_cast<SignalTreeModel*>(model())->movePinnedSignal(from, to);
  event->acceptProposedAction();
}

void SignalTree::updateHighlight(const dbc::Signal* sig, bool current_message) {
  if (sig != last_sig || current_message != last_current_message) {
    last_sig = sig;
    last_current_message = current_message;
    emit highlightRequested(sig, current_message);
  }
}

void SignalTree::paintEvent(QPaintEvent* event) {
  QTreeView::paintEvent(event);

  if (pin_drop_row >= 0) {
    auto index = model()->index(pin_drop_row, SignalTreeModel::NameColumn);
    int y;
    if (index.isValid()) {
      y = visualRect(index).top();
    } else {
      index = model()->index(model()->rowCount() - 1, SignalTreeModel::NameColumn);
      for (auto next = indexBelow(index); next.isValid(); next = indexBelow(index)) index = next;
      y = visualRect(index).bottom();
    }
    QPainter p(viewport());
    p.setPen(QPen(palette().color(QPalette::Highlight), 2));
    p.drawLine(0, y, viewport()->width(), y);
  }

  if (!model() || model()->rowCount(rootIndex()) > 0) return;

  QPainter p(viewport());
  p.setPen(palette().color(QPalette::PlaceholderText));

  QFont font = p.font();
  font.setBold(true);
  p.setFont(font);
  p.drawText(viewport()->rect().adjusted(0, -15, 0, -15), Qt::AlignCenter, tr("No Signals Defined"));

  font.setBold(false);
  p.setFont(font);
  p.drawText(viewport()->rect().adjusted(20, 15, -20, 15), Qt::AlignCenter | Qt::TextWordWrap,
             tr("Drag bits in the binary view above to create a signal."));
}
