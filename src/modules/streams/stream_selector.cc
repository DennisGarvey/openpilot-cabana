#include "stream_selector.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "modules/settings/settings.h"
#include "asc_log.h"
#include "candump_log.h"
#include "device.h"
#include "custom_usb_serial.h"
#include "panda.h"
#include "route.h"
#include "socketcan.h"
#include "trc_log.h"

StreamSelector::StreamSelector(QWidget* parent, const QString& initial_dbc)
    : QDialog(parent), dbc_file_overridden_(!initial_dbc.isEmpty()) {
  setWindowTitle(tr("Open stream"));
  setMinimumWidth(620);
  QVBoxLayout* layout = new QVBoxLayout(this);
  tab = new QTabWidget(this);
  tab->setUsesScrollButtons(false);
  layout->addWidget(tab);

  QHBoxLayout* dbc_layout = new QHBoxLayout();
  dbc_file = new QLineEdit(this);
  dbc_file->setReadOnly(true);
  dbc_file->setPlaceholderText(tr("Choose a dbc file to open"));
  dbc_file->setText(initial_dbc);
  QPushButton* browse_btn = new QPushButton(tr("Browse..."));
  QPushButton* clear_btn = new QPushButton(tr("Clear"));
  clear_btn->setEnabled(!initial_dbc.isEmpty());
  dbc_layout->addWidget(new QLabel(tr("dbc File")));
  dbc_layout->addWidget(dbc_file);
  dbc_layout->addWidget(browse_btn);
  dbc_layout->addWidget(clear_btn);
  connect(dbc_file, &QLineEdit::textChanged, clear_btn, [clear_btn](const QString& text) {
    clear_btn->setEnabled(!text.isEmpty());
  });
  connect(clear_btn, &QPushButton::clicked, this, [this] {
    dbc_file_overridden_ = true;
    dbc_file->clear();
  });
  layout->addLayout(dbc_layout);

  QFrame* line = new QFrame(this);
  line->setFrameStyle(QFrame::HLine | QFrame::Sunken);
  layout->addWidget(line);

  btn_box = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel);
  layout->addWidget(btn_box);

  tab->setFocusPolicy(Qt::TabFocus);
  connect(tab, &QTabWidget::currentChanged, [this](int index) {
    if (QWidget* w = tab->widget(index)) {
      w->setFocus();
    }
  });

  addStreamWidget(new RouteWidget, tr("&Route"));
  auto* asc_log = new AscLogWidget;
  addStreamWidget(asc_log, tr("&ASC Log"));
  addStreamWidget(new CandumpLogWidget, tr("&candump"));
  addStreamWidget(new TrcLogWidget, tr("&TRC"));
  addStreamWidget(new PandaWidget, tr("&Panda"));
  if (SocketCanStream::available()) {
    addStreamWidget(new SocketCanWidget, tr("&SocketCAN"));
  }
  addStreamWidget(new DeviceWidget, tr("&Device"));
  addStreamWidget(new CustomUsbSerialWidget, tr("Custom USB &Serial"));

  if (settings.last_stream_option >= 0 && settings.last_stream_option < tab->count()) {
    tab->setCurrentIndex(settings.last_stream_option);
  }

  const auto updateRememberedDbc = [this, asc_log] {
    if (dbc_file_overridden_) return;
    if (tab->currentWidget() == asc_log) {
      for (const QString& path : settings.recent_files) {
        const QFileInfo file(path);
        if (file.isFile() && file.isReadable()) {
          dbc_file->setText(file.absoluteFilePath());
          return;
        }
      }
    }
    dbc_file->clear();
  };
  connect(tab, &QTabWidget::currentChanged, this, updateRememberedDbc);
  updateRememberedDbc();

  connect(btn_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(btn_box, &QDialogButtonBox::accepted, [=]() {
    setEnabled(false);
    if (stream_ = ((AbstractStreamWidget*)tab->currentWidget())->open(); stream_) {
      settings.last_stream_option = tab->currentIndex();
      accept();
    }
    setEnabled(true);
  });
  connect(browse_btn, &QPushButton::clicked, [this]() {
    QString fn = QFileDialog::getOpenFileName(this, tr("Open File"), settings.last_dir, "DBC (*.dbc)");
    if (!fn.isEmpty()) {
      dbc_file_overridden_ = true;
      dbc_file->setText(fn);
      settings.last_dir = QFileInfo(fn).absolutePath();
    }
  });
}

void StreamSelector::addStreamWidget(AbstractStreamWidget* w, const QString& title) {
  tab->addTab(w, title);
  auto open_btn = btn_box->button(QDialogButtonBox::Open);
  connect(w, &AbstractStreamWidget::enableOpenButton, open_btn, &QPushButton::setEnabled);
}
