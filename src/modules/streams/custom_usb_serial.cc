#include "custom_usb_serial.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "core/dbc/dbc_manager.h"
#include "core/streams/custom_usb_serial_protocol.h"
#include "core/streams/custom_usb_serial_stream.h"

namespace {

QByteArray signalMask(const dbc::Signal& signal, int data_size) {
  QByteArray mask(data_size, 0);
  for (int i = 0; i < signal.size; ++i) {
    const int bit = signal.getBitIndex(i);
    if (bit >= 0 && bit / 8 < mask.size()) mask[bit / 8] |= char(1u << (bit % 8));
  }
  return mask;
}

struct SendControls {
  SendControls(QDialog& dialog, QFormLayout& form, CustomUsbSerialStream* stream, bool allow_timed)
      : mode(new QComboBox(&dialog)), frequency(new QSpinBox(&dialog)), duration(new QDoubleSpinBox(&dialog)),
        send(new QPushButton(QObject::tr("Send Once"), &dialog)),
        cancel(new QPushButton(QObject::tr("Cancel Send"), &dialog)), allow_timed(allow_timed) {
    mode->addItem(QObject::tr("Once"));
    if (allow_timed) mode->addItems({QObject::tr("Fixed Rate"), QObject::tr("Automatic Retransmission")});
    frequency->setRange(1, DK_TX_LEGACY_MAX_HZ);
    frequency->setValue(5);
    frequency->setSuffix(QStringLiteral(" Hz"));
    duration->setDecimals(1);
    duration->setRange(DK_TX_MIN_DURATION_MS / 1000.0, DK_TX_MAX_DURATION_MS / 1000.0);
    duration->setSingleStep(0.1);
    duration->setValue(5);
    duration->setSuffix(QStringLiteral(" s"));
    send->setAutoDefault(false);
    cancel->setAutoDefault(false);
    form.addRow(QObject::tr("Mode"), mode);
    form.addRow(QObject::tr("Frequency"), frequency);
    form.addRow(QObject::tr("Duration"), duration);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(send);
    buttons->addWidget(cancel);
    form.addRow(buttons);

    auto update_mode = [this, &form] {
      form.setRowVisible(frequency, fixedRate());
      form.setRowVisible(duration, timed());
      send->setText(fixedRate() ? QObject::tr("Start Fixed Rate") :
                    (automatic() ? QObject::tr("Start Automatic Retransmission") : QObject::tr("Send Once")));
    };
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dialog, update_mode);
    QObject::connect(cancel, &QPushButton::clicked, stream, &CustomUsbSerialStream::cancelRepeat);
    QObject::connect(&dialog, &QDialog::finished, stream, &CustomUsbSerialStream::cancelRepeat);
    update_mode();
  }

  bool fixedRate() const { return mode->currentIndex() == 1; }
  bool automatic() const { return mode->currentIndex() == 2; }
  bool timed() const { return fixedRate() || automatic(); }

  std::optional<CustomUsbSerialStream::RepeatTiming> timing() const {
    if (!timed()) return std::nullopt;
    return CustomUsbSerialStream::RepeatTiming{automatic() ? 0u : static_cast<unsigned>(frequency->value()),
                                               static_cast<unsigned>(qRound(duration->value() * 1000)),
                                               automatic()};
  }

  void refresh(const CustomUsbSerialStream* stream, bool valid_input) const {
    const bool fixed_supported = stream && stream->repeatSupported();
    const bool automatic_supported = stream && stream->automaticSupported();
    const bool active = stream && stream->repeatPending();
    mode->setEnabled(allow_timed && (fixed_supported || automatic_supported) && !active);
    const bool selected_supported = !timed() || (fixedRate() ? fixed_supported : automatic_supported);
    mode->setToolTip(selected_supported ? QString() :
                     QObject::tr("This mode requires compatible USB transmit firmware."));
    if (stream && !active) {
      const int maximum = stream->fixedRateLimit();
      if (frequency->maximum() != maximum) frequency->setMaximum(maximum);
    }
    frequency->setEnabled(!active);
    duration->setEnabled(!active);
    send->setEnabled(valid_input && stream && stream->txReady() && selected_supported);
    cancel->setVisible(active);
  }

  QComboBox* mode;
  QSpinBox* frequency;
  QDoubleSpinBox* duration;
  QPushButton* send;
  QPushButton* cancel;
  bool allow_timed;
};

}  // namespace

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
      new QLabel(tr("Requires firmware implementing the Custom USB Serial protocol. View frames from both buses. Transmit-capable firmware also supports manual sending. "
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

void CustomUsbSerialWidget::showUtilizationDialog(QWidget* parent, CustomUsbSerialStream* usb_stream) {
  const auto name = QStringLiteral("usb_stream_utilization");
  if (auto* existing = parent->findChild<QDialog*>(name, Qt::FindDirectChildrenOnly)) {
    existing->showNormal();
    existing->raise();
    existing->activateWindow();
    return;
  }
  QPointer<CustomUsbSerialStream> stream(usb_stream);
  auto* dialog = new QDialog(parent, Qt::Window);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName(name);
  connect(usb_stream, &QObject::destroyed, dialog, &QDialog::close);
  dialog->setWindowTitle(tr("USB Stream Utilization"));
  auto* form = new QFormLayout(dialog);
  auto* serial = new QLabel(dialog);
  auto* cpu0 = new QLabel(dialog);
  auto* cpu1 = new QLabel(dialog);
  auto* spi = new QLabel(dialog);
  auto* can0 = new QLabel(dialog);
  auto* can1 = new QLabel(dialog);
  form->addRow(tr("Serial utilization"), serial);
  form->addRow(tr("CPU utilization (core 0)"), cpu0);
  form->addRow(tr("CPU utilization (core 1)"), cpu1);
  form->addRow(tr("SPI utilization (shared)"), spi);
  form->addRow(tr("CAN bus 0 utilization"), can0);
  form->addRow(tr("CAN bus 1 utilization"), can1);
  serial->setToolTip(tr("Read and write throughput relative to the 8 Mbit/s logical serial setting."));
  spi->setToolTip(tr("Time spent clocking the shared SPI bus."));
  const QString can_hint = tr("Estimated from frame sizes and bit rates with a bit-stuffing allowance.");
  can0->setToolTip(can_hint);
  can1->setToolTip(can_hint);
  auto* status = new QLabel(dialog);
  form->addRow(status);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  form->addRow(buttons);

  auto refresh = [stream, serial, cpu0, cpu1, spi, can0, can1, status] {
    const auto values = stream ? stream->utilization() : std::nullopt;
    auto format = [&](uint16_t value) {
      return values ? tr("%L1%").arg(value / 10.0, 0, 'f', 1) : QStringLiteral("—");
    };
    const auto sample = values.value_or(CustomUsbSerialStream::Utilization{});
    serial->setText(format(sample.serial));
    cpu0->setText(format(sample.cpu[0]));
    cpu1->setText(format(sample.cpu[1]));
    spi->setText(format(sample.spi));
    can0->setText(format(sample.can[0]));
    can1->setText(format(sample.can[1]));
    status->setText(!stream ? tr("Stream closed.") : values ? QString() : tr("Waiting for utilization data from the adapter."));
  };
  auto* timer = new QTimer(dialog);
  connect(timer, &QTimer::timeout, dialog, refresh);
  refresh();
  timer->start(500);
  dialog->show();
}

void CustomUsbSerialWidget::showSendFrameDialog(QWidget* parent, CustomUsbSerialStream* usb_stream) {
  QPointer<CustomUsbSerialStream> stream(usb_stream);
  QDialog dialog(parent);
  dialog.setWindowTitle(tr("Send Frame"));
  auto* form = new QFormLayout(&dialog);
  auto* bus = new QSpinBox(&dialog);
  bus->setRange(0, 1);
  auto* messages = new QComboBox(&dialog);
  auto* address = new QLineEdit(&dialog);
  address->setPlaceholderText(tr("Hex CAN ID"));
  auto* data = new QLineEdit(&dialog);
  data->setPlaceholderText(tr("Hex bytes, for example 01 23 AB"));
  data->setMinimumWidth(330);
  auto* extended = new QCheckBox(tr("29-bit ID"), &dialog);
  auto* fd = new QCheckBox(tr("CAN FD"), &dialog);
  auto* brs = new QCheckBox(tr("Bit rate switch"), &dialog);
  auto* status = new QLabel(&dialog);
  status->setWordWrap(true);
  form->addRow(tr("Bus"), bus);
  form->addRow(tr("DBC message"), messages);
  form->addRow(tr("CAN ID"), address);
  form->addRow(tr("Payload"), data);
  form->addRow(extended);
  form->addRow(fd);
  form->addRow(brs);
  const SendControls controls(dialog, *form, usb_stream, false);
  form->addRow(status);

  auto populate = [=] {
    const QSignalBlocker blocker(messages);
    messages->clear();
    address->clear();
    data->clear();
    extended->setChecked(false);
    fd->setChecked(false);
    messages->addItem(tr("Custom frame"));
    for (const auto& [id, message] : GetDBC()->getMessages(bus->value())) {
      messages->addItem(QStringLiteral("%1 (0x%2)").arg(message.name).arg(id, 0, 16), id);
    }
  };
  populate();
  connect(bus, &QSpinBox::valueChanged, &dialog, populate);
  connect(GetDBC(), &dbc::Manager::DBCFileChanged, &dialog, populate);
  connect(messages, &QComboBox::activated, &dialog, [=](int index) {
    if (!index) return;
    uint32_t id = messages->currentData().toUInt();
    const auto* message = GetDBC()->msg(MessageId(bus->value(), id));
    if (!message) return;
    address->setText(QString::number(id & 0x1fffffffU, 16));
    extended->setChecked(id > 0x7ff);
    fd->setChecked(message->size > 8);
    data->setText(QStringLiteral("00 ").repeated(std::min(message->size, 64u)).trimmed());
  });
  connect(controls.send, &QPushButton::clicked, &dialog, [=, &controls] {
    QString hex = data->text();
    hex.remove(QRegularExpression(QStringLiteral("\\s")));
    const QRegularExpression payload_pattern(QStringLiteral("\\A(?:[0-9a-fA-F]{2}){0,64}\\z"));
    bool address_ok = false;
    const uint32_t id = address->text().toUInt(&address_ok, 16);
    const uint8_t flags = (extended->isChecked() ? 1 : 0) | (fd->isChecked() ? 4 : 0) |
                          (brs->isChecked() ? 8 : 0);
    if (!stream) return;
    if (!address_ok || !payload_pattern.match(hex).hasMatch() ||
        !stream->sendFrame(bus->value(), id, flags, QByteArray::fromHex(hex.toLatin1()),
                           std::nullopt, controls.timing())) {
      QMessageBox::warning(controls.send, tr("Frame not queued"),
                           tr("Check the CAN ID, frame format, payload length, and USB transmitter status."));
    }
  });
  QTimer timer;
  auto refresh = [&] {
    controls.refresh(stream, true);
    status->setText(stream ? stream->txStatus() : tr("USB stream closed."));
    brs->setEnabled(fd->isChecked());
    if (!fd->isChecked()) brs->setChecked(false);
  };
  connect(&timer, &QTimer::timeout, &dialog, refresh);
  connect(fd, &QCheckBox::toggled, &dialog, refresh);
  timer.start(100);
  refresh();
  dialog.exec();
}

void CustomUsbSerialWidget::showSendSignalDialog(QWidget* parent, CustomUsbSerialStream* usb_stream,
                                                const MessageId& id, const QString& signal_name) {
  QPointer<CustomUsbSerialStream> stream(usb_stream);
  QDialog dialog(parent);
  dialog.setWindowTitle(tr("Send Signal Value"));
  auto* form = new QFormLayout(&dialog);
  auto* value = new QLineEdit(&dialog);
  auto* choices = new QComboBox(&dialog);
  choices->setPlaceholderText(tr("Choose a value"));
  auto* status_area = new QWidget(&dialog);
  auto* status_layout = new QHBoxLayout(status_area);
  status_layout->setContentsMargins(0, 0, 0, 0);
  auto* indicator = new QLabel(QStringLiteral("●"), status_area);
  indicator->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
  indicator->setFixedWidth(indicator->fontMetrics().horizontalAdvance(QStringLiteral("●")) + 4);
  auto* status = new QLabel(status_area);
  status->setTextFormat(Qt::PlainText);
  status->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  status->setWordWrap(true);
  status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  status->setMinimumWidth(320);
  status->setFixedHeight(3 * status->fontMetrics().lineSpacing());
  status_layout->addWidget(indicator);
  status_layout->addWidget(status, 1);
  form->addRow(tr("Signal"), new QLabel(signal_name + QStringLiteral(" · ") + id.toString(), &dialog));
  form->addRow(tr("New value"), value);
  form->addRow(tr("New value"), choices);
  const SendControls controls(dialog, *form, usb_stream, true);
  form->addRow(status_area);
  form->setSizeConstraint(QLayout::SetFixedSize);

  auto getSignal = [&]() -> const dbc::Signal* {
    const auto* message = GetDBC()->msg(id);
    return message ? message->sig(signal_name) : nullptr;
  };
  const auto* initial_signal = getSignal();
  const ValueTable value_table = initial_signal ? initial_signal->value_table : ValueTable{};
  const bool has_choices = !value_table.empty();
  form->setRowVisible(choices, has_choices);
  form->setRowVisible(value, !has_choices);
  if (has_choices) {
    for (const auto& [raw, label] : value_table) {
      const QString number = QString::number(raw, 'g', 17);
      choices->addItem(label.isEmpty() ? number : QStringLiteral("%1 (%2)").arg(label, number), raw);
    }
    choices->setCurrentIndex(-1);
    choices->setFocus();
  } else if (const auto* signal = initial_signal) {
    value->setPlaceholderText(signal->unit);
    if (const auto frame = stream->latestDataFrame(id, signal)) {
      const auto current = signal->parse(reinterpret_cast<const uint8_t*>(frame->data.constData()), frame->data.size());
      if (current) value->setText(QString::number(*current, 'g', 17));
    }
  }
  using TxState = CustomUsbSerialStream::TxState;
  QString input_error;
  bool submitted = false;
  std::optional<TxState> displayed_state;
  auto refresh = [&] {
    const auto* signal = getSignal();
    const bool has_frame = stream && signal && stream->latestDataFrame(id, signal).has_value();
    const bool has_value = has_choices ? choices->currentIndex() >= 0 : !value->text().trimmed().isEmpty();
    const bool mux_supported = !(controls.timed() && signal && signal->multiplexor) ||
                               (stream && stream->transmitFeedbackSupported());
    controls.refresh(stream, has_frame && has_value && mux_supported);

    TxState state = TxState::Waiting;
    QString message;
    if (!stream) {
      state = TxState::Error;
      message = tr("USB stream closed.");
    } else if (stream->repeatPending()) {
      message = stream->txStatus(&state, submitted);
    } else if (!signal) {
      state = TxState::Error;
      message = tr("Signal definition is unavailable.");
    } else if (!mux_supported) {
      message = tr("Timed sends for multiplexed signals require updated USB transmit firmware.");
    } else if (!has_frame && signal->multiplexor) {
      message = tr("Waiting for a frame with %1 = %2.")
                    .arg(signal->multiplexor->name).arg(signal->multiplex_value);
    } else if (!has_frame) {
      message = tr("Waiting for a frame containing this signal.");
    } else if (!input_error.isEmpty()) {
      state = TxState::Error;
      message = input_error;
    } else {
      message = stream->txStatus(&state, submitted);
      if (message.isEmpty()) message = has_value ? tr("Ready to send.") : tr("Choose or enter a value to send.");
    }
    if (displayed_state == state && status->text() == message) return;

    QColor color = status->palette().color(QPalette::PlaceholderText);
    switch (state) {
      case TxState::Ready: color = status->palette().color(QPalette::Highlight); break;
      case TxState::Waiting: break;
      case TxState::Queued: break;
      case TxState::Acknowledged: color = QColor("#2d9d64"); break;
      case TxState::Error: color = QColor("#d85c5c"); break;
    }
    auto palette = indicator->palette();
    palette.setColor(QPalette::WindowText, color);
    indicator->setPalette(palette);
    displayed_state = state;
    status->setText(message);
    status->setToolTip(message);
  };

  connect(controls.send, &QPushButton::clicked, &dialog, [&] {
    if (!stream) return;
    const auto* signal = getSignal();
    auto frame = signal ? stream->latestDataFrame(id, signal) : std::nullopt;
    bool numeric = false;
    const double physical = value->text().toDouble(&numeric);
    bool encoded = false;
    if (frame && signal && signal->value_table == value_table) {
      auto* bytes = reinterpret_cast<uint8_t*>(frame->data.data());
      if (has_choices && choices->currentIndex() >= 0) {
        encoded = signal->encodeRaw(choices->currentData().toDouble(), bytes, frame->data.size());
      } else if (!has_choices && numeric) {
        encoded = signal->encodePhysical(physical, bytes, frame->data.size());
      }
    }
    input_error.clear();
    if (!encoded) {
      input_error = tr("Could not encode the value. Check that it fits the signal.");
    } else {
      const auto timing = controls.timing();
      const auto selector = timing && signal->multiplexor ? signalMask(*signal->multiplexor, frame->data.size()) : QByteArray();
      if (!stream->sendFrame(id.source, id.address, frame->flags, frame->data, frame->boot_id,
                             timing, signalMask(*signal, frame->data.size()), selector)) {
        input_error = tr("The frame could not be queued. Check the USB connection and try again.");
      } else {
        submitted = true;
      }
    }
    refresh();
  });

  QTimer timer;
  connect(&timer, &QTimer::timeout, &dialog, refresh);
  connect(controls.mode, &QComboBox::currentIndexChanged, &dialog, refresh);
  auto value_changed = [&] {
    input_error.clear();
    refresh();
  };
  connect(choices, &QComboBox::currentIndexChanged, &dialog, value_changed);
  connect(value, &QLineEdit::textChanged, &dialog, value_changed);
  timer.start(100);
  refresh();
  if (!has_choices) value->selectAll();
  dialog.exec();
}
