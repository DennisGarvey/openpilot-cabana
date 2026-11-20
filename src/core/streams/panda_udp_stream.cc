#include "panda_udp_stream.h"

#include <QHostInfo>
#include <QScopeGuard>
#include <QThread>
#include <QtEndian>

#include <cerrno>
#include <chrono>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace {
constexpr int kFrameSize = 16;
constexpr int kHeaderSize = 8;
constexpr int kMaxPayload = 8;
constexpr char kSubscribe = '\x0c';
}

PandaUdpStream::PandaUdpStream(QObject* parent, const PandaUdpConfig& config)
    : LiveStream(parent), config_(config) {
  auto address = QHostAddress(config_.host);
  if (address.isNull()) {
    const auto addresses = QHostInfo::fromName(config_.host).addresses();
    const auto ipv4 = std::ranges::find_if(addresses, [](const QHostAddress& candidate) {
      return candidate.protocol() == QAbstractSocket::IPv4Protocol;
    });
    if (ipv4 != addresses.end()) address = *ipv4;
  }
  if (address.protocol() != QAbstractSocket::IPv4Protocol) {
    throw std::runtime_error("Panda UDP requires an IPv4 address or a hostname with an IPv4 address");
  }

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "Creating UDP socket");
  auto close_on_error = qScopeGuard([fd] { ::close(fd); });

  const int buffer_size = 1 << 20;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_port = htons(config_.local_port);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) {
    throw std::system_error(errno, std::generic_category(), "Binding local UDP port");
  }

  sockaddr_in peer = {};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(config_.port);
  peer.sin_addr.s_addr = htonl(address.toIPv4Address());
  // A connected UDP socket accepts datagrams only from the selected host and port.
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) < 0) {
    throw std::system_error(errno, std::generic_category(), "Setting Panda UDP peer");
  }
  socket_fd_ = fd;
  close_on_error.dismiss();
}

PandaUdpStream::~PandaUdpStream() {
  stop();
  ::close(socket_fd_);
}

QString PandaUdpStream::routeName() const {
  return QStringLiteral("Panda UDP: %1:%2").arg(config_.host).arg(config_.port);
}

void PandaUdpStream::streamThread() {
  using Clock = std::chrono::steady_clock;
  auto heartbeat = Clock::now();
  char datagram[65536];
  pollfd socket_poll = {socket_fd_, POLLIN, 0};
  while (!QThread::currentThread()->isInterruptionRequested()) {
    const auto now = Clock::now();
    if (now >= heartbeat) {
      ::send(socket_fd_, "ehllo", 5, 0);
      ::send(socket_fd_, &kSubscribe, 1, 0);
      heartbeat = now + std::chrono::seconds(5);
    }
    const int ready = ::poll(&socket_poll, 1, 250);
    if (ready < 0 && errno != EINTR) break;
    if (ready > 0) {
      const auto received = ::recv(socket_fd_, datagram, sizeof(datagram), MSG_DONTWAIT);
      if (received > 0) handleDatagram(datagram, received);
    }
  }
  ::send(socket_fd_, "bye", 3, 0);
}

void PandaUdpStream::handleDatagram(const char* data, size_t size) {
  if (size % kFrameSize != 0) return;

  struct Frame {
    uint32_t address;
    uint8_t bus;
    uint8_t length;
    const char* payload;
  };
  std::vector<Frame> frames;
  frames.reserve(size / kFrameSize);
  bool acknowledged = false;
  for (size_t offset = 0; offset < size; offset += kFrameSize) {
    const auto rir = qFromLittleEndian<uint32_t>(data + offset);
    const auto flags = qFromLittleEndian<uint32_t>(data + offset + 4);
    const uint8_t length = flags & 0xf;
    if (length > kMaxPayload) return;
    const uint8_t bus = (flags >> 4) & 0xff;
    const uint32_t address = rir >> ((rir & 4) ? 3 : 21);
    if (bus == 15 && address == 6) {
      acknowledged = true;
    } else {
      frames.push_back({address, bus, length, data + offset + kHeaderSize});
    }
  }
  if (acknowledged) ::send(socket_fd_, &kSubscribe, 1, 0);
  if (frames.empty()) return;

  MessageBuilder message;
  auto can_data = message.initEvent().initCan(frames.size());
  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    can_data[i].setAddress(frame.address);
    can_data[i].setSrc(frame.bus);
    can_data[i].setDat(kj::arrayPtr(reinterpret_cast<const uint8_t*>(frame.payload), frame.length));
  }
  handleEvent(capnp::messageToFlatArray(message));
}
