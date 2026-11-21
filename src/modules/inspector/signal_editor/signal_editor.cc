#include "signal_editor.h"

#include <QApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "core/commands/commands.h"
#include "core/streams/custom_usb_serial_stream.h"
#include "modules/settings/settings.h"
#include "modules/streams/custom_usb_serial.h"
#include "modules/system/stream_manager.h"
#include "widgets/panel_splitter.h"

SignalEditor::SignalEditor(ChartsPanel* charts, QWidget* parent) : QFrame(parent) {
  setFrameStyle(QFrame::NoFrame);
  tree = new SignalTree(this);
  model = new SignalTreeModel(this);
  pinned_tree = new SignalTree(this);
  pinned_model = new SignalTreeModel(this, true);

  configureTree(tree, model, charts);
  configureTree(pinned_tree, pinned_model, charts);
  delegate = static_cast<SignalTreeDelegate*>(tree->itemDelegate());
  auto* header = tree->header();
  connect(header, &QHeaderView::sectionResized, this, [this](int section, int, int) {
    if (section == SignalTreeModel::NameColumn) nameColumnManuallySized = true;
    if (section != SignalTreeModel::ValueColumn) pinned_tree->setColumnWidth(section, tree->columnWidth(section));
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

  auto* splitter = new PanelSplitter(Qt::Vertical, this);
  pinned_section = new QWidget(splitter);
  auto* pins_layout = new QVBoxLayout(pinned_section);
  pins_layout->setContentsMargins(0, 4, 0, 0);
  auto* title = new QLabel(tr("Pinned Signals"), pinned_section);
  title->setContentsMargins(4, 0, 4, 0);
  pins_layout->addWidget(title);
  pins_layout->addWidget(pinned_tree);
  splitter->addWidget(tree);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({150, 450});
  main_layout->addWidget(splitter);

  updateToolBar();
  setupConnections();
  pinned_model->rebuild();
}

void SignalEditor::configureTree(SignalTree* view, SignalTreeModel* tree_model, ChartsPanel* charts) {
  view->setModel(tree_model);
  auto* row_delegate = new SignalTreeDelegate(view);
  view->setItemDelegate(row_delegate);
  auto* header = view->header();
  const int row_height = row_delegate->sizeHint(QStyleOptionViewItem{}, {}).height();
  view->setMinimumHeight(header->sizeHint().height() + row_height + view->frameWidth() * 2);
  header->setMinimumSectionSize(100);
  header->setSectionResizeMode(SignalTreeModel::NameColumn, QHeaderView::Interactive);
  header->setSectionResizeMode(SignalTreeModel::HistoryColumn, QHeaderView::Interactive);
  header->setSectionResizeMode(SignalTreeModel::ValueColumn, QHeaderView::Stretch);

  view->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(view, &QWidget::customContextMenuRequested, this, [this, view, tree_model](const QPoint& point) {
    const auto clicked = view->indexAt(point);
    const QPersistentModelIndex property_index(clicked);
    auto index = clicked;
    while (index.parent().isValid()) index = index.parent();
    if (!index.isValid()) return;
    const auto* item = tree_model->itemFromIndex(index);
    if (!item || !item->sig) return;
    const MessageId message_id = item->id;
    const auto key = message_id.toString() + "|" + item->sig->name;
    const bool pinned = settings.pinned_signals.contains(key);
    QMenu menu;
    auto* action = menu.addAction(pinned ? tr("Unpin signal") : tr("Pin signal"));
    QAction* edit = nullptr;
    if (property_index.parent().isValid() && tree_model->flags(property_index).testFlag(Qt::ItemIsEditable))
      edit = menu.addAction(tr("Edit property…"));
    QPointer<CustomUsbSerialStream> usb = dynamic_cast<CustomUsbSerialStream*>(StreamManager::stream());
    auto* send = usb ? menu.addAction(tr("Send Signal Value…")) : nullptr;
    const QString signal_name = item->sig->name;
    auto* selected = menu.exec(view->viewport()->mapToGlobal(point));
    if (send && selected == send && usb) {
      CustomUsbSerialWidget::showSendSignalDialog(this, usb, message_id, signal_name);
    }
    if (edit && selected == edit && property_index.isValid()) view->edit(property_index);
    if (selected == action) {
      if (pinned) settings.pinned_signals.removeAll(key);
      else settings.pinned_signals.append(key);
      pinned_model->rebuild();
      tree->viewport()->update();
    }
  });

  connect(tree_model, &QAbstractItemModel::modelAboutToBeReset, row_delegate, &SignalTreeDelegate::clearHoverState);
  connect(tree_model, &QAbstractItemModel::rowsAboutToBeRemoved, row_delegate, &SignalTreeDelegate::clearHoverState);
  connect(view, &SignalTree::highlightRequested, this, [this](const dbc::Signal* sig, bool current_message) {
    emit highlight(current_message ? sig : nullptr);
  });
  connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { updateState(); });
  connect(view->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { updateState(); });
  connect(charts, &ChartsPanel::seriesChanged, tree_model,
          [tree_model, charts] { tree_model->updateChartedSignals(charts->getChartedSignals()); });
  connect(row_delegate, &SignalTreeDelegate::removeRequested, this,
          [](const MessageId& id, const dbc::Signal* sig) { UndoStack::push(new RemoveSigCommand(id, sig)); });
  connect(row_delegate, &SignalTreeDelegate::plotRequested, this, &SignalEditor::showChart);
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

void SignalEditor::setupConnections() {
  for (auto* view : {tree, pinned_tree}) {
    auto* other_selection = (view == tree ? pinned_tree : tree)->selectionModel();
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, other_selection,
            [other_selection](const QItemSelection& selected) {
              if (!selected.isEmpty()) other_selection->clear();
            });
  }
  connect(filter_edit, &DebouncedLineEdit::debouncedTextEdited, model, &SignalTreeModel::setFilter);
  connect(sparkline_range_slider, &QSlider::valueChanged, this, &SignalEditor::setSparklineRange);
  connect(collapse_btn, &QPushButton::clicked, tree, &QTreeView::collapseAll);
  connect(model, &QAbstractItemModel::modelReset, this, &SignalEditor::rowsChanged);
  connect(model, &QAbstractItemModel::rowsRemoved, this, &SignalEditor::rowsChanged);
  connect(model, &QAbstractItemModel::rowsInserted, this, &SignalEditor::rowsChanged);
  const auto update_pinned_section = [this] {
    pinned_section->setVisible(pinned_model->rowCount() > 0);
    updateColumnWidths();
  };
  connect(pinned_model, &QAbstractItemModel::modelReset, this, update_pinned_section);
  connect(pinned_model, &QAbstractItemModel::rowsRemoved, this, update_pinned_section);
  connect(collapse_btn, &QPushButton::clicked, pinned_tree, &QTreeView::collapseAll);
  connect(GetDBC(), &dbc::Manager::signalAdded, this, &SignalEditor::handleSignalAdded);
  connect(GetDBC(), &dbc::Manager::signalUpdated, this, [this] { updateState(); });

  auto* extra_info_shortcut = new QShortcut(QKeySequence(Qt::Key_V), this);
  extra_info_shortcut->setAutoRepeat(false);
  extra_info_shortcut->setEnabled(false);
  for (auto* view : {tree, pinned_tree}) {
    connect(view, &SignalTree::highlightRequested, extra_info_shortcut,
            [extra_info_shortcut](const dbc::Signal* sig) { extra_info_shortcut->setEnabled(sig != nullptr); });
  }
  connect(extra_info_shortcut, &QShortcut::activated, this, [this] {
    for (auto* view : {tree, pinned_tree}) {
      if (!view->isVisible()) continue;
      const QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos());
      const QModelIndex hovered = view->indexAt(pos);
      if (!view->viewport()->rect().contains(pos) || !hovered.isValid()) continue;
      auto* tree_model = static_cast<SignalTreeModel*>(view->model());
      auto* row_delegate = static_cast<SignalTreeDelegate*>(view->itemDelegate());
      row_delegate->openValueTable(view, tree_model->valueTableIndex(hovered), true);
      return;
    }
  });
}

void SignalEditor::setMessage(const MessageId& id) {
  filter_edit->clear();
  model->setMessage(id);
  pinned_model->setMessage(id);
  tree->scrollToTop();
}

void SignalEditor::clearMessage() {
  setMessage(MessageId());
}

void SignalEditor::updateState(const std::set<MessageId>* msgs) {
  // Skip update if the widget is hidden or collapsed
  if (!isVisible() || height() == 0 || width() == 0) return;

  for (auto* view : {tree, pinned_tree}) {
    auto* tree_model = static_cast<SignalTreeModel*>(view->model());
    if (!view->isVisible() || tree_model->rowCount() == 0) continue;
    auto [first, last] = visibleSignalRange(view);
    tree_model->updateValues(msgs);
    if (!first.isValid() || !last.isValid()) continue;
    const int width = view->columnWidth(SignalTreeModel::HistoryColumn) - 2 * delegate->kPadding;
    tree_model->updateSparklines(first.row(), last.row(), QSize(width, delegate->kBtnSize));
  }
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
  pinned_model->resetSparklines();
  updateState();
}

void SignalEditor::handleSignalAdded(MessageId id, const dbc::Signal* sig) {
  if (id.address == model->messageId().address) {
    selectSignal(sig);
  }
}

std::pair<QModelIndex, QModelIndex> SignalEditor::visibleSignalRange(SignalTree* view) const {
  auto topLevelIndex = [](QModelIndex index) {
    while (index.isValid() && index.parent().isValid()) index = index.parent();
    return index;
  };

  const auto viewport_rect = view->viewport()->rect();
  QModelIndex first_visible = view->indexAt(viewport_rect.topLeft());
  if (first_visible.parent().isValid()) {
    first_visible = topLevelIndex(first_visible);
    first_visible = first_visible.siblingAtRow(first_visible.row() + 1);
  }

  QModelIndex last_visible = topLevelIndex(view->indexAt(viewport_rect.bottomRight()));
  if (!last_visible.isValid()) {
    last_visible = view->model()->index(view->model()->rowCount() - 1, 0);
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
  for (int row = 0; row < pinned_model->rowCount(); ++row) {
    const auto* item = pinned_model->itemFromIndex(pinned_model->index(row, SignalTreeModel::NameColumn));
    width = std::max(width, delegate->nameColumnWidth(item->sig) + tree->indentation());
  }
  return width;
}

void SignalEditor::updateColumnWidths() {
  const QSignalBlocker blocker(tree->header());
  const auto* scrollbar = tree->verticalScrollBar();
  const int available_width = width() - (scrollbar->isVisible() ? scrollbar->width() : 0);
  if (!nameColumnManuallySized && (model->rowCount() > 0 || pinned_model->rowCount() > 0)) {
    const int limit = std::max(150, available_width / 2);
    tree->setColumnWidth(SignalTreeModel::NameColumn, std::min(signalNameWidth(), limit));
  }
  if (!historyColumnManuallySized) {
    const int remaining_width = available_width - tree->columnWidth(SignalTreeModel::NameColumn);
    tree->setColumnWidth(SignalTreeModel::HistoryColumn,
                         std::max(tree->header()->minimumSectionSize(), remaining_width / 3));
  }
  for (int column : {SignalTreeModel::NameColumn, SignalTreeModel::HistoryColumn}) {
    pinned_tree->setColumnWidth(column, tree->columnWidth(column));
  }
  updateState();
}

void SignalEditor::resizeEvent(QResizeEvent* event) {
  QFrame::resizeEvent(event);
  if (event->oldSize().width() != event->size().width()) {
    updateColumnWidths();
  }
}
