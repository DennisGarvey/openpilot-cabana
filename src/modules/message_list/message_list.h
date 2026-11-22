#pragma once

#include <optional>

#include "message_delegate.h"
#include "message_header.h"
#include "message_model.h"
#include "message_table.h"

class QCheckBox;
class QMenu;
class QPushButton;
class ToolButton;

class MessageList : public QWidget {
  Q_OBJECT

 public:
  MessageList(QWidget* parent);
  QByteArray saveHeaderState() const { return table_->header()->saveState(); }
  bool restoreHeaderState(const QByteArray& state) const { return table_->header()->restoreState(state); }
  void suppressHighlighted(bool suppress);
  void selectMessage(const MessageId& message_id);
  void clearMessageSelection();

 signals:
  void msgSelectionChanged(const MessageId& message_id);
  void titleChanged(const QString& title);
  void nameFilterChanged(const QString& text);

 protected:
  // Construction helpers
  QWidget* createToolBar();
  void setupConnections();
  // State management
  void resetState();
  void updateTitle();
  void saveViewport();
  void restoreViewport();
  // Event handlers
  void onCurrentChanged(const QModelIndex& current);
  void syncPinnedSelection();
  void onHeaderContextMenuRequested(const QPoint& pos);
  void onMenuAboutToShow();

  // Core model/view
  MessageModel* model_;
  MessageTable* table_;
  MessageHeader* header_;
  MessageDelegate* delegate_;
  MessageModel* pinned_model_;
  MessageTable* pinned_table_;
  QWidget* pinned_section_;
  std::optional<MessageId> current_msg_id_;
  std::optional<MessageId> top_msg_id_;
  int vertical_scroll_ = 0;
  int horizontal_scroll_ = 0;
  int top_offset_ = 0;
  // Toolbar widgets
  ToolButton* mute_active_btn_;
  ToolButton* unmute_all_btn_;
  QCheckBox* suppress_defined_signals_;
  QMenu* menu_;
};
