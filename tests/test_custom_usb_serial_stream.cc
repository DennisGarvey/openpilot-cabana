#include <QApplication>
#include <QDir>
#include <QTimer>
#include <iostream>

#include "core/streams/custom_usb_serial_stream.h"
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
  bool sent = false;
  QTimer send_timer;
  QObject::connect(&send_timer, &QTimer::timeout, &app, [&] {
    if (!stream.txReady()) return;
    send_timer.stop();
    if ((automatic ? stream.automaticSupported() : stream.repeatSupported()) != repeat) {
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
    if (repeat) timing = CustomUsbSerialStream::RepeatTiming{automatic ? 0u : 5u, 1000, automatic};
    QByteArray signal_mask(64, 0);
    signal_mask[0] = char(0xff);
    signal_mask[1] = char(0xf0);
    signal_mask[63] = 1;
    sent = stream.sendFrame(multiplexed_id.source, multiplexed_id.address, matching->flags,
                            matching->data, matching->boot_id, timing, signal_mask);
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
    if (!sent || stream.repeatPending() || !stream.txStatus().contains(expected_status)) {
      app.exit(9);
      return;
    }
    stream.stop();
    std::cout << stream.captureStatus().toStdString() << '\n' << stream.captureDetails().toStdString() << '\n';
    const auto& events = stream.allEvents();
    const size_t expected_events = 8;
    if (events.size() != expected_events) {
      std::cerr << "Expected " << expected_events << " frames, got " << events.size() << '\n';
      app.exit(1);
      return;
    }
    if (events[0]->address != 0x132 || events[0]->src != 0 || events[0]->size != 8 || events[1]->address != 0x1abcde ||
        events[1]->src != 1 || events[1]->size != 64 || events[1]->dat[63] != 63 || events[2]->size != 0) {
      app.exit(3);
      return;
    }
    for (size_t i = 1; i < events.size(); ++i) {
      if (events[i]->mono_ns < events[i - 1]->mono_ns) {
        app.exit(4);
        return;
      }
    }
    const auto status = stream.captureStatus();
    const auto details = stream.captureDetails();
    if (!status.contains("gaps:1") || !status.contains("bad:2") || !status.contains("Q:1") ||
        !status.contains("FIFO:2") || !details.contains("device restarts: 1")) {
      app.exit(5);
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
