#pragma once

#include <QStringList>
#include <map>

struct StartupDbcs {
  QString global_file;
  std::map<int, QString> bus_files;
};

StartupDbcs parseStartupDbcs(const QStringList& arguments);
