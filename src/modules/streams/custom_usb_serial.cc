#include "custom_usb_serial.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSerialPortInfo>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <array>

#include "core/dbc/dbc_manager.h"
#include "core/streams/custom_usb_serial_protocol.h"
#include "core/streams/custom_usb_serial_stream.h"
#include "modules/settings/settings.h"
#include "modules/system/stream_manager.h"

namespace {

QByteArray signalMask(const dbc::Signal& signal, int data_size) {
  QByteArray mask(data_size, 0);
  for (int i = 0; i < signal.size; ++i) {
    const int bit = signal.getBitIndex(i);
    if (bit >= 0 && bit / 8 < mask.size()) mask[bit / 8] |= char(1u << (bit % 8));
  }
  return mask;
}

const dbc::Signal* counterSignal(const dbc::Msg* message, const dbc::Signal* requested) {
  if (!message) return nullptr;
  static const QRegularExpression counter_name(QStringLiteral("(?:^|_)counter(?:$|_)|counter$"),
                                                QRegularExpression::CaseInsensitiveOption);
  const dbc::Signal* found = nullptr;
  for (const auto* signal : message->getSignals()) {
    if (signal == requested || signal->size < 1 || signal->size > 64 ||
        !counter_name.match(signal->name).hasMatch()) continue;
    if (found) return nullptr;
    found = signal;
  }
  return found;
}

const dbc::Signal* checksumSignal(const dbc::Msg* message, const dbc::Signal* requested) {
  if (!message) return nullptr;
  static const QRegularExpression checksum_name(QStringLiteral("checksum$"),
                                                 QRegularExpression::CaseInsensitiveOption);
  const dbc::Signal* found = nullptr;
  for (const auto* signal : message->getSignals()) {
    if (signal == requested || signal->size != 8 || !signal->is_little_endian ||
        signal->start_bit % 8 != 0 || !checksum_name.match(signal->name).hasMatch()) continue;
    if (found) return nullptr;
    found = signal;
  }
  return found;
}

class TimingArrow : public QWidget {
public:
  explicit TimingArrow(QWidget* parent) : QWidget(parent) {
    setMinimumSize(26, 14);
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(palette().color(QPalette::Text), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const int y = height() / 2;
    const int end = width() - 3;
    painter.drawLine(2, y, end, y);
    painter.drawPolyline(QPolygon{QPoint(end - 5, y - 4), QPoint(end, y), QPoint(end - 5, y + 4)});
  }
};

QString lastSamples(uint32_t count) {
  return count == 1 ? QObject::tr("(Last 1 Sample)") : QObject::tr("(Last %L1 Samples)").arg(count);
}

class TxTimingDiagram : public QWidget {
public:
  explicit TxTimingDiagram(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    heading = new QLabel(this);
    layout->addWidget(heading);
    auto* stages = new QHBoxLayout;
    stages->setSpacing(6);
    layout->addLayout(stages);

    const QStringList activity{QObject::tr("Wait for receive task"), QObject::tr("Read and detect overwrite"),
                               QObject::tr("Prepare and queue correction"), QObject::tr("Wait to transmit")};
    const QStringList explanation{
        QObject::tr("From frame start until the receive task is awake. Zero when it is already running."),
        QObject::tr("From the receive task being available until the firmware detects the overwrite."),
        QObject::tr("From overwrite detection until the controller is asked to transmit."),
        QObject::tr("From the transmit request to the controller timestamp for successful attempts.")};
    for (int i = 0; i < activity.size(); ++i) {
      if (i) stages->addWidget(new TimingArrow(this), 0, Qt::AlignVCenter);
      auto* card = new QFrame(this);
      card->setFrameShape(QFrame::StyledPanel);
      card->setToolTip(explanation[i]);
      auto* content = new QVBoxLayout(card);
      content->setContentsMargins(8, 6, 8, 6);
      content->setSpacing(3);
      timings[i] = new QLabel(card);
      timings[i]->setAlignment(Qt::AlignCenter);
      auto font = timings[i]->font();
      font.setBold(true);
      timings[i]->setFont(font);
      auto* name = new QLabel(activity[i], card);
      name->setAlignment(Qt::AlignCenter);
      content->addWidget(timings[i]);
      content->addWidget(name);
      stages->addWidget(card, 1);
    }
  }

  void setLatency(const CustomUsbSerialStream::TxLatency& latency) {
    const auto title = QObject::tr("Retransmission Timing %1").arg(lastSamples(latency.count ? latency.count : latency.tx_count));
    if (heading->text() != title) heading->setText(title);
    const std::array<uint32_t, 4> averages{latency.wake_mean_us, latency.drain_mean_us,
                                           latency.submit_mean_us, latency.tx_mean_us};
    const std::array<uint32_t, 4> peaks{latency.wake_max_us, latency.drain_max_us,
                                        latency.submit_max_us, latency.tx_max_us};
    const std::array<bool, 4> available{latency.count != 0, latency.count != 0,
                                        latency.count != 0, latency.tx_count != 0};
    for (int i = 0; i < 4; ++i) {
      const auto text = available[i] ? QObject::tr("%L1 µs avg\n%L2 µs peak").arg(averages[i]).arg(peaks[i]) : QStringLiteral("—");
      if (timings[i]->text() != text) timings[i]->setText(text);
    }
  }

private:
  QLabel* heading;
  std::array<QLabel*, 4> timings{};
};

struct SendStatus {
  using TxState = CustomUsbSerialStream::TxState;

  SendStatus(QDialog& dialog, QFormLayout& form) {
    auto* status_area = new QWidget(&dialog);
    auto* status_layout = new QHBoxLayout(status_area);
    status_layout->setContentsMargins(0, 0, 0, 0);
    indicator = new QLabel(QStringLiteral("●"), status_area);
    indicator->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    indicator->setFixedWidth(indicator->fontMetrics().horizontalAdvance(QStringLiteral("●")) + 4);
    label = new QLabel(status_area);
    label->setTextFormat(Qt::PlainText);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setWordWrap(true);
    label->setMinimumWidth(320);
    label->setMinimumHeight(3 * label->fontMetrics().lineSpacing());
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_layout->addWidget(indicator);
    status_layout->addWidget(label, 1);
    form.addRow(status_area);
  }

  void show(TxState state, const QString& message) {
    if (displayed_state == state && label->text() == message) return;

    QColor color = label->palette().color(QPalette::PlaceholderText);
    switch (state) {
      case TxState::Ready: color = label->palette().color(QPalette::Highlight); break;
      case TxState::Waiting: break;
      case TxState::Queued: break;
      case TxState::Acknowledged: color = QColor("#2d9d64"); break;
      case TxState::Error: color = QColor("#d85c5c"); break;
    }
    auto palette = indicator->palette();
    palette.setColor(QPalette::WindowText, color);
    indicator->setPalette(palette);
    displayed_state = state;
    label->setText(message);
    label->setToolTip(message);
  }

  QLabel* indicator;
  QLabel* label;
  std::optional<TxState> displayed_state;
};

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

void CustomUsbSerialWidget::showFirmwareLogDialog(QWidget* parent, CustomUsbSerialStream* usb_stream) {
  const auto name = QStringLiteral("usb_serial_firmware_log");
  if (auto* existing = parent->findChild<QDialog*>(name, Qt::FindDirectChildrenOnly)) {
    existing->showNormal();
    existing->raise();
    existing->activateWindow();
    return;
  }

  auto* dialog = new QDialog(parent, Qt::Window);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName(name);
  dialog->setWindowTitle(tr("Firmware Log"));
  dialog->resize(780, 440);
  if (usb_stream == StreamManager::stream()) {
    connect(&StreamManager::instance(), &StreamManager::streamChanged, dialog, &QDialog::close);
  }
  connect(usb_stream, &QObject::destroyed, dialog, &QDialog::close);
  connect(dialog, &QDialog::finished, dialog, [dialog] { dialog->setObjectName({}); });

  auto* layout = new QVBoxLayout(dialog);
  auto* status = new QLabel(tr("Waiting for firmware logs…"), dialog);
  auto* output = new QPlainTextEdit(dialog);
  output->setReadOnly(true);
  output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  output->setMaximumBlockCount(2000);
  layout->addWidget(status);
  layout->addWidget(output);

  auto* timer = new QTimer(dialog);
  connect(timer, &QTimer::timeout, dialog, [stream = QPointer(usb_stream), status, output, cursor = uint64_t{0}]() mutable {
    if (!stream) return;
    const QString text = stream->firmwareLogSince(cursor);
    if (text.isEmpty()) return;
    status->hide();
    auto* scrollbar = output->verticalScrollBar();
    const bool at_end = scrollbar->value() == scrollbar->maximum();
    QTextCursor end(output->document());
    end.movePosition(QTextCursor::End);
    end.insertText(text);
    if (at_end) scrollbar->setValue(scrollbar->maximum());
  });
  timer->start(100);
  dialog->show();
}

void CustomUsbSerialWidget::showDeviceMetricsDialog(QWidget* parent, CustomUsbSerialStream* usb_stream) {
  const auto name = QStringLiteral("usb_serial_device_metrics");
  if (auto* existing = parent->findChild<QDialog*>(name, Qt::FindDirectChildrenOnly)) {
    existing->showNormal();
    existing->raise();
    existing->activateWindow();
    return;
  }
  auto* dialog = new QDialog(parent, Qt::Window);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName(name);
  connect(dialog, &QDialog::finished, dialog, [dialog] { dialog->setObjectName({}); });
  dialog->setWindowTitle(tr("USB Serial Device Metrics"));
  auto* form = new QFormLayout(dialog);
  using Metric = CustomUsbSerialStream::StatusMetric;
  form->addRow(new QLabel(tr("Check a metric to show it in the status bar."), dialog));
  auto add_metric = [&](Metric metric, const QString& title, const QString& tooltip = {}) {
    auto* checkbox = new QCheckBox(title, dialog);
    checkbox->setChecked(settings.usb_status_metrics & metric);
    checkbox->setToolTip(tooltip);
    connect(checkbox, &QCheckBox::toggled, dialog, [metric](bool checked) {
      if (checked) settings.usb_status_metrics |= metric;
      else settings.usb_status_metrics &= ~metric;
      emit settings.usbStatusMetricsChanged();
    });
    auto* value = new QLabel(dialog);
    value->setToolTip(tooltip);
    form->addRow(checkbox, value);
    return value;
  };
  auto* serial = add_metric(Metric::SerialUtil, tr("Serial utilization"),
                            tr("Throughput relative to the 8 Mbit/s logical serial setting."));
  auto* cpu0 = add_metric(Metric::Cpu0Util, tr("CPU utilization (core 0)"));
  auto* cpu1 = add_metric(Metric::Cpu1Util, tr("CPU utilization (core 1)"));
  auto* spi = add_metric(Metric::SpiUtil, tr("SPI utilization (shared)"),
                         tr("Time spent clocking the shared SPI bus."));
  const auto can_hint = tr("Estimated from frame sizes and bit rates with a bit-stuffing allowance.");
  auto* can0 = add_metric(Metric::Bus0Util, tr("CAN bus 0 utilization"), can_hint);
  auto* can1 = add_metric(Metric::Bus1Util, tr("CAN bus 1 utilization"), can_hint);
  auto* gaps = add_metric(Metric::SequenceGaps, tr("USB Frame Sequence Gaps"),
                          tr("Missing CAN frame sequence numbers in the USB stream across both buses."));
  auto* drops = add_metric(Metric::QueueDrops, tr("Firmware Capture Queue Drops"),
                           tr("CAN frames discarded because the DashKit firmware capture queue was full."));
  auto* fifo_a = add_metric(Metric::FifoA, tr("CAN Controller A FIFO Overflow"),
                            tr("Bus 0 receive FIFO overflow events. Each event indicates at least one lost frame."));
  auto* fifo_b = add_metric(Metric::FifoB, tr("CAN Controller B FIFO Overflow"),
                            tr("Bus 1 receive FIFO overflow events. Each event indicates at least one lost frame."));
  auto* invalid = add_metric(Metric::InvalidRecords, tr("Invalid USB Records"),
                              tr("USB records Cabana rejected due to framing, checksum, or format errors."));
  auto* status = new QLabel(dialog);
  form->addRow(status);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  form->addRow(buttons);

  const bool follow_active_stream = usb_stream == StreamManager::stream();
  auto refresh = [initial_stream = QPointer(usb_stream), follow_active_stream,
                  serial, cpu0, cpu1, spi, can0, can1, gaps, drops, fifo_a, fifo_b, invalid, status] {
    const auto* stream = follow_active_stream ? dynamic_cast<CustomUsbSerialStream*>(StreamManager::stream())
                                              : initial_stream.data();
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
    if (stream) {
      const auto counters = stream->captureCounters();
      gaps->setText(tr("%L1").arg(counters.missing_frames));
      drops->setText(tr("%L1").arg(counters.queue_drops));
      fifo_a->setText(tr("%L1").arg(counters.fifo_overflow_events[0]));
      fifo_b->setText(tr("%L1").arg(counters.fifo_overflow_events[1]));
      invalid->setText(tr("%L1").arg(counters.invalid_records));
    } else {
      for (auto* counter : {gaps, drops, fifo_a, fifo_b, invalid}) counter->setText(QStringLiteral("—"));
    }
    status->setText(!stream ? tr("Stream closed.") : values ? QString() : tr("Waiting for utilization data from the adapter."));
  };
  auto* timer = new QTimer(dialog);
  connect(timer, &QTimer::timeout, dialog, refresh);
  if (follow_active_stream) {
    connect(&StreamManager::instance(), &StreamManager::streamChanged, dialog, refresh);
  }
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
  form->addRow(tr("Bus"), bus);
  form->addRow(tr("DBC message"), messages);
  form->addRow(tr("CAN ID"), address);
  form->addRow(tr("Payload"), data);
  form->addRow(extended);
  form->addRow(fd);
  form->addRow(brs);
  const SendControls controls(dialog, *form, usb_stream, false);
  SendStatus status(dialog, *form);

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
  using TxState = CustomUsbSerialStream::TxState;
  bool submitted = false;
  QString input_error;
  QTimer timer;
  auto refresh = [&] {
    controls.refresh(stream, true);
    TxState state = TxState::Error;
    QString message;
    if (!stream) {
      message = tr("USB stream closed.");
    } else if (!input_error.isEmpty()) {
      message = input_error;
    } else {
      message = stream->txStatus(&state, submitted);
      if (message.isEmpty()) message = tr("Ready to send.");
    }
    status.show(state, message);
    brs->setEnabled(fd->isChecked());
    if (!fd->isChecked()) brs->setChecked(false);
  };
  connect(controls.send, &QPushButton::clicked, &dialog, [&] {
    if (!stream) return;
    QString hex = data->text();
    hex.remove(QRegularExpression(QStringLiteral("\\s")));
    const QRegularExpression payload_pattern(QStringLiteral("\\A(?:[0-9a-fA-F]{2}){0,64}\\z"));
    bool address_ok = false;
    const uint32_t id = address->text().toUInt(&address_ok, 16);
    const uint8_t flags = (extended->isChecked() ? 1 : 0) | (fd->isChecked() ? 4 : 0) |
                          (brs->isChecked() ? 8 : 0);
    input_error.clear();
    if (address_ok && payload_pattern.match(hex).hasMatch() &&
        stream->sendFrame(bus->value(), id, flags, QByteArray::fromHex(hex.toLatin1()),
                          std::nullopt, controls.timing())) {
      submitted = true;
    } else {
      submitted = false;
      input_error = tr("Frame not queued. Check the CAN ID, format, payload length, and USB transmitter status.");
    }
    refresh();
  });
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
  auto* increase_counter = new QCheckBox(tr("Increase from last received frame"), &dialog);
  auto* recalculate_checksum = new QCheckBox(tr("Recalculate before transmission"), &dialog);
  choices->setPlaceholderText(tr("Choose a value"));
  form->addRow(tr("Signal"), new QLabel(signal_name + QStringLiteral(" · ") + id.toString(), &dialog));
  form->addRow(tr("New value"), value);
  form->addRow(tr("New value"), choices);
  form->addRow(tr("Counter"), increase_counter);
  form->addRow(tr("Checksum"), recalculate_checksum);
  const SendControls controls(dialog, *form, usb_stream, true);
  SendStatus status(dialog, *form);
  auto* latency_summary = new QLabel(&dialog);
  latency_summary->setWordWrap(true);
  auto* show_details = new QToolButton(&dialog);
  show_details->setText(tr("More info"));
  show_details->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  show_details->setArrowType(Qt::RightArrow);
  show_details->setCheckable(true);
  show_details->setAutoRaise(true);
  auto* timing_diagram = new TxTimingDiagram(&dialog);
  auto* details = new QLabel(&dialog);
  details->setTextFormat(Qt::RichText);
  details->setWordWrap(true);
  details->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(latency_summary);
  form->addRow(show_details);
  form->setAlignment(show_details, Qt::AlignLeft);
  form->addRow(timing_diagram);
  form->addRow(details);
  form->setRowVisible(latency_summary, false);
  form->setRowVisible(show_details, false);
  form->setRowVisible(timing_diagram, false);
  form->setRowVisible(details, false);
  form->setSizeConstraint(QLayout::SetFixedSize);

  auto getSignal = [&]() -> const dbc::Signal* {
    const auto* message = GetDBC()->msg(id);
    return message ? message->sig(signal_name) : nullptr;
  };
  const auto* initial_signal = getSignal();
  const auto* initial_message = GetDBC()->msg(id);
  const auto* initial_counter = counterSignal(initial_message, initial_signal);
  const auto* initial_checksum = checksumSignal(initial_message, initial_signal);
  const QString counter_name = initial_counter ? initial_counter->name : QString();
  const QString checksum_name = initial_checksum ? initial_checksum->name : QString();
  increase_counter->setEnabled(initial_counter && stream && stream->counterSupported());
  increase_counter->setChecked(increase_counter->isEnabled());
  increase_counter->setToolTip(initial_counter ? tr("Increment %1, wrapping at its signal width.").arg(counter_name)
                                               : tr("This message does not have one identifiable counter signal."));
  recalculate_checksum->setChecked(initial_checksum);
  recalculate_checksum->setEnabled(initial_checksum && stream && stream->txReady());
  recalculate_checksum->setToolTip(initial_checksum ? tr("Recalculate %1 using the Tesla byte-sum algorithm.").arg(checksum_name)
                                                    : tr("This message does not have one identifiable byte checksum signal."));
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
  auto format_stats = [&](const std::optional<CustomUsbSerialStream::TxLatency>& latency) {
    if (!latency) return QString();
    QString rows;
    const auto row = [&](const QString& label, const QString& value) {
      rows += QStringLiteral("<tr><td>%1</td><td><b>%2</b></td></tr>")
                  .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
    };
    if (latency->count) {
      row(tr("Frame start to overwrite detection"),
          tr("%L1 µs avg · %L2 µs peak").arg(latency->rx_mean_us).arg(latency->rx_max_us));
      row(tr("Frames waiting when the task woke on timeout"), tr("%L1").arg(latency->timeouts));
      row(tr("Frames processed first · same FIFO"),
          tr("%L1 avg · %L2 peak").arg(latency->same_fifo_mean).arg(latency->same_fifo_max));
      row(tr("Frames processed first · other FIFOs"),
          tr("%L1 avg · %L2 peak").arg(latency->other_fifo_mean).arg(latency->other_fifo_max));
      row(tr("All frames processed first (up to 32 counted)"),
          tr("%L1 avg · %L2 peak").arg(latency->ahead_mean).arg(latency->ahead_max));
    }
    row(tr("Successful transmissions timed"), tr("%L1").arg(latency->tx_count));
    row(tr("Attempts that only lost arbitration"), tr("%L1").arg(latency->arbitration_losses));
    return QStringLiteral("<table cellspacing='3' cellpadding='3'>%1</table>").arg(rows);
  };
  auto refresh = [&] {
    const auto* signal = getSignal();
    const bool has_frame = stream && signal && stream->latestDataFrame(id, signal).has_value();
    const bool has_value = has_choices ? choices->currentIndex() >= 0 : !value->text().trimmed().isEmpty();
    const bool mux_supported = !(controls.timed() && signal && signal->multiplexor) ||
                               (stream && stream->transmitFeedbackSupported());
    const bool counter_supported = initial_counter && stream && stream->counterSupported();
    controls.refresh(stream, has_frame && has_value && mux_supported &&
                             (!increase_counter->isChecked() || counter_supported));
    increase_counter->setEnabled(counter_supported && !stream->repeatPending());
    recalculate_checksum->setEnabled(initial_checksum && stream && stream->txReady() && !stream->repeatPending());
    if (initial_counter) {
      increase_counter->setToolTip(counter_supported ? tr("Increment %1, wrapping at its signal width.").arg(counter_name)
                                                     : tr("Waiting for USB transmit firmware."));
    }

    TxState state = TxState::Waiting;
    QString message;
    std::optional<CustomUsbSerialStream::TxErrorDetails> controller_error;
    if (!stream) {
      state = TxState::Error;
      message = tr("USB stream closed.");
    } else if (stream->repeatPending()) {
      message = stream->txStatus(&state, submitted, &controller_error);
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
      message = stream->txStatus(&state, submitted, &controller_error);
      if (message.isEmpty()) message = has_value ? tr("Ready to send.") : tr("Choose or enter a value to send.");
    }
    QString error_details;
    if (state == TxState::Error) {
      const auto newline = message.indexOf(QLatin1Char('\n'));
      if (newline >= 0) {
        error_details = message.mid(newline + 1);
        message.truncate(newline);
      }
    }
    status.show(state, message);

    const auto latency = stream && submitted && controls.automatic()
        ? stream->txLatency(id.source) : std::nullopt;
    QString summary;
    if (latency && latency->rx_to_tx_count) {
      summary = tr("The average time from receiving a frame to transmitting its correction was %L1 ms.")
                    .arg(latency->rx_to_tx_mean_us / 1000.0, 0, 'f', 2);
      summary += QLatin1Char(' ') + lastSamples(latency->rx_to_tx_count);
    }
    if (latency_summary->text() != summary) latency_summary->setText(summary);
    form->setRowVisible(latency_summary, !summary.isEmpty());
    const bool has_details = controls.automatic() || !error_details.isEmpty();
    form->setRowVisible(show_details, has_details);
    const bool expanded = has_details && show_details->isChecked();
    const bool show_diagram = expanded && latency && (latency->count || latency->tx_count);
    if (show_diagram) timing_diagram->setLatency(*latency);
    form->setRowVisible(timing_diagram, show_diagram);
    if (expanded) {
      QString text;
      if (controller_error) {
        text = QStringLiteral("<p><b>%1</b></p>").arg(tr("Controller error").toHtmlEscaped());
        if (!controller_error->conditions.isEmpty()) {
          QStringList conditions;
          for (const auto& condition : controller_error->conditions)
            conditions << QStringLiteral("<li>%1</li>").arg(condition.toHtmlEscaped());
          text += QStringLiteral("<ul>%1</ul>").arg(conditions.join(QString()));
        }
        text += QStringLiteral("<table border='1' cellspacing='0' cellpadding='6'>"
                               "<tr><th>%1</th><th>%2</th><th>%3</th></tr>"
                               "<tr><td align='center'>%4</td><td align='center'>%5</td><td align='center'>%6</td></tr></table>")
                    .arg(tr("Transmit error counter").toHtmlEscaped(), tr("Receive error counter").toHtmlEscaped(),
                         tr("Acknowledged this session").toHtmlEscaped())
                    .arg(controller_error->transmit_errors).arg(controller_error->receive_errors)
                    .arg(controller_error->acknowledged);
        text += QStringLiteral("<p><small>%1</small></p>")
                    .arg(tr("Raw codes · FIFO 0x%1 · Bus diagnostics 0x%2")
                             .arg(controller_error->fifo, 0, 16).arg(controller_error->bus_diagnostics, 0, 16).toHtmlEscaped());
      } else if (!error_details.isEmpty()) {
        text = QStringLiteral("<p>%1</p>").arg(error_details.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
      }
      text += format_stats(latency);
      if (controls.automatic() && !latency) {
        const auto note = !submitted ? tr("Start automatic retransmission to collect timing data.")
                        : stream && stream->repeatPending() ? tr("Waiting for timing data from the DashKit.")
                        : tr("The DashKit reported no timing samples for this run.");
        text += QStringLiteral("<p>%1</p>").arg(note.toHtmlEscaped());
      }
      if (details->text() != text) details->setText(text);
    }
    form->setRowVisible(details, expanded);
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
      std::optional<CustomUsbSerialStream::Counter> counter;
      std::optional<CustomUsbSerialStream::Checksum> checksum;
      if (increase_counter->isChecked()) {
        const auto* message = GetDBC()->msg(id);
        const auto* counter_signal = message ? message->sig(counter_name) : nullptr;
        if (counter_signal) {
          counter = CustomUsbSerialStream::Counter{static_cast<uint16_t>(counter_signal->start_bit),
                                                   static_cast<uint8_t>(counter_signal->size),
                                                   counter_signal->is_little_endian};
        }
      }
      if (recalculate_checksum->isChecked()) {
        const auto* message = GetDBC()->msg(id);
        const auto* checksum_signal = message ? message->sig(checksum_name) : nullptr;
        if (checksum_signal) {
          checksum = CustomUsbSerialStream::Checksum{static_cast<uint8_t>(checksum_signal->start_bit / 8)};
        }
      }
      if (!stream->sendFrame(id.source, id.address, frame->flags, frame->data, frame->boot_id,
                             timing, signalMask(*signal, frame->data.size()), selector, counter, checksum)) {
        input_error = tr("The frame could not be queued. Check the USB connection and try again.");
      } else {
        submitted = true;
      }
    }
    refresh();
  });

  QTimer timer;
  connect(&timer, &QTimer::timeout, &dialog, refresh);
  connect(controls.mode, &QComboBox::currentIndexChanged, &dialog, [&] {
    submitted = false;
    input_error.clear();
    const QSignalBlocker blocker(show_details);
    show_details->setChecked(false);
    show_details->setArrowType(Qt::RightArrow);
    refresh();
  });
  connect(show_details, &QToolButton::toggled, &dialog, [&](bool expanded) {
    show_details->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    refresh();
  });
  auto value_changed = [&] {
    input_error.clear();
    refresh();
  };
  connect(choices, &QComboBox::currentIndexChanged, &dialog, value_changed);
  connect(value, &QLineEdit::textChanged, &dialog, value_changed);
  connect(increase_counter, &QCheckBox::toggled, &dialog, value_changed);
  connect(recalculate_checksum, &QCheckBox::toggled, &dialog, value_changed);
  timer.start(100);
  refresh();
  if (!has_choices) value->selectAll();
  dialog.exec();
}
