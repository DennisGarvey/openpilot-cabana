#include "startup_dbcs.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <stdexcept>

StartupDbcs parseStartupDbcs(const QStringList& arguments) {
  static const QRegularExpression bus_prefix(R"(^([0-9]+):(.*)$)");
  StartupDbcs result;
  for (const auto& argument : arguments) {
    const auto match = bus_prefix.match(argument);
    QString filename = argument;
    int bus = -1;
    if (match.hasMatch()) {
      bool ok = false;
      bus = match.captured(1).toInt(&ok);
      if (!ok || bus > 63) throw std::invalid_argument("DBC bus must be between 0 and 63");
      filename = match.captured(2);
      if (result.bus_files.contains(bus)) {
        throw std::invalid_argument(QString("DBC bus %1 is specified more than once").arg(bus).toStdString());
      }
    } else if (!result.global_file.isEmpty()) {
      throw std::invalid_argument("Use --dbc BUS:FILE to assign multiple DBC files to separate buses");
    }
    const QFileInfo file(filename);
    if (filename.isEmpty() || !file.isFile() || !file.isReadable()) {
      throw std::invalid_argument(QString("Cannot read DBC file: %1").arg(filename).toStdString());
    }
    if (bus < 0) {
      result.global_file = file.absoluteFilePath();
    } else {
      result.bus_files.emplace(bus, file.absoluteFilePath());
    }
  }
  return result;
}
