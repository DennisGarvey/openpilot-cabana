#include "signal_editor.h"

#include <QApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "core/commands/commands.h"
#include "modules/settings/settings.h"

SignalEditor::SignalEditor(ChartsPanel* charts, QWidget* parent) : QFrame(parent) {
  setFrameStyle(QFrame::NoFrame);
  tree = new SignalTree(this);
  tree->setModel(model = new SignalTreeModel(this));
  tree->setItemDelegate(delegate = new SignalTreeDelegate(this));
  tree->setMinimumHeight(300);

  QHeaderView* header = tree->header();
  header->setCascadingSectionResizes(false);
  header->setMinimumSectionSize(100);
  header->setStretchLastSection(true);
  header->setSectionResizeMode(SignalTreeModel::NameColumn, QHeaderView::Interactive);
  header->setSectionResizeMode(SignalTreeModel::HistoryColumn, QHeaderView::Interactive);
  header->setSectionResizeMode(SignalTreeModel::ValueColumn, QHeaderView::Stretch);
  connect(header, &QHeaderView::sectionResized, this, [this](int section, int, int) {
    if (section == SignalTreeModel::NameColumn) nameColumnManuallySized = true;
    if (section == SignalTreeModel::HistoryColumn) {
      historyColumnManuallySized = true;
      updateState();
    }
  });
  connect(header, &QHeaderView::sectionHandleDoubleClicked, this, [this](int section) {
    if (section == SignalTreeModel::NameColumn) {
      nameColumnManuallySized = true;
      tree->setColumnWidth(section, signalNameWidth());
    } else if (section == SignalTreeModel::HistoryColumn) {
      historyColumnManuallySized = false;
      updateColumnWidths();
    }
  });

  QVBoxLayout* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);
  main_layout->addWidget(createToolbar());

  QFrame* line = new QFrame(this);
  line->setFrameStyle(QFrame::HLine | QFrame::Sunken);
  main_layout->addWidget(line);

  main_layout->addWidget(tree);

  updateToolBar();
  setupConnections(charts);

  setWhatsThis(tr(R"(
    <b>Signal view</b><br />
    <!-- TODO: add descprition here -->
  )"));
}

QWidget* SignalEditor::createToolbar() {
  QWidget* toolbar = new QWidget(this);
  QHBoxLayout* hl = new QHBoxLayout(toolbar);
  hl->setContentsMargins(4, 4, 4, 4);

  hl->addWidget(signal_count_lb = new QLabel());
  filter_edit = new DebouncedLineEdit(this);

  QRegularExpression re("\\S+");
  filter_edit->setValidator(new QRegularExpressionValidator(re, this));
  filter_edit->setClearButtonEnabled(true);
  filter_edit->setPlaceholderText(tr("Filter signal..."));
  filter_edit->setToolTip(tr("Filter signals by name"));
  hl->addWidget(filter_edit, 0, Qt::AlignCenter);
  hl->addStretch(1);

  // WARNING: increasing the maximum range can result in severe performance degradation.
  // 30s is a reasonable value at present.
  const int max_range = 30;  // 30s
  settings.sparkline_range = std::clamp(settings.sparkline_range, 1, max_range);
  hl->addWidget(sparkline_label = new QLabel());
  hl->addWidget(sparkline_range_slider = new QSlider(Qt::Horizontal, this));
  sparkline_range_slider->setMinimumWidth(100);
  sparkline_range_slider->setRange(1, max_range);
  sparkline_range_slider->setValue(settings.sparkline_range);
  sparkline_range_slider->setToolTip(tr("Adjust sparkline history duration"));

  collapse_btn = new ToolButton("fold-vertical", tr("Collapse All"));
  hl->addWidget(collapse_btn);

  return toolbar;
}

void SignalEditor::setupConnections(ChartsPanel* charts) {
  connect(filter_edit, &DebouncedLineEdit::debouncedTextEdited, model, &SignalTreeModel::setFilter);
  connect(sparkline_range_slider, &QSlider::valueChanged, this, &SignalEditor::setSparklineRange);
  connect(collapse_btn, &QPushButton::clicked, tree, &QTreeView::collapseAll);
  connect(model, &QAbstractItemModel::modelReset, this, &SignalEditor::rowsChanged);
  connect(model, &QAbstractItemModel::rowsRemoved, this, &SignalEditor::rowsChanged);
  connect(model, &QAbstractItemModel::rowsInserted, this, &SignalEditor::rowsChanged);
  connect(model, &QAbstractItemModel::modelAboutToBeReset, delegate, &SignalTreeDelegate::clearHoverState);
  connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, delegate, &SignalTreeDelegate::clearHoverState);
  connect(GetDBC(), &dbc::Manager::signalAdded, this, &SignalEditor::handleSignalAdded);
  connect(GetDBC(), &dbc::Manager::signalUpdated, this, &SignalEditor::handleSignalUpdated);
  connect(tree, &SignalTree::highlightRequested, this, &SignalEditor::highlight);
  auto* extra_info_shortcut = new QShortcut(QKeySequence(Qt::Key_V), tree);
  extra_info_shortcut->setAutoRepeat(false);
  extra_info_shortcut->setEnabled(false);
  connect(tree, &SignalTree::highlightRequested, extra_info_shortcut,
          [extra_info_shortcut](const dbc::Signal* sig) { extra_info_shortcut->setEnabled(sig != nullptr); });
  connect(extra_info_shortcut, &QShortcut::activated, this, [this] {
    const QPoint pos = tree->viewport()->mapFromGlobal(QCursor::pos());
    if (!tree->viewport()->rect().contains(pos)) return;
    const QModelIndex hovered = tree->indexAt(pos);
    if (hovered.isValid()) {
      delegate->openValueTable(tree, model->valueTableIndex(model->itemFromIndex(hovered)->sig), true);
    }
  });
  connect(tree->verticalScrollBar(), &QScrollBar::valueChanged, [this]() { updateState(); });
  connect(tree->verticalScrollBar(), &QScrollBar::rangeChanged, [this]() { updateState(); });
  connect(charts, &ChartsPanel::seriesChanged, model,
          [this, charts]() { model->updateChartedSignals(charts->getChartedSignals()); });

  connect(delegate, &SignalTreeDelegate::removeRequested, this,
          [this](const dbc::Signal* sig) { UndoStack::push(new RemoveSigCommand(model->messageId(), sig)); });
  connect(delegate, &SignalTreeDelegate::plotRequested, this, [this](const dbc::Signal* sig, bool show, bool merge) {
    emit showChart(model->messageId(), sig, show, merge);
  });
}

void SignalEditor::setMessage(const MessageId& id) {
  filter_edit->clear();
  model->setMessage(id);
  tree->scrollToTop();
}

void SignalEditor::clearMessage() {
  filter_edit->clear();
  model->setMessage(MessageId());
  tree->scrollToTop();
}

void SignalEditor::updateState(const std::set<MessageId>* msgs) {
  // Skip update if the widget is hidden or collapsed
  if (!isVisible() || height() == 0 || width() == 0) return;

  if (model->rowCount() == 0 || (msgs && !msgs->count(model->messageId()))) return;
  const auto* last_msg = StreamManager::stream()->snapshot(model->messageId());

  auto [first_v, last_v] = visibleSignalRange();
  if (!first_v.isValid()) return;

  model->updateValues(last_msg);

  int spark_w = tree->columnWidth(SignalTreeModel::HistoryColumn) - 2 * delegate->kPadding;
  model->updateSparklines(last_msg, first_v.row(), last_v.row(), QSize(spark_w, delegate->kBtnSize));
}

void SignalEditor::rowsChanged() {
  updateToolBar();
  updateColumnWidths();
}

void SignalEditor::selectSignal(const dbc::Signal* sig, bool expand) {
  if (int row = model->signalRow(sig); row != -1) {
    auto idx = model->index(row, 0);
    if (expand) {
      tree->setExpanded(idx, !tree->isExpanded(idx));
    }
    tree->scrollTo(idx, QAbstractItemView::PositionAtTop);
    tree->setCurrentIndex(idx);
  }
}

void SignalEditor::updateToolBar() {
  signal_count_lb->setText(tr("Signals: %1").arg(model->rowCount()));
  sparkline_label->setText(utils::formatSeconds(settings.sparkline_range));
}

void SignalEditor::setSparklineRange(int value) {
  settings.sparkline_range = value;
  updateToolBar();

  // Clear history to prevent scaling artifacts when range changes drastically
  model->resetSparklines();
  updateState();
}

void SignalEditor::handleSignalAdded(MessageId id, const dbc::Signal* sig) {
  if (id.address == model->messageId().address) {
    selectSignal(sig);
  }
}

void SignalEditor::handleSignalUpdated(const dbc::Signal* sig) {
  if (int row = model->signalRow(sig); row != -1) updateState();
}

std::pair<QModelIndex, QModelIndex> SignalEditor::visibleSignalRange() {
  auto topLevelIndex = [](QModelIndex index) {
    while (index.isValid() && index.parent().isValid()) index = index.parent();
    return index;
  };

  const auto viewport_rect = tree->viewport()->rect();
  QModelIndex first_visible = tree->indexAt(viewport_rect.topLeft());
  if (first_visible.parent().isValid()) {
    first_visible = topLevelIndex(first_visible);
    first_visible = first_visible.siblingAtRow(first_visible.row() + 1);
  }

  QModelIndex last_visible = topLevelIndex(tree->indexAt(viewport_rect.bottomRight()));
  if (!last_visible.isValid()) {
    last_visible = model->index(model->rowCount() - 1, 0);
  }
  return {first_visible, last_visible};
}

int SignalEditor::signalNameWidth() const {
  auto* m = GetDBC()->msg(model->messageId());
  int width = 150;
  if (m) {
    for (const auto* sig : m->getSignals()) {
      width = std::max(width, delegate->nameColumnWidth(sig) + tree->indentation());
    }
  }
  return width;
}

void SignalEditor::updateColumnWidths() {
  const QSignalBlocker blocker(tree->header());
  const auto* scrollbar = tree->verticalScrollBar();
  const int available_width = width() - (scrollbar->isVisible() ? scrollbar->width() : 0);
  if (!nameColumnManuallySized && GetDBC()->msg(model->messageId())) {
    const int limit = std::max(150, available_width / 2);
    tree->setColumnWidth(SignalTreeModel::NameColumn, std::min(signalNameWidth(), limit));
  }
  if (!historyColumnManuallySized) {
    const int remaining_width = available_width - tree->columnWidth(SignalTreeModel::NameColumn);
    tree->setColumnWidth(SignalTreeModel::HistoryColumn,
                         std::max(tree->header()->minimumSectionSize(), remaining_width / 3));
  }
  updateState();
}

void SignalEditor::resizeEvent(QResizeEvent* event) {
  QFrame::resizeEvent(event);
  if (event->oldSize().width() != event->size().width()) {
    updateColumnWidths();
  }
}
