#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

#include "mainwin.h"
#include "modules/dbc/startup_dbcs.h"
#include "modules/system/stream_manager.h"

int main(int argc, char** argv) {
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QApplication app(argc, argv);
  QTemporaryDir directory(QDir::currentPath() + "/build/startup-dbcs-XXXXXX");
  if (!directory.isValid()) return 1;
  auto fixture = [&](const QString& filename, const QString& message) {
    const auto path = directory.filePath(filename);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) std::abort();
    file.write(QString("VERSION \"\"\nNS_ :\nBS_:\nBU_: NODE\nBO_ 42 %1: 8 NODE\n").arg(message).toUtf8());
    return path;
  };
  const auto chassis = fixture("chassis:local.dbc", "ChassisMessage");
  const auto vehicle = fixture("vehicle signals.dbc", "VehicleMessage");
  const auto fallback = fixture("fallback.dbc", "FallbackMessage");
  const auto parsed = parseStartupDbcs({"1:" + vehicle, fallback, "0:" + chassis});
  if (parsed.global_file != fallback || parsed.bus_files.at(0) != chassis || parsed.bus_files.at(1) != vehicle)
    return 2;
  for (const QStringList invalid : {QStringList{"64:" + chassis},
                                    {"999999999999999999:" + chassis},
                                    {"0:"},
                                    {"0:" + chassis, "0:" + vehicle},
                                    {chassis, vehicle},
                                    {directory.filePath("missing.dbc")}}) {
    try {
      parseStartupDbcs(invalid);
      std::cerr << "Accepted invalid DBC arguments\n";
      return 3;
    } catch (const std::invalid_argument&) {
    }
  }
  MainWindow window(new DummyStream(&app), parsed.global_file, parsed.bus_files);
  QTimer::singleShot(100, &app, [&] {
    auto matches = [](int bus, const QString& expected) {
      const auto* message = GetDBC()->msg({static_cast<uint8_t>(bus), 42});
      return message && message->name == expected;
    };
    const bool ok = matches(0, "ChassisMessage") && matches(1, "VehicleMessage") && matches(2, "FallbackMessage") &&
                    matches(128, "ChassisMessage") && matches(192, "ChassisMessage") &&
                    matches(129, "VehicleMessage") && matches(193, "VehicleMessage");
    std::cout << (ok ? "Startup DBC assignments: separate buses, aliases, global fallback and input validation passed\n"
                     : "Startup DBC assignments failed\n");
    StreamManager::instance().shutdown();
    app.exit(ok ? 0 : 4);
  });
  return app.exec();
}
