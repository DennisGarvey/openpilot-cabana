#pragma once
#include <QDialog>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <optional>

#include "core/dbc/dbc_message.h"

class QDialogButtonBox;
class QPushButton;

class ValueTableEditor : public QDialog {
 public:
  ValueTableEditor(const ValueTable& descriptions, QWidget* parent, const dbc::Signal* extra_info = nullptr);
  void highlightCurrentValue(std::optional<double> raw_value, bool reveal = false);
  ValueTable value_table;

 private:
  void addRow(const QString& val, const QString& desc);

  void setupConnections();
  void handleSave();
  struct Delegate : public QStyledItemDelegate {
    Delegate(QWidget* parent) : QStyledItemDelegate(parent) {}
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  };

  QTableWidget* table;
  QDialogButtonBox* btn_box;
  QPushButton* add_btn;
  QPushButton* remove_btn;
};
