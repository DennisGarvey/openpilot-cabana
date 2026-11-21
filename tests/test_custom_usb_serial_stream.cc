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
  QTimer::singleShot(2200, &app, [&] {
    stream.stop();
    std::cout << stream.captureStatus().toStdString() << '\n' << stream.captureDetails().toStdString() << '\n';
    const auto& events = stream.allEvents();
    if (events.size() != 6) {
      std::cerr << "Expected 6 frames, got " << events.size() << '\n';
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
