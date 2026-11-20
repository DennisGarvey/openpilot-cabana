#include "panda.h"

#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QFormLayout>
#include <QIntValidator>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include "core/streams/panda_udp_stream.h"
#include "modules/settings/settings.h"
#include "modules/system/stream_manager.h"

namespace {
enum Transport { Serial, Udp };
void reserveWhenHidden(QWidget* widget) {
  auto policy = widget->sizePolicy();
  policy.setRetainSizeWhenHidden(true);
  widget->setSizePolicy(policy);
}
}

PandaWidget::PandaWidget(QWidget* parent) : AbstractStreamWidget(parent) {
  auto* main_layout = new QVBoxLayout(this);
  main_layout->addStretch(1);
  auto* content = new QWidget(this);
  auto* content_layout = new QVBoxLayout(content);
  content_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->addWidget(content, 0, Qt::AlignHCenter);
  main_layout->addStretch(1);

  auto* current_stream = StreamManager::stream();
  if (dynamic_cast<PandaStream*>(current_stream) || dynamic_cast<PandaUdpStream*>(current_stream)) {
    content_layout->addWidget(new QLabel(tr("Already connected to %1.").arg(current_stream->routeName())));
    content_layout->addWidget(
        new QLabel(tr("Close the current stream before connecting to another Panda.")));
    return;
  }

  transport = new QTabBar(content);
  transport->setAccessibleName(tr("Panda connection"));
  transport->setExpanding(false);
  transport->setDrawBase(false);
  transport->addTab(tr("Serial"));
  transport->addTab(tr("UDP (v2)"));
  content_layout->addWidget(transport, 0, Qt::AlignHCenter);
  auto* pages = new QStackedWidget(content);
  content_layout->addWidget(pages);

  const auto makeForm = [](QWidget* page) {
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    return form;
  };
  const QStringList labels = {tr("Serial"), tr("Host"), tr("Device port"), tr("Receive port"),
                              tr("Bus %1:").arg(2)};
  int label_width = 0;
  for (const QString& label : labels) {
    label_width = std::max(label_width, fontMetrics().horizontalAdvance(label));
  }
  const auto fieldLabel = [label_width](const QString& text) {
    auto* label = new QLabel(text);
    label->setMinimumWidth(label_width);
    return label;
  };

  auto* serial_page = new QWidget(pages);
  auto* serial_form = makeForm(serial_page);
  auto* serial_layout = new QHBoxLayout;
  serial_layout->addWidget(serial_edit = new QComboBox);
  serial_edit->setMinimumWidth(200);
  auto* refresh = new QPushButton(tr("Refresh"));
  serial_layout->addWidget(refresh);
  serial_layout->addStretch();
  serial_form->addRow(fieldLabel(tr("Serial")), serial_layout);
  pages->addWidget(serial_page);

  serial_options = new QStackedWidget(serial_page);
  auto* empty = new QWidget(serial_options);
  auto* empty_layout = new QVBoxLayout(empty);
  empty_layout->setContentsMargins(0, 0, 0, 0);
  empty_layout->addWidget(new QLabel(tr("No Panda found")), 0, Qt::AlignTop);
  empty_layout->addStretch(1);
  serial_options->addWidget(empty);

  auto* buses = new QWidget(serial_options);
  auto* bus_form = makeForm(buses);
  config.bus_config.resize(3);
  for (size_t i = 0; i < config.bus_config.size(); ++i) {
    auto* bus_layout = new QHBoxLayout;
    bus_layout->addWidget(new QLabel(tr("CAN Speed (kbps):")));
    auto* can_speed = new QComboBox;
    for (uint32_t speed : speeds) can_speed->addItem(QString::number(speed));
    can_speed->setCurrentText(QString::number(config.bus_config[i].can_speed_kbps));
    connect(can_speed, &QComboBox::currentIndexChanged, this,
            [this, i](int index) { config.bus_config[i].can_speed_kbps = speeds[index]; });
    bus_layout->addWidget(can_speed);

    auto* fd = new QWidget(buses);
    auto* fd_layout = new QHBoxLayout(fd);
    fd_layout->setContentsMargins(0, 0, 0, 0);
    auto* enable_fd = new QCheckBox(tr("CAN-FD"));
    fd_layout->addWidget(enable_fd);
    fd_layout->addWidget(new QLabel(tr("Data Speed (kbps):")));
    auto* data_speed = new QComboBox;
    for (uint32_t speed : data_speeds) data_speed->addItem(QString::number(speed));
    data_speed->setCurrentText(QString::number(config.bus_config[i].data_speed_kbps));
    data_speed->setEnabled(false);
    fd_layout->addWidget(data_speed);
    connect(data_speed, &QComboBox::currentIndexChanged, this,
            [this, i](int index) { config.bus_config[i].data_speed_kbps = data_speeds[index]; });
    connect(enable_fd, &QCheckBox::toggled, data_speed, &QComboBox::setEnabled);
    connect(enable_fd, &QCheckBox::toggled, this, [this, i](bool enabled) { config.bus_config[i].can_fd = enabled; });
    reserveWhenHidden(fd);
    fd_controls.push_back(fd);
    bus_layout->addWidget(fd);
    bus_form->addRow(fieldLabel(tr("Bus %1:").arg(i)), bus_layout);
  }
  serial_options->addWidget(buses);
  serial_form->addRow(serial_options);

  auto* udp_page = new QWidget(pages);
  auto* udp_form = makeForm(udp_page);
  pages->addWidget(udp_page);
  udp_host = new QLineEdit(settings.last_panda_udp_host, this);
  udp_host->setPlaceholderText(tr("IP address or hostname"));
  udp_host->setMinimumWidth(260);
  udp_port = new QLineEdit(QString::number(settings.last_panda_udp_port), this);
  udp_port->setValidator(new QIntValidator(1, 65535, udp_port));
  udp_port->setFixedWidth(110);
  fixed_receive_port = new QCheckBox(tr("Use a fixed receive port"), this);
  fixed_receive_port->setChecked(settings.last_panda_udp_local_port > 0);
  fixed_receive_port->setToolTip(tr("Cabana chooses an available receive port automatically by default."));
  const int local_port = settings.last_panda_udp_local_port;
  udp_local_port = new QLineEdit(QString::number(local_port > 0 ? local_port : 1338), this);
  udp_local_port->setValidator(new QIntValidator(1, 65535, udp_local_port));
  udp_local_port->setFixedWidth(110);
  udp_form->addRow(fieldLabel(tr("Host")), udp_host);
  udp_form->addRow(fieldLabel(tr("Device port")), udp_port);
  udp_form->addRow(fieldLabel({}), fixed_receive_port);
  auto* receive_label = fieldLabel(tr("Receive port"));
  udp_form->addRow(receive_label, udp_local_port);
  reserveWhenHidden(receive_label);
  reserveWhenHidden(udp_local_port);
  const auto showReceivePort = [this, receive_label](bool visible) {
    receive_label->setVisible(visible);
    udp_local_port->setVisible(visible);
  };
  connect(fixed_receive_port, &QCheckBox::toggled, this, showReceivePort);
  showReceivePort(fixed_receive_port->isChecked());

  connect(refresh, &QPushButton::clicked, this, &PandaWidget::refreshSerials);
  connect(serial_edit, &QComboBox::currentTextChanged, this, &PandaWidget::updateSerialConfig);
  const auto showTransport = [this, pages](int index) {
    if (index == Serial && serial_edit->count() == 0) refreshSerials();
    pages->setCurrentIndex(index);
  };
  transport->setCurrentIndex(settings.last_panda_udp ? Udp : Serial);
  connect(transport, &QTabBar::currentChanged, this, showTransport);
  showTransport(transport->currentIndex());
  setFocusProxy(transport);
}

void PandaWidget::refreshSerials() {
  const QSignalBlocker blocker(serial_edit);
  const QString previous = serial_edit->currentText();
  serial_edit->clear();
  for (auto serial : Panda::list()) {
    serial_edit->addItem(QString::fromStdString(serial));
  }
  serial_edit->setEnabled(serial_edit->count() > 0);
  const QString preferred = previous.isEmpty() ? settings.last_panda_serial : previous;
  if (!preferred.isEmpty()) {
    int idx = serial_edit->findText(preferred);
    if (idx != -1) {
      serial_edit->setCurrentIndex(idx);
    }
  }
  updateSerialConfig();
}

void PandaWidget::updateSerialConfig() {
  const QString serial = serial_edit->currentText();
  bool has_fd = false;
  bool has_panda = !serial.isEmpty();
  if (has_panda) {
    try {
      Panda panda(serial.toStdString());
      has_fd = (panda.hw_type == cereal::PandaState::PandaType::RED_PANDA) ||
               (panda.hw_type == cereal::PandaState::PandaType::RED_PANDA_V2);
    } catch (const std::exception&) {
      qDebug() << "failed to open panda" << serial;
      has_panda = false;
    }
  }
  config.serial = has_panda ? serial : QString();
  for (auto* controls : fd_controls) controls->setVisible(has_fd);
  serial_options->setCurrentIndex(has_panda ? 1 : 0);
}

AbstractStream* PandaWidget::open() {
  if (!transport) {
    QMessageBox::warning(this, tr("Panda already connected"), tr("Close the current stream first."));
    return nullptr;
  }
  try {
    if (transport->currentIndex() == Udp) {
      const QString host = udp_host->text().trimmed();
      if (host.isEmpty()) {
        QMessageBox::warning(this, tr("Missing host"), tr("Enter a Panda UDP host."));
        return nullptr;
      }
      const bool use_fixed_port = fixed_receive_port->isChecked();
      if (!udp_port->hasAcceptableInput() || (use_fixed_port && !udp_local_port->hasAcceptableInput())) {
        QMessageBox::warning(this, tr("Invalid port"), tr("Enter a port from 1 to 65535."));
        return nullptr;
      }
      const auto device_port = static_cast<uint16_t>(udp_port->validator()->locale().toUInt(udp_port->text()));
      const uint16_t receive_port = use_fixed_port
          ? udp_local_port->validator()->locale().toUShort(udp_local_port->text()) : 0;
      const PandaUdpConfig udp_config{host, device_port, receive_port};
      auto* stream = new PandaUdpStream(qApp, udp_config);
      settings.last_panda_udp = true;
      settings.last_panda_udp_host = host;
      settings.last_panda_udp_port = device_port;
      settings.last_panda_udp_local_port = receive_port;
      return stream;
    }
    if (config.serial.isEmpty()) {
      QMessageBox::warning(this, tr("No Panda found"), tr("Connect a Panda and click Refresh."));
      return nullptr;
    }
    auto stream = new PandaStream(qApp, config);
    settings.last_panda_udp = false;
    settings.last_panda_serial = config.serial;
    return stream;
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("Panda connection failed"), QString::fromLocal8Bit(e.what()));
    return nullptr;
  }
}
