#pragma once

#include <QComboBox>
#include <QLineEdit>

#include "abstract.h"
#include "core/streams/panda_stream.h"

class QCheckBox;
class QStackedWidget;
class QTabBar;

class PandaWidget : public AbstractStreamWidget {
  Q_OBJECT

 public:
  PandaWidget(QWidget* parent = nullptr);
  AbstractStream* open() override;

 private:
  void refreshSerials();
  void updateSerialConfig();

  QTabBar* transport = nullptr;
  QComboBox* serial_edit = nullptr;
  QStackedWidget* serial_options = nullptr;
  std::vector<QWidget*> fd_controls;
  QLineEdit* udp_host = nullptr;
  QLineEdit* udp_port = nullptr;
  QLineEdit* udp_local_port = nullptr;
  QCheckBox* fixed_receive_port = nullptr;
  PandaStreamConfig config = {};
};
