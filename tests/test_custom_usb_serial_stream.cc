#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QEventLoop>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QtCharts/QLegendMarker>
#include <algorithm>
#include <iostream>
#include <utility>

#include "core/streams/custom_usb_serial_stream.h"
#include "modules/charts/charts_panel.h"
#include "modules/settings/settings.h"
#include "modules/streams/custom_usb_serial.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (argc == 2 && QString::fromUtf8(argv[1]) == "--preview") {
    CustomUsbSerialWidget widget;
    widget.resize(660, 340);
    widget.show();
    app.processEvents();
    return widget.grab().save("build/custom-usb-serial-source-preview.png") ? 0 : 1;
  }
  if (argc == 3 && QString::fromUtf8(argv[1]) == "--live-check") {
    CustomUsbSerialStream stream(&app, QString::fromUtf8(argv[2]));
    stream.start();
    QTimer::singleShot(10000, &app, [&] {
      stream.stop();
      uint64_t buses[2] = {};
      for (const auto* event : stream.allEvents()) if (event->src < 2) buses[event->src]++;
      std::cout << stream.captureStatus().toStdString() << '\n' << stream.captureDetails().toStdString() << '\n';
      std::cout << "Qt live reader: bus0=" << buses[0] << " bus1=" << buses[1] << '\n';
      app.exit(buses[0] && buses[1] ? 0 : 1);
    });
    return app.exec();
  }
  if (argc != 3) return 2;
  struct RestoreLogging {
    bool saved = settings.log_livestream;
    QString saved_path = settings.log_path;
    ~RestoreLogging() {
      settings.log_livestream = saved;
      settings.log_path = saved_path;
    }
  } restore_logging;
  // The Custom USB Serial source must stay in memory even if global live logging is enabled.
  settings.log_livestream = true;
  settings.log_path = QString::fromUtf8(argv[2]);
  // macOS PTYs do not implement the custom-speed ioctl used by USB devices.
  CustomUsbSerialStream stream(&app, QString::fromUtf8(argv[1]), 115200);
  stream.start();
  const QByteArray repeat_test = qgetenv("CABANA_TEST_TX_REPEAT");
  const bool repeat = !repeat_test.isEmpty();
  const bool automatic = repeat_test.startsWith("automatic");
  const QByteArray feedback = qgetenv("CABANA_TEST_TX_FEEDBACK");
  bool sent = false;
  QTimer send_timer;
  QObject::connect(&send_timer, &QTimer::timeout, &app, [&] {
    if (!stream.txReady()) return;
    send_timer.stop();
    if (!stream.automaticSupported() || !stream.repeatSupported()) {
      app.exit(13);
      return;
    }
    if (stream.sendFrame(1, 0x1abcde, 13, QByteArray(10, 0))) {
      app.exit(7);  // Ten bytes cannot be encoded in a CAN FD DLC.
      return;
    }
    auto frame = stream.latestDataFrame(MessageId(0, 0x203));
    if (!frame || frame->data != QByteArray(1, char(0x01)) ||
        stream.latestDataFrame(MessageId(1, 0x1abcde))) {
      app.exit(10);  // A restarted adapter must not reuse older frames.
      return;
    }
    dbc::Signal signal;
    signal.start_bit = 1;
    signal.size = 3;
    signal.factor = 0.5;
    signal.is_signed = true;
    if (!signal.encodePhysical(-1.0, reinterpret_cast<uint8_t*>(frame->data.data()), frame->data.size()) ||
        frame->data != QByteArray(1, char(0x0d))) {
      app.exit(11);
      return;
    }
    const MessageId multiplexed_id(1, 0x22222);
    dbc::Signal selector;
    selector.start_bit = 63 * 8;
    selector.size = 1;
    selector.update();
    signal.multiplexor = &selector;
    signal.multiplex_value = 1;
    signal.start_bit = 7;
    signal.size = 12;
    signal.is_signed = false;
    signal.is_little_endian = false;
    signal.factor = 0.25;
    signal.offset = 10.0;
    signal.value_table = {{0x123, QStringLiteral("Enabled")}};
    const auto latest = stream.latestDataFrame(multiplexed_id);
    auto matching = stream.latestDataFrame(multiplexed_id, &signal);
    if (!latest || latest->data.back() != char(0xa4) || !matching || matching->data.back() != char(0xa5) ||
        !signal.encodeRaw(signal.value_table.front().first,
                          reinterpret_cast<uint8_t*>(matching->data.data()), matching->data.size())) {
      app.exit(12);
      return;
    }
    std::optional<CustomUsbSerialStream::RepeatTiming> timing;
    if (repeat) {
      const unsigned frequency = automatic ? 0u : (feedback.isEmpty() ? 5u : 500u);
      timing = CustomUsbSerialStream::RepeatTiming{frequency, 1000, automatic};
    } else if (!feedback.isEmpty()) {
      QTimer::singleShot(150, &app, [&] {
        if (stream.txReady() || !stream.txStatus().contains("Waiting for controller feedback")) app.exit(17);
      });
    }
    QByteArray signal_mask(64, 0);
    signal_mask[0] = char(0xff);
    signal_mask[1] = char(0xf0);
    QByteArray selector_mask;
    if (repeat && !feedback.isEmpty()) {
      selector_mask.fill(0, 64);
      selector_mask[63] = 1;
    } else {
      signal_mask[63] = 1;
    }
    sent = stream.sendFrame(multiplexed_id.source, multiplexed_id.address, matching->flags,
                            matching->data, matching->boot_id, timing, signal_mask, selector_mask);
    if (repeat_test.endsWith("cancel")) {
      QTimer::singleShot(550, &app, [&] {
        if (!stream.repeatPending()) app.exit(14);
        stream.cancelRepeat();
      });
    }
    if (!sent || stream.sendFrame(0, 0x123, 0, QByteArray())) app.exit(8);
  });
  send_timer.start(20);
  QTimer::singleShot(2200, &app, [&] {
    QString expected_status = "Queued by the USB adapter";
    if (repeat_test == "cancel") expected_status = "Fixed-rate send cancelled";
    else if (repeat_test == "automatic-cancel") expected_status = "Automatic send cancelled";
    else if (automatic) expected_status = "Automatic send finished";
    else if (repeat) expected_status = "Fixed-rate send finished";
    else if (qEnvironmentVariableIsSet("CABANA_TEST_TX_FIFO_FULL")) expected_status = "CAN bus 1 transmit queue is full";
    if (!feedback.isEmpty()) {
      if (repeat) expected_status += ".\nAcknowledged frames (session total): 2";
      else if (feedback == "ack") expected_status = "Transmitted and acknowledged";
      else if (feedback == "unconfirmed") expected_status = "Could not confirm the send on CAN bus 1";
      else if (feedback == "unknown") expected_status = "Could not determine whether the frame was sent on CAN bus 1";
    }
    if (!sent || stream.repeatPending() || !stream.txStatus().contains(expected_status)) {
      std::cerr << "Expected " << expected_status.toStdString() << ", got " << stream.txStatus().toStdString() << '\n';
      app.exit(9);
      return;
    }
    if (feedback == "unconfirmed") {
      std::optional<CustomUsbSerialStream::TxErrorDetails> details;
      const auto message = stream.txStatus(nullptr, true, &details);
      if (!message.contains("recorded a missing acknowledgment") || !message.contains("transmit 8, receive 0") ||
          !details || details->transmit_errors != 8 || details->receive_errors || details->acknowledged ||
          details->fifo != 0x30 || details->bus_diagnostics != (1u << 18) || details->conditions.size() != 2) {
        app.exit(18);
        return;
      }
    }
    CustomUsbSerialStream::TxState fresh_state;
    if (!stream.txStatus(&fresh_state, false).isEmpty() || fresh_state != CustomUsbSerialStream::TxState::Ready) {
      app.exit(23);
      return;
    }
    stream.stop();
    std::cout << stream.captureStatus().toStdString() << '\n' << stream.captureDetails().toStdString() << '\n';
    const auto& events = stream.allEvents();
    const bool has_ack_frame = feedback == "ack";
    const int ack_frames = has_ack_frame ? (repeat ? 2 : 1) : 0;
    const size_t expected_events = 9 + ack_frames;
    if (has_ack_frame) {
      const auto& sent_frames = stream.events(MessageId(1 + CustomUsbSerialStream::TxSourceOffset, 0x22222));
      if (sent_frames.size() != ack_frames || sent_frames.front()->size != 64 || sent_frames.front()->dat[0] != 0x12 ||
          stream.latestDataFrame(MessageId(1, 0x22222))->data.back() != char(0xa4)) {
        app.exit(19);
        return;
      }
      dbc::Signal signal;
      signal.size = 8;
      signal.is_little_endian = true;
      signal.factor = 1;
      signal.update();
      ChartsPanel charts;
      charts.resize(800, 400);
      charts.show();
      charts.showChart(MessageId(1, 0x22222), &signal, true, false);
      auto* view = charts.findChild<ChartView*>();
      if (!view) {
        app.exit(21);
        return;
      }
      auto* chart = view->chart();
      auto& chart_signal = chart->sigs_.front();
      auto* acknowledged = new QScatterSeries;
      chart_signal.acknowledged_series = acknowledged;
      chart->addSeries(acknowledged);
      for (auto* axis : chart_signal.series->attachedAxes()) acknowledged->attachAxis(axis);
      for (auto* marker : chart->legend()->markers(acknowledged)) marker->setVisible(false);
      // Paint with an empty TX legend entry, then paint again with a sample.
      if (view->viewport()->grab().isNull()) {
        app.exit(22);
        return;
      }
      chart_signal.prepareData(&stream.eventsMap(), 0, 1);
      chart_signal.updateSeries(SeriesType::Line, 0, 1);
      chart_signal.updateRange(0, 1);
      if (view->viewport()->grab().isNull() ||
          chart_signal.series->count() != 2 + ack_frames || acknowledged->count() != ack_frames ||
          chart_signal.series->at(0).y() != 0xa5 || chart_signal.series->at(2).y() != 0x12 ||
          acknowledged->at(0).y() != 0x12 ||
          (repeat && acknowledged->at(1).y() != 0x34) ||
          chart_signal.min_value != 0x12 || chart_signal.max_value != 0xa5) {
        app.exit(20);
        return;
      }
    }
    if (events.size() != expected_events) {
      std::cerr << "Expected " << expected_events << " frames, got " << events.size() << '\n';
      app.exit(1);
      return;
    }
    if (events[0]->address != 0x1abcde || events[0]->src != 1 || events[0]->size != 64 || events[0]->dat[63] != 63 ||
        events[1]->address != 0x132 || events[1]->src != 0 || events[1]->size != 8 || events[2]->size != 0 ||
        events[1]->mono_ns - events[0]->mono_ns != 50000) {
      app.exit(3);
      return;
    }
    for (size_t i = 1; i < events.size(); ++i) {
      if (events[i]->mono_ns < events[i - 1]->mono_ns) {
        app.exit(4);
        return;
      }
    }
    const auto& reordered = stream.events(MessageId(0, 0x203));
    if (reordered.size() != 2 || reordered[1]->mono_ns - reordered[0]->mono_ns != 5000 ||
        !stream.captureDetails().contains("Late CAN timestamps: 1")) {
      app.exit(25);
      return;
    }
    const auto status = stream.captureStatus();
    const auto details = stream.captureDetails();
    const auto counters = stream.captureCounters();
    if (status != "USB 0:6 1:3" || counters.queue_drops != 1 || counters.missing_frames != 1 || counters.fifo_overflow_events[0] != 2 ||
        counters.fifo_overflow_events[1] != 0 ||
        counters.invalid_records != 2 || !details.contains("device restarts: 1")) {
      app.exit(5);
      return;
    }
    const auto latency = stream.txLatency(1);
    if (stream.txLatency(0) || !latency || latency->count != 5 ||
        latency->rx_mean_us != 200 || latency->rx_max_us != 220 ||
        latency->submit_mean_us != 30 || latency->submit_max_us != 40 ||
        latency->wake_mean_us != 140 || latency->wake_max_us != 170 ||
        latency->drain_mean_us != 60 || latency->drain_max_us != 80 || latency->timeouts != 3 ||
        latency->ahead_mean != 4 || latency->ahead_max != 8 ||
        latency->tx_count != 7 || latency->tx_mean_us != 100 || latency->tx_max_us != 180 ||
        latency->arbitration_losses != 2 || latency->rx_to_tx_count != 4 || latency->rx_to_tx_mean_us != 330 || latency->rx_to_tx_max_us != 900 ||
        latency->same_fifo_mean != 1 || latency->same_fifo_max != 3 ||
        latency->other_fifo_mean != 3 || latency->other_fifo_max != 6) {
      qCritical("Automatic TX latency metrics were not decoded");
      app.exit(1);
      return;
    }
    const auto utilization = stream.utilization();
    if (!utilization || utilization->serial != 123 || utilization->cpu[0] != 234 ||
        utilization->cpu[1] != 345 || utilization->spi != 456 || utilization->can[0] != 567 ||
        utilization->can[1] != 678) {
      app.exit(14);
      return;
    }
    QWidget log_owner;
    CustomUsbSerialWidget::showFirmwareLogDialog(&log_owner, &stream);
    CustomUsbSerialWidget::showFirmwareLogDialog(&log_owner, &stream);
    const auto log_dialogs = log_owner.findChildren<QDialog*>(Qt::FindDirectChildrenOnly);
    if (log_dialogs.size() != 1 || log_dialogs.front()->isModal() ||
        log_dialogs.front()->windowTitle() != "Firmware Log") {
      app.exit(24);
      return;
    }
    auto* log_output = log_dialogs.front()->findChild<QPlainTextEdit*>();
    QEventLoop log_update;
    QTimer::singleShot(150, &log_update, &QEventLoop::quit);
    log_update.exec();
    if (!log_output || !log_output->isReadOnly() ||
        log_output->toPlainText() != QString::fromUtf8("I (1) dac: démarre\nI (2) can_mgr: ready\n")) {
      app.exit(25);
      return;
    }
    const int previous_metrics = std::exchange(settings.usb_status_metrics, 0);
    QWidget owner;
    CustomUsbSerialWidget::showDeviceMetricsDialog(&owner, &stream);
    CustomUsbSerialWidget::showDeviceMetricsDialog(&owner, &stream);
    const auto dialogs = owner.findChildren<QDialog*>(Qt::FindDirectChildrenOnly);
    if (dialogs.size() != 1 || !dialogs.front()->isVisible() || dialogs.front()->isModal() ||
        dialogs.front()->windowType() != Qt::Window ||
        dialogs.front()->windowFlags().testFlag(Qt::WindowStaysOnTopHint)) {
      settings.usb_status_metrics = previous_metrics;
      app.exit(15);
      return;
    }
    QStringList labels;
    for (auto* label : dialogs.front()->findChildren<QLabel*>()) labels.append(label->text());
    for (auto* checkbox : dialogs.front()->findChildren<QCheckBox*>()) {
      labels.append(checkbox->text());
      if (checkbox->text() == "Firmware Capture Queue Drops" || checkbox->text() == "Invalid USB Records")
        checkbox->setChecked(true);
    }
    const auto selected_status = stream.captureStatus(settings.usb_status_metrics);
    settings.usb_status_metrics = previous_metrics;
    if (dialogs.front()->windowTitle() != "USB Serial Device Metrics" ||
        !selected_status.endsWith(" | Capture drops:1  Invalid USB:2") ||
        !labels.contains("USB Frame Sequence Gaps") || !labels.contains("CAN Controller A FIFO Overflow") ||
        !labels.contains("CAN Controller B FIFO Overflow") ||
        !labels.contains("Invalid USB Records") || !labels.contains("SPI utilization (shared)") || !labels.contains("45.6%") ||
        !labels.contains("CAN bus 1 utilization") || !labels.contains("67.8%") ||
        std::any_of(labels.begin(), labels.end(), [](const QString& text) { return text.contains("RX → detection"); })) {
      app.exit(16);
      return;
    }
    bool old_diagnostics_hidden = false;
    QTimer::singleShot(0, &app, [&] {
      auto* send_dialog = qobject_cast<QDialog*>(app.activeModalWidget());
      if (!send_dialog) {
        for (auto* dialog : owner.findChildren<QDialog*>(Qt::FindDirectChildrenOnly))
          if (dialog->isModal()) dialog->reject();
        return;
      }
      const auto diagnostics_hidden = [&] {
        const auto labels = send_dialog->findChildren<QLabel*>();
        const auto buttons = send_dialog->findChildren<QToolButton*>();
        return std::none_of(labels.begin(), labels.end(), [](const QLabel* label) {
                 return label->isVisible() && label->text().contains("Frame start to successful transmission");
               }) &&
               std::none_of(buttons.begin(), buttons.end(), [](const QToolButton* button) {
                 return button->isVisible() && button->text() == "More info";
               });
      };
      old_diagnostics_hidden = diagnostics_hidden();
      for (auto* mode : send_dialog->findChildren<QComboBox*>()) {
        const int automatic_index = mode->findText("Automatic Retransmission");
        if (automatic_index < 0) continue;
        mode->setCurrentIndex(automatic_index);
        const auto buttons = send_dialog->findChildren<QToolButton*>();
        const auto more_info = std::find_if(buttons.begin(), buttons.end(), [](const QToolButton* button) {
          return button->isVisible() && button->text() == "More info";
        });
        if (more_info == buttons.end()) {
          old_diagnostics_hidden = false;
        } else {
          (*more_info)->click();
          const auto labels = send_dialog->findChildren<QLabel*>();
          old_diagnostics_hidden &= std::any_of(labels.begin(), labels.end(), [](const QLabel* label) {
            return label->isVisible() && label->text().contains("Start automatic retransmission to collect timing data.");
          });
        }
        mode->setCurrentIndex(0);
        old_diagnostics_hidden &= diagnostics_hidden();
      }
      send_dialog->reject();
    });
    CustomUsbSerialWidget::showSendSignalDialog(&owner, &stream, MessageId(1, 0x22222), "Test Signal");
    if (!old_diagnostics_hidden) {
      app.exit(26);
      return;
    }
    if (!QDir(QString::fromUtf8(argv[2])).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
      std::cerr << "Live USB wrote files unexpectedly\n";
      app.exit(6);
      return;
    }
    app.quit();
  });
  return app.exec();
}
