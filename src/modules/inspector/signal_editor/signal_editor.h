#pragma once

#include <QLabel>
#include <QMouseEvent>
#include <QSlider>
#include <memory>
#include <set>
#include <utility>

#include "modules/charts/charts_panel.h"
#include "modules/charts/sparkline.h"
#include "signal_tree.h"
#include "signal_tree_delegate.h"
#include "signal_tree_model.h"
#include "widgets/debounced_line_edit.h"

class SignalEditor : public QFrame {
  Q_OBJECT

 public:
  SignalEditor(ChartsPanel* charts, QWidget* parent);
  void setMessage(const MessageId& id);
  void clearMessage();
  void selectSignal(const dbc::Signal* sig, bool expand = false);
  void updateState(const std::set<MessageId>* msgs = nullptr);
  SignalTreeModel* model = nullptr;

 signals:
  void highlight(const dbc::Signal* sig);
  void showChart(const MessageId& id, const dbc::Signal* sig, bool show, bool merge);

 private:
  QWidget* createToolbar();
  void setupConnections();
  void rowsChanged();
  void configureTree(SignalTree* view, SignalTreeModel* tree_model, ChartsPanel* charts);
  void resizeEvent(QResizeEvent* event) override;
  void updateToolBar();
  void setSparklineRange(int value);
  void handleSignalAdded(MessageId id, const dbc::Signal* sig);
  void updateColumnWidths();
  int signalNameWidth() const;
  std::pair<QModelIndex, QModelIndex> visibleSignalRange(SignalTree* view) const;

  SignalTree* tree;
  SignalTree* pinned_tree;
  SignalTreeModel* pinned_model;
  QWidget* pinned_section;
  QLabel* sparkline_label;
  QSlider* sparkline_range_slider;
  DebouncedLineEdit* filter_edit;
  QLabel* signal_count_lb;
  SignalTreeDelegate* delegate;
  ToolButton* collapse_btn;
  bool nameColumnManuallySized = false;
  bool historyColumnManuallySized = false;
};
