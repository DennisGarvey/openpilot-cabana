#pragma once

#include "live_stream.h"

struct PandaUdpConfig {
  QString host;
  uint16_t port = 1338;
  uint16_t local_port = 0;
};

class PandaUdpStream : public LiveStream {
 public:
  PandaUdpStream(QObject* parent, const PandaUdpConfig& config);
  ~PandaUdpStream() override;
  QString routeName() const override;

 private:
  void streamThread() override;
  void handleDatagram(const char* data, size_t size);

  PandaUdpConfig config_;
  int socket_fd_ = -1;
};
