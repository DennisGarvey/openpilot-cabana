#include "custom_usb_serial.h"

#include <QApplication>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QVBoxLayout>

#include "core/streams/custom_usb_serial_stream.h"

CustomUsbSerialWidget::CustomUsbSerialWidget(QWidget* parent) : AbstractStreamWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->addStretch();
  auto* form = new QFormLayout;
  auto* ports = new QHBoxLayout;
  port_edit_ = new QComboBox;
  port_edit_->setEditable(true);
  ports->addWidget(port_edit_, 1);
  auto* refresh = new QPushButton(tr("Refresh"));
  ports->addWidget(refresh);
  form->addRow(tr("USB serial port"), ports);
  layout->addLayout(form);
  auto* hint =
      new QLabel(tr("Requires firmware implementing the Custom USB Serial protocol. View frames from both buses in listen-only mode. "
                    "Hover over USB statistics for connection details."));
  hint->setWordWrap(true);
  layout->addWidget(hint);
  layout->addStretch();
  setFocusProxy(port_edit_);
  connect(refresh, &QPushButton::clicked, this, &CustomUsbSerialWidget::refreshDevices);
  connect(port_edit_, &QComboBox::currentTextChanged, this,
          [this](const QString& port) { emit enableOpenButton(!port.isEmpty()); });
  refreshDevices();
}

void CustomUsbSerialWidget::refreshDevices() {
  const auto selected = port_edit_->currentText();
  port_edit_->clear();
  for (const auto& port : QSerialPortInfo::availablePorts()) {
    if (port.hasVendorIdentifier() && port.vendorIdentifier() == 0x303a) port_edit_->addItem(port.systemLocation());
  }
  if (!selected.isEmpty()) port_edit_->setCurrentText(selected);
}

AbstractStream* CustomUsbSerialWidget::open() {
  try {
    return new CustomUsbSerialStream(qApp, port_edit_->currentText());
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("Custom USB Serial"), QString::fromUtf8(e.what()));
    return nullptr;
  }
}
