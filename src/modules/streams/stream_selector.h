#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QTabWidget>

#include "abstract.h"
#include "core/streams/abstract_stream.h"

class StreamSelector : public QDialog {
  Q_OBJECT

 public:
  StreamSelector(QWidget* parent = nullptr, const QString& initial_dbc = {});
  void addStreamWidget(AbstractStreamWidget* w, const QString& title);
  QString dbcFile() const { return dbc_file->text(); }
  AbstractStream* stream() const { return stream_; }

 private:
  AbstractStream* stream_ = nullptr;
  bool dbc_file_overridden_ = false;
  QLineEdit* dbc_file;
  QTabWidget* tab;
  QDialogButtonBox* btn_box;
};
