#include "message_list.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

#include "common.h"
#include "core/commands/commands.h"
#include "modules/settings/settings.h"
#include "modules/system/stream_manager.h"
#include "widgets/tool_button.h"

MessageList::MessageList(QWidget* parent) : QWidget(parent) {
  QVBoxLayout* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);

  table_ = new MessageTable(this);
  model_ = new MessageModel(this);
  header_ = new MessageHeader(this);
  delegate_ = new MessageDelegate(table_, CallerType::MessageList);
  menu_ = new QMenu(this);

  table_->setItemDelegate(delegate_);
  // Save the visible row before the view clears its item layout on a reset.
  connect(model_, &MessageModel::modelAboutToBeReset, this, &MessageList::saveViewport);
  table_->setModel(model_);  // Set model before configuring header sections
  table_->setHeader(header_);

  header_->blockSignals(true);
  header_->setSectionsMovable(true);
  header_->setContextMenuPolicy(Qt::CustomContextMenu);
  header_->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  header_->setStretchLastSection(true);

  header_->blockSignals(false);
  header_->setSortIndicator(MessageModel::Column::NAME, Qt::AscendingOrder);

  restoreHeaderState(settings.message_header_state);

  main_layout->addWidget(createToolBar());
  auto* splitter = new QSplitter(Qt::Vertical, this);
  splitter->setChildrenCollapsible(false);
  pinned_section_ = new QWidget(splitter);
  auto* pins_layout = new QVBoxLayout(pinned_section_);
  pins_layout->setContentsMargins(0, 0, 0, 0);
  pins_layout->addWidget(new QLabel(tr("Pinned Messages"), pinned_section_));
  pinned_table_ = new MessageTable(pinned_section_);
  pinned_model_ = new MessageModel(this, true);
  pinned_table_->setSortingEnabled(false);
  pinned_table_->setModel(pinned_model_);
  pinned_table_->setItemDelegate(new MessageDelegate(pinned_table_, CallerType::MessageList));
  pinned_table_->header()->setStretchLastSection(true);
  pins_layout->addWidget(pinned_table_);
  splitter->addWidget(table_);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({150, 450});
  main_layout->addWidget(splitter);

  setWhatsThis(tr(R"(
    <b>Message View</b><br/>
    <span style="color:gray">Byte color</span><br />
    <span style="color:gray;">■ </span> constant changing<br />
    <span style="color:blue;">■ </span> increasing<br />
    <span style="color:red;">■ </span> decreasing<br />
    <span style="color:gray">Shortcuts</span><br />
    Horizontal Scrolling: <span style="background-color:lightGray;color:gray">&nbsp;shift+wheel&nbsp;</span>
  )"));

  setupConnections();
  pinned_section_->hide();
  pinned_model_->rebuild();
}

QWidget* MessageList::createToolBar() {
  QWidget* toolbar = new QWidget(this);
  QHBoxLayout* layout = new QHBoxLayout(toolbar);
  layout->setContentsMargins(0, 4, 0, 0);
  layout->setSpacing(style()->pixelMetric(QStyle::PM_ToolBarItemSpacing));

  layout->addWidget(mute_active_btn_ = new ToolButton("ban"));
  mute_active_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  mute_active_btn_->setText(tr("Mute Active"));
  mute_active_btn_->setToolTip(
      tr("Mute Active Bits\n"
         "Silences currently changing bits to isolate new transitions."));
  layout->addWidget(unmute_all_btn_ = new ToolButton("refresh-ccw"));
  unmute_all_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  unmute_all_btn_->setText(tr("Unmute All"));
  unmute_all_btn_->setToolTip(
      tr("Unmute All Bits\n"
         "Restores highlighting for all data bits."));

  suppress_defined_signals_ = new QCheckBox(tr("Mute Defined"), this);
  suppress_defined_signals_->setFocusPolicy(Qt::NoFocus);
  suppress_defined_signals_->setToolTip(
      tr("Mute Defined Signals\n"
         "Silences bits already assigned to DBC signals to focus on unknown data."));
  layout->addWidget(suppress_defined_signals_);

  layout->addStretch(1);
  auto view_button = new ToolButton("ellipsis", tr("View..."));
  view_button->setMenu(menu_);
  view_button->setPopupMode(QToolButton::InstantPopup);
  view_button->setStyleSheet("QToolButton::menu-indicator { image: none; }");
  layout->addWidget(view_button);

  connect(mute_active_btn_, &ToolButton::clicked, this, [this]() { suppressHighlighted(true); });
  connect(unmute_all_btn_, &ToolButton::clicked, this, [this]() { suppressHighlighted(false); });
  connect(suppress_defined_signals_, &QCheckBox::stateChanged, this,
          [this]() { StreamManager::stream()->suppressDefinedSignals(suppress_defined_signals_->isChecked()); });

  suppressHighlighted(false);
  return toolbar;
}

void MessageList::setupConnections() {
  connect(menu_, &QMenu::aboutToShow, this, &MessageList::onMenuAboutToShow);
  for (auto* view : {table_, pinned_table_}) {
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this, [this, view](const QPoint& point) {
      const auto* model = static_cast<const MessageModel*>(view->model());
      const auto* item = model->getItem(view->indexAt(point));
      if (!item) return;
      const MessageId id = item->id;
      const bool pinned = settings.pinned_messages.contains(id.toString());
      QMenu menu(this);
      const auto* action = menu.addAction(pinned ? tr("Unpin message") : tr("Pin message"));
      if (menu.exec(view->viewport()->mapToGlobal(point)) == action) {
        pinned_model_->setPinned(id, !pinned);
        table_->update(model_->index(model_->getRowForMessageId(id), MessageModel::NAME));
      }
    });
  }
  const auto sync_columns = [this] { pinned_table_->header()->restoreState(header_->saveState()); };
  sync_columns();
  connect(header_, &QHeaderView::sectionResized, this, sync_columns);
  connect(header_, &QHeaderView::sectionMoved, this, sync_columns);
  connect(&StreamManager::instance(), &StreamManager::snapshotsUpdated, pinned_model_, &MessageModel::onSnapshotsUpdated);
  connect(GetDBC(), &dbc::Manager::DBCFileChanged, pinned_model_, &MessageModel::rebuild);
  connect(UndoStack::instance(), &QUndoStack::indexChanged, pinned_model_, &MessageModel::rebuild);
  connect(pinned_table_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MessageList::onCurrentChanged);
  connect(pinned_model_, &MessageModel::modelReset, this, [this] {
    pinned_section_->setVisible(pinned_model_->rowCount() > 0);
    syncPinnedSelection();
  });
  connect(header_, &MessageHeader::customContextMenuRequested, this, &MessageList::onHeaderContextMenuRequested);
  connect(table_->horizontalScrollBar(), &QScrollBar::valueChanged, header_, &MessageHeader::updateHeaderPositions);
  connect(&StreamManager::instance(), &StreamManager::snapshotsUpdated, model_, &MessageModel::onSnapshotsUpdated);
  connect(&StreamManager::instance(), &StreamManager::streamChanged, this, &MessageList::resetState);
  connect(GetDBC(), &dbc::Manager::DBCFileChanged, model_, &MessageModel::rebuild);
  connect(UndoStack::instance(), &QUndoStack::indexChanged, model_, &MessageModel::rebuild);
  connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MessageList::onCurrentChanged);
  connect(model_, &MessageModel::modelReset, this, [this]() {
    restoreViewport();
    updateTitle();
  });
}

void MessageList::suppressHighlighted(bool suppress) {
  int n = 0;
  if (suppress) {
    n = StreamManager::stream()->suppressHighlighted();
  } else {
    StreamManager::stream()->clearSuppressed();
  }
  unmute_all_btn_->setText(n > 0 ? tr("Unmute (%1 bits)").arg(n) : tr("Unmute All"));
  unmute_all_btn_->setEnabled(n > 0);
}

void MessageList::selectMessage(const MessageId& msg_id) {
  if (current_msg_id_ && *current_msg_id_ == msg_id) return;

  int row = model_->getRowForMessageId(msg_id);
  if (row != -1) {
    QModelIndex index = model_->index(row, 0);
    table_->setCurrentIndex(index);
    table_->scrollTo(index, QAbstractItemView::PositionAtCenter);
  } else if (const int pin_row = pinned_model_->getRowForMessageId(msg_id); pin_row >= 0) {
    pinned_table_->setCurrentIndex(pinned_model_->index(pin_row, 0));
  }
}

void MessageList::saveViewport() {
  const auto* vertical_bar = table_->verticalScrollBar();
  vertical_scroll_ = vertical_bar->value();
  horizontal_scroll_ = table_->horizontalScrollBar()->value();
  top_msg_id_.reset();
  if (vertical_scroll_ == vertical_bar->minimum()) return;

  const auto index = table_->indexAt(QPoint(0, 0));
  if (const auto* item = model_->getItem(index)) {
    top_msg_id_ = item->id;
    top_offset_ = table_->visualRect(index).top();
  }
}

void MessageList::restoreViewport() {
  if (current_msg_id_) {
    const int row = model_->getRowForMessageId(*current_msg_id_);
    if (row >= 0) table_->setCurrentIndex(model_->index(row, 0));
  }

  table_->doItemsLayout();
  auto* vertical_bar = table_->verticalScrollBar();
  const int top_row = top_msg_id_ ? model_->getRowForMessageId(*top_msg_id_) : -1;
  if (top_row >= 0) {
    table_->scrollTo(model_->index(top_row, 0), QAbstractItemView::PositionAtTop);
    if (table_->verticalScrollMode() == QAbstractItemView::ScrollPerPixel) {
      vertical_bar->setValue(vertical_bar->value() - top_offset_);
    }
  } else {
    vertical_bar->setValue(vertical_scroll_);
  }
  table_->horizontalScrollBar()->setValue(horizontal_scroll_);
}

void MessageList::clearMessageSelection() {
  current_msg_id_.reset();
  table_->selectionModel()->clear();
  pinned_table_->selectionModel()->clear();
}

void MessageList::resetState() {
  clearMessageSelection();
  model_->rebuild();
  pinned_model_->rebuild();

  unmute_all_btn_->setText(tr("Unmute All"));
  unmute_all_btn_->setEnabled(false);

  updateTitle();
  table_->scrollToTop();
}

void MessageList::updateTitle() {
  emit titleChanged(tr("%1 Messages (%2 DBC Messages, %3 Signals)")
                        .arg(model_->rowCount())
                        .arg(model_->getDbcMessageCount())
                        .arg(model_->getSignalCount()));
}

void MessageList::onCurrentChanged(const QModelIndex& current) {
  if (!current.isValid()) return;
  const auto* source_model = static_cast<const MessageModel*>(current.model());
  const auto* item = source_model->getItem(current);
  if (!item || (current_msg_id_ && item->id == *current_msg_id_)) return;

  current_msg_id_ = item->id;
  if (source_model == pinned_model_) {
    const QSignalBlocker blocker(table_->selectionModel());
    const int row = model_->getRowForMessageId(item->id);
    if (row >= 0) table_->setCurrentIndex(model_->index(row, 0));
    else table_->selectionModel()->clear();
  } else {
    syncPinnedSelection();
  }
  emit msgSelectionChanged(*current_msg_id_);
}

void MessageList::syncPinnedSelection() {
  const QSignalBlocker blocker(pinned_table_->selectionModel());
  const int row = current_msg_id_ ? pinned_model_->getRowForMessageId(*current_msg_id_) : -1;
  if (row >= 0) pinned_table_->setCurrentIndex(pinned_model_->index(row, 0));
  else pinned_table_->selectionModel()->clear();
}

void MessageList::onHeaderContextMenuRequested(const QPoint& pos) { menu_->exec(header_->mapToGlobal(pos)); }

void MessageList::onMenuAboutToShow() {
  menu_->clear();
  for (int i = 0; i < header_->count(); ++i) {
    int logical_index = header_->logicalIndex(i);
    auto action = menu_->addAction(model_->headerData(logical_index, Qt::Horizontal).toString(),
                                  [this, logical_index](bool checked) {
                                    header_->setSectionHidden(logical_index, !checked);
                                    pinned_table_->header()->setSectionHidden(logical_index, !checked);
                                  });
    action->setCheckable(true);
    action->setChecked(!header_->isSectionHidden(logical_index));
    // Can't hide the name column
    action->setEnabled(logical_index > 0);
  }
  menu_->addSeparator();

  auto* action = menu_->addAction(tr("Show inactive Messages"), model_, &MessageModel::setInactiveMessagesVisible);
  action->setCheckable(true);
  action->setChecked(model_->isInactiveMessagesVisible());
}
