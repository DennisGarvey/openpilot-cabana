#pragma once

#include <QPersistentModelIndex>
#include <QTimer>
#include <QTreeView>

#include "core/dbc/dbc_message.h"

struct SignalTree : public QTreeView {
  Q_OBJECT
 public:
  SignalTree(QWidget* parent, bool reorder_pins = false);
  void dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight,
                   const QVector<int>& roles = QVector<int>()) override;
  void leaveEvent(QEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;

 signals:
  void highlightRequested(const dbc::Signal* sig, bool current_message);

 private:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dragLeaveEvent(QDragLeaveEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  QModelIndex pinHandleAt(const QPoint& point) const;
  int pinDropRow(const QPoint& point) const;
  void updateHighlight(const dbc::Signal* sig, bool current_message = false);

  QPersistentModelIndex dragged_pin;
  QPoint pin_press_pos;
  QPoint pin_drop_pos;
  int pin_drop_row = -1;
  QTimer pin_scroll_timer;
  const dbc::Signal* last_sig = nullptr;
  bool last_current_message = false;
};
