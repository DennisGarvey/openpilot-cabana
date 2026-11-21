#pragma once

#include <QComboBox>

#include "abstract.h"

class CustomUsbSerialWidget : public AbstractStreamWidget {
 public:
  explicit CustomUsbSerialWidget(QWidget* parent = nullptr);
  AbstractStream* open() override;

 private:
  void refreshDevices();
  QComboBox* port_edit_;
};
