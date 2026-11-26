#pragma once

#include <QComboBox>

#include "abstract.h"

class CustomUsbSerialStream;
struct MessageId;

class CustomUsbSerialWidget : public AbstractStreamWidget {
 public:
  explicit CustomUsbSerialWidget(QWidget* parent = nullptr);
  AbstractStream* open() override;
  static void showSendSignalDialog(QWidget* parent, CustomUsbSerialStream* stream,
                                   const MessageId& id, const QString& signal_name);
  static void showFirmwareLogDialog(QWidget* parent, CustomUsbSerialStream* stream);
  static void showDeviceMetricsDialog(QWidget* parent, CustomUsbSerialStream* stream);
  static void showSendFrameDialog(QWidget* parent, CustomUsbSerialStream* stream);

 private:
  void refreshDevices();
  QComboBox* port_edit_;
};
