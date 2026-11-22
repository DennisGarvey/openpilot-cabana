#include "mainwin.h"

#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QScreen>
#include <QShortcut>
#include <QTimer>
#include <QUndoView>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "core/streams/custom_usb_serial_stream.h"
#include "core/streams/device_stream.h"
#include "core/streams/file_stream.h"
#include "core/commands/commands.h"
#include "modules/dbc/dbc_controller.h"
#include "modules/dbc/export.h"
#include "modules/settings/settings_dialog.h"
#include "modules/streams/custom_usb_serial.h"
#include "modules/streams/stream_selector.h"
#include "modules/streams/zmq_load_dialog.h"
#include "modules/system/stream_manager.h"
#include "modules/system/system_relay.h"
#include "replay/include/http.h"
#include "tools/findsignal.h"
#include "widgets/guide_overlay.h"

static QStringList streamLayoutKey(const AbstractStream& stream) {
  QStringList key{QString::fromLatin1(stream.metaObject()->className())};
  if (const auto* files = dynamic_cast<const FileStream*>(&stream)) {
    for (const auto& path : files->filePaths()) {
      const QFileInfo file(path);
      const auto canonical = file.canonicalFilePath();
      key.append(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
    }
  } else {
    key.append(stream.routeName());
  }
  return key;
}

MainWindow::MainWindow(AbstractStream* stream, const QString& dbc_file, const std::map<int, QString>& bus_dbcs)
    : QMainWindow() {
  dbc_controller_ = new DbcController(this);
  charts_panel = new ChartsPanel(this);
  inspector_widget_ = new MessageInspector(charts_panel, this);
  setCentralWidget(inspector_widget_);

  status_bar_ = new StatusBar(this);
  setStatusBar(status_bar_);
  setupDocks();
  setupMenus();
  createShortcuts();

  // save default window state to allow resetting it
  default_window_state_ = saveState();

  // restore states
  if (!settings.geometry.isEmpty()) restoreGeometry(settings.geometry);
  if (!settings.window_state.isEmpty()) restoreState(settings.window_state);
  if (isMaximized()) setGeometry(screen()->availableGeometry());

  setupConnections();

  QTimer::singleShot(0, this, [this, stream, dbc_file, bus_dbcs]() {
    for (const auto& [bus, filename] : bus_dbcs) {
      dbc_controller_->loadFile(filename, {bus, bus + 128, bus + 192});
    }
    stream ? openStream(stream, dbc_file) : selectAndOpenStream(dbc_file);
    if (!bus_dbcs.empty()) DBCFileChanged();
  });
  show();
}

void MainWindow::setupConnections() {
  auto& relay = SystemRelay::instance();
  relay.installGlobalHandlers();

  connect(&relay, &SystemRelay::logMessage, status_bar_, &StatusBar::showMessage);
  connect(&relay, &SystemRelay::downloadProgress, status_bar_, &StatusBar::updateDownloadProgress);
  connect(&settings, &Settings::changed, status_bar_, &StatusBar::updateMetrics);
  restore_session_timer_ = new QTimer(this);
  restore_session_timer_->setSingleShot(true);
  connect(restore_session_timer_, &QTimer::timeout, this, &MainWindow::restoreSessionState);
  connect(GetDBC(), &dbc::Manager::DBCFileChanged, this, &MainWindow::DBCFileChanged);
  connect(UndoStack::instance(), &QUndoStack::cleanChanged, this, &MainWindow::undoStackCleanChanged);
  connect(&StreamManager::instance(), &StreamManager::streamChanged, this, &MainWindow::onStreamChanged);
  connect(&StreamManager::instance(), &StreamManager::eventsMerged, this, &MainWindow::eventsMerged);
  connection_rate_timer_ = new QTimer(this);
  connection_rate_timer_->setInterval(1000);
  connect(connection_rate_timer_, &QTimer::timeout, this, &MainWindow::updateConnectionRate);

  connect(charts_panel, &ChartsPanel::openMessage, message_list_, &MessageList::selectMessage);
  connect(message_list_, &MessageList::nameFilterChanged, inspector_widget_->getMessageView(),
          &MessageView::setMessageNameFilter);
  connect(inspector_widget_->getMessageView(), &MessageView::activeMessageChanged, message_list_,
          &MessageList::selectMessage);
  connect(inspector_widget_->getMessageView(), &MessageView::lastTabClosed, message_list_,
          &MessageList::clearMessageSelection);
}

void MainWindow::setupMenus() {
  createFileMenu();
  createEditMenu();
  createViewMenu();
  createToolsMenu();
  createHelpMenu();
}

void MainWindow::createFileMenu() {
  QMenu* file_menu = menuBar()->addMenu(tr("&File"));
  open_stream_act_ = file_menu->addAction(tr("Open Stream..."), this, [this] { selectAndOpenStream(); },
                                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
  open_stream_act_->setAutoRepeat(false);
  close_stream_act_ = file_menu->addAction(tr("Close stream"), this, &MainWindow::closeStream,
                                           QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
  close_stream_act_->setAutoRepeat(false);
  export_to_csv_act_ = file_menu->addAction(tr("Export to CSV..."), this, &MainWindow::exportToCSV);
  close_stream_act_->setEnabled(false);
  export_to_csv_act_->setEnabled(false);
  file_menu->addSeparator();

  file_menu->addAction(tr("New DBC File"), [this]() { dbc_controller_->newFile(); }, QKeySequence::New);
  file_menu->addAction(tr("Open DBC File..."), [this]() { dbc_controller_->openFile(); }, QKeySequence::Open);

  manage_dbcs_menu_ = file_menu->addMenu(tr("Manage &DBC Files"));
  connect(manage_dbcs_menu_, &QMenu::aboutToShow, this,
          [this]() { dbc_controller_->populateManageMenu(manage_dbcs_menu_); });

  recent_files_menu_ = file_menu->addMenu(tr("Open &Recent"));
  connect(recent_files_menu_, &QMenu::aboutToShow, this,
          [this]() { dbc_controller_->populateRecentMenu(recent_files_menu_); });

  file_menu->addSeparator();
  QMenu* load_opendbc_menu = file_menu->addMenu(tr("Load DBC from commaai/opendbc"));
  dbc_controller_->populateOpendbcFiles(load_opendbc_menu);

  file_menu->addAction(tr("Load DBC From Clipboard"), [=]() { dbc_controller_->loadFromClipboard(); });

  file_menu->addSeparator();
  save_dbc_ = file_menu->addAction(tr("Save DBC..."), dbc_controller_, &DbcController::save, QKeySequence::Save);
  save_dbc_as_ =
      file_menu->addAction(tr("Save DBC As..."), dbc_controller_, &DbcController::saveAs, QKeySequence::SaveAs);
  copy_dbc_to_clipboard_ =
      file_menu->addAction(tr("Copy DBC To Clipboard"), dbc_controller_, &DbcController::saveToClipboard);

  file_menu->addSeparator();
  file_menu->addAction(tr("Settings..."), this, &MainWindow::setOption, QKeySequence::Preferences);

  file_menu->addSeparator();
  file_menu->addAction(tr("E&xit"), qApp, &QApplication::closeAllWindows, QKeySequence::Quit);
}

void MainWindow::createEditMenu() {
  QMenu* edit_menu = menuBar()->addMenu(tr("&Edit"));
  auto undo_act = UndoStack::instance()->createUndoAction(this, tr("&Undo"));
  undo_act->setShortcuts(QKeySequence::Undo);
  edit_menu->addAction(undo_act);
  auto redo_act = UndoStack::instance()->createRedoAction(this, tr("&Redo"));
  redo_act->setShortcuts(QKeySequence::Redo);
  edit_menu->addAction(redo_act);
  edit_menu->addSeparator();

  QMenu* commands_menu = edit_menu->addMenu(tr("Command &List"));
  QWidgetAction* commands_act = new QWidgetAction(this);
  QUndoView* view = new QUndoView(UndoStack::instance(), this);  // Parent set here
  view->setEmptyLabel(tr("No commands"));
  commands_act->setDefaultWidget(view);
  commands_menu->addAction(commands_act);
}

void MainWindow::createViewMenu() {
  QMenu* view_menu = menuBar()->addMenu(tr("&View"));
  auto act = view_menu->addAction(tr("Full Screen"), this, &MainWindow::toggleFullScreen, QKeySequence::FullScreen);
  addAction(act);
  view_menu->addSeparator();
  view_menu->addAction(messages_dock_->toggleViewAction());
  view_menu->addAction(video_dock_->toggleViewAction());
  view_menu->addSeparator();
  view_menu->addAction(tr("Reset Window Layout"), [this]() { restoreState(default_window_state_); });
}

void MainWindow::createToolsMenu() {
  tools_menu_ = menuBar()->addMenu(tr("&Tools"));
  tools_menu_->addAction(tr("Find &Similar Bits"), this, &MainWindow::findSimilarBits);
  tools_menu_->addAction(tr("&Find Signal"), this, &MainWindow::findSignal);
  auto* utilization_action = tools_menu_->addAction(tr("USB Stream Utilization…"));
  auto* tx_action = tools_menu_->addAction(tr("Send Frame…"));
  connect(tools_menu_, &QMenu::aboutToShow, this, [tx_action, utilization_action] {
    const bool custom_usb = dynamic_cast<CustomUsbSerialStream*>(StreamManager::stream()) != nullptr;
    utilization_action->setVisible(custom_usb);
    tx_action->setVisible(custom_usb);
  });
  connect(utilization_action, &QAction::triggered, this, [this] {
    if (auto* stream = dynamic_cast<CustomUsbSerialStream*>(StreamManager::stream())) {
      CustomUsbSerialWidget::showUtilizationDialog(this, stream);
    }
  });
  connect(tx_action, &QAction::triggered, this, [this] {
    if (auto* stream = dynamic_cast<CustomUsbSerialStream*>(StreamManager::stream())) {
      CustomUsbSerialWidget::showSendFrameDialog(this, stream);
    }
  });
}

void MainWindow::createHelpMenu() {
  QMenu* help_menu = menuBar()->addMenu(tr("&Help"));
  help_menu->addAction(tr("Help"), this, &MainWindow::onlineHelp, QKeySequence::HelpContents);
  help_menu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
}

void MainWindow::setupDocks() {
  createMessagesDock();
  createVideoChartsDock();
}

void MainWindow::createMessagesDock() {
  messages_dock_ = new QDockWidget(tr("MESSAGES"), this);
  messages_dock_->setObjectName("MessagesPanel");
  messages_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::TopDockWidgetArea |
                                  Qt::BottomDockWidgetArea);
  messages_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);

  message_list_ = new MessageList(this);
  messages_dock_->setWidget(message_list_);
  addDockWidget(Qt::LeftDockWidgetArea, messages_dock_);

  connect(message_list_, &MessageList::titleChanged, messages_dock_, &QDockWidget::setWindowTitle);
  connect(message_list_, &MessageList::msgSelectionChanged, inspector_widget_, &MessageInspector::setMessage);
}

void MainWindow::createVideoChartsDock() {
  video_dock_ = new QDockWidget("", this);
  video_dock_->setObjectName("VideoPanel");
  video_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  video_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);

  QWidget* charts_container = new QWidget(this);
  charts_layout_ = new QVBoxLayout(charts_container);
  charts_layout_->setContentsMargins(0, 0, 0, 0);
  charts_layout_->addWidget(charts_panel);

  // splitter between video and charts
  video_splitter_ = new PanelSplitter(Qt::Vertical, this);
  video_player_ = new VideoPlayer(this);
  video_splitter_->addWidget(video_player_);

  video_splitter_->addWidget(charts_container);
  video_splitter_->setStretchFactor(0, 0);
  video_splitter_->setStretchFactor(1, 1);

  video_dock_->setWidget(video_splitter_);
  addDockWidget(Qt::RightDockWidgetArea, video_dock_);

  connect(charts_panel, &ChartsPanel::toggleChartsDocking, this, &MainWindow::toggleChartsDocking);
  connect(charts_panel, &ChartsPanel::showCursor, video_player_, &VideoPlayer::showThumbnail);
}

void MainWindow::createShortcuts() {
  auto shortcut = new QShortcut(QKeySequence(Qt::Key_Space), this, nullptr, nullptr, Qt::ApplicationShortcut);
  connect(shortcut, &QShortcut::activated, this, []() {
    if (auto* stream = StreamManager::stream()) {
      stream->pause(!stream->isPaused());
    }
  });
  auto* message_view = inspector_widget_->getMessageView();
  auto* close_tab = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_W), inspector_widget_);
  auto* reopen_tab = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), inspector_widget_);
  close_tab->setAutoRepeat(false);
  reopen_tab->setAutoRepeat(false);
  connect(close_tab, &QShortcut::activated, message_view, &MessageView::closeCurrentTab);
  connect(reopen_tab, &QShortcut::activated, message_view, &MessageView::reopenClosedTab);
}

void MainWindow::onStreamChanged() {
  auto& manager = StreamManager::instance();
  const bool has_stream = manager.hasStream();
  restore_session_timer_->stop();
  open_stream_act_->setEnabled(!has_stream);
  close_stream_act_->setEnabled(has_stream);
  export_to_csv_act_->setEnabled(has_stream);
  tools_menu_->setEnabled(has_stream);
  manage_dbcs_menu_->setEnabled(has_stream);
  inspector_widget_->clear();
  if (auto* handle = video_splitter_->handle(1)) {
    handle->setEnabled(!manager.isLiveStream());
  }
  connection_rate_timer_->stop();
  if (manager.hasStream() && manager.isLiveStream()) {
    last_frame_count_ = manager.currentStream()->allEvents().size();
    connection_rate_clock_.start();
    video_dock_->setWindowTitle(tr("%1 · %L2 frames/s").arg(manager.currentStream()->routeName()).arg(0));
    connection_rate_timer_->start();
  } else {
    video_dock_->setWindowTitle(manager.hasStream() ? manager.currentStream()->routeName() : tr("Video"));
  }
}

void MainWindow::updateConnectionRate() {
  const auto* stream = StreamManager::stream();
  const size_t count = stream->allEvents().size();
  const qint64 elapsed_ms = connection_rate_clock_.restart();
  if (elapsed_ms <= 0) return;

  const size_t received = count >= last_frame_count_ ? count - last_frame_count_ : 0;
  last_frame_count_ = count;
  const auto rate = qRound64(1000.0 * received / elapsed_ms);
  video_dock_->setWindowTitle(tr("%1 · %L2 frames/s").arg(stream->routeName()).arg(rate));
}

void MainWindow::undoStackCleanChanged(bool clean) { setWindowModified(!clean); }

void MainWindow::DBCFileChanged() {
  UndoStack::instance()->clear();

  // Update file menu
  int cnt = GetDBC()->nonEmptyFileCount();
  save_dbc_->setText(cnt > 1 ? tr("Save %1 DBCs...").arg(cnt) : tr("Save DBC..."));
  save_dbc_->setEnabled(cnt > 0);
  save_dbc_as_->setEnabled(cnt == 1);
  // TODO: Support clipboard for multiple files
  copy_dbc_to_clipboard_->setEnabled(cnt == 1);
  manage_dbcs_menu_->setEnabled(StreamManager::instance().hasStream());

  QStringList title;
  for (const auto &f : GetDBC()->allFiles()) {
    title.push_back(tr("(%1) %2").arg(toString(GetDBC()->getSourcesForFile(f.get())), f->name()));
  }
  setWindowFilePath(title.join(" | "));

  restore_session_timer_->start(20);
}

void MainWindow::selectAndOpenStream(const QString& dbc_file) {
  auto& manager = StreamManager::instance();
  if (manager.hasStream() || !open_stream_act_->isEnabled()) return;

  open_stream_act_->setEnabled(false);
  StreamSelector dlg(this, dbc_file);
  if (dlg.exec()) {
    openStream(dlg.stream(), dlg.dbcFile());
  } else {
    dbc_controller_->loadFile(dbc_file);
  }
  open_stream_act_->setEnabled(!manager.hasStream());
}

void MainWindow::closeStream() {
  auto& manager = StreamManager::instance();
  if (!manager.hasStream()) return;

  auto [active_message, message_tabs] = inspector_widget_->getMessageView()->serializeMessageIds();
  stream_layouts_.insert_or_assign(streamLayoutKey(*manager.currentStream()),
                                   StreamLayout{active_message, message_tabs, charts_panel->saveLayout()});
  openStream(new DummyStream(this));
  statusBar()->showMessage(tr("stream closed"));
}

void MainWindow::exportToCSV() {
  auto* stream = StreamManager::stream();
  if (!stream) return;

  QString dir = QString("%1/%2.csv").arg(settings.last_dir).arg(stream->routeName());
  QString fn = QFileDialog::getSaveFileName(this, "Export stream to CSV file", dir, tr("csv (*.csv)"));
  if (!fn.isEmpty()) {
    exportMessagesToCSV(fn);
    QMessageBox::information(this, tr("Export"), tr("Data successfully exported to:\n%1").arg(fn));
  }
}

void MainWindow::openStream(AbstractStream* stream, const QString& dbc_file) {
  auto& sm = StreamManager::instance();
  if (sm.hasStream() && stream && !dynamic_cast<DummyStream*>(stream)) {
    if (stream != sm.currentStream()) stream->deleteLater();
    return;
  }
  sm.setStream(stream, dbc_file);
  dbc_controller_->loadFile(dbc_file);

  const bool has_stream = sm.hasStream();
  const bool is_live_stream = sm.isLiveStream();

  if (is_live_stream || video_splitter_->sizes()[0] == 0) {
    // display video at minimum size.
    video_splitter_->setSizes({1, 1});
  }
  // Don't overwrite already loaded DBC
  if (!GetDBC()->nonEmptyFileCount()) {
    dbc_controller_->newFile();
  }

  if (has_stream) {
    if (!stream_layouts_.empty()) restore_session_timer_->stop();
    if (const auto it = stream_layouts_.find(streamLayoutKey(*stream)); it != stream_layouts_.end()) {
      const auto& layout = it->second;
      if (!layout.message_tabs.isEmpty()) {
        inspector_widget_->getMessageView()->restoreTabs(layout.active_message, layout.message_tabs, true);
        inspector_widget_->setMessage(MessageId::fromString(layout.active_message));
      }
      charts_panel->restoreLayout(layout.charts);
    }
    createLoadingDialog(is_live_stream);
  }
}

void MainWindow::createLoadingDialog(bool is_live) {
  auto* stream = StreamManager::stream();
  if (dynamic_cast<CustomUsbSerialStream*>(stream)) return;  // USB transmission can start before the first captured frame.
  auto* device_stream = dynamic_cast<DeviceStream*>(stream);

  if (is_live && device_stream) {
    const QString ip = device_stream->zmqAddress();
    auto* wait_dlg = new ZmqLoadDialog(ip, this);
    wait_dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(wait_dlg, &QDialog::rejected, this, &MainWindow::closeStream);
    connect(stream, &AbstractStream::eventsMerged, wait_dlg, &QDialog::accept);
    connect(&StreamManager::instance(), &StreamManager::streamChanged, wait_dlg, &QDialog::accept);
    wait_dlg->show();
    return;
  }

  auto* wait_dlg = new QProgressDialog(is_live ? tr("Waiting for live stream...") : tr("Loading segments..."),
                                       tr("&Abort"), 0, 100, this);

  wait_dlg->setWindowModality(Qt::WindowModal);
  wait_dlg->setAttribute(Qt::WA_DeleteOnClose);
  wait_dlg->setFixedSize(400, wait_dlg->sizeHint().height());

  connect(wait_dlg, &QProgressDialog::canceled, this, [this, wait_dlg]() {
    wait_dlg->deleteLater();
    closeStream();
  });
  connect(stream, &AbstractStream::eventsMerged, wait_dlg, &QProgressDialog::accept);
  connect(&StreamManager::instance(), &StreamManager::streamChanged, wait_dlg, &QProgressDialog::accept);
  connect(&SystemRelay::instance(), &SystemRelay::downloadProgress, wait_dlg,
          [wait_dlg](uint64_t cur, uint64_t total, bool success) {
            Q_UNUSED(success);
            if (total == 0) return;
            wait_dlg->setValue((int)((cur / (double)total) * 100));
          });
  wait_dlg->show();
}

void MainWindow::eventsMerged() {
  auto* stream = StreamManager::stream();
  if (!stream) return;

  if (!stream->liveStreaming()) {
    const QString prev_fingerprint = car_fingerprint_;
    car_fingerprint_ = stream->carFingerprint();
    if (prev_fingerprint != car_fingerprint_) {
      video_dock_->setWindowTitle(tr("ROUTE: %1  FINGERPRINT: %2")
                                      .arg(stream->routeName())
                                      .arg(car_fingerprint_.isEmpty() ? tr("Unknown Car") : car_fingerprint_));
      // Don't overwrite already loaded DBC
      if (!GetDBC()->nonEmptyFileCount()) {
        QTimer::singleShot(0, this, [this]() { dbc_controller_->loadFromFingerprint(car_fingerprint_); });
      }
    }
  }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
  if (obj == floating_window_ && event->type() == QEvent::Close) {
    toggleChartsDocking();
    return true;
  }
  return QMainWindow::eventFilter(obj, event);
}

void MainWindow::toggleChartsDocking() {
  if (floating_window_) {
    // Dock the charts widget back to the main window
    floating_window_->removeEventFilter(this);
    charts_layout_->insertWidget(0, charts_panel, 1);
    floating_window_->deleteLater();
    floating_window_ = nullptr;
    charts_panel->getToolBar()->setIsDocked(true);
  } else {
    // Float the charts widget in a separate window
    floating_window_ = new QWidget(this, Qt::Window);
    floating_window_->setWindowTitle("Charts");
    floating_window_->setLayout(new QVBoxLayout());
    floating_window_->layout()->addWidget(charts_panel);
    floating_window_->installEventFilter(this);
    floating_window_->showMaximized();
    charts_panel->getToolBar()->setIsDocked(false);
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!dbc_controller_->remindSaveChanges()) { event->ignore(); return; }
  StreamManager::instance().shutdown();

  if (floating_window_) floating_window_->deleteLater();

  // save states
  settings.geometry = saveGeometry();
  settings.window_state = saveState();
  if (!StreamManager::instance().isLiveStream()) {
    settings.video_splitter_state = video_splitter_->saveState();
  }
  settings.message_header_state = message_list_->saveHeaderState();

  saveSessionState();
  SystemRelay::instance().uninstallHandlers();
  QMainWindow::closeEvent(event);
}

void MainWindow::setOption() {
  SettingsDialog dlg(this);
  dlg.exec();
}

void MainWindow::findSimilarBits() {
  FindSimilarBitsDlg* dlg = new FindSimilarBitsDlg(this);
  connect(dlg, &FindSimilarBitsDlg::openMessage, message_list_, &MessageList::selectMessage);
  dlg->show();
}

void MainWindow::findSignal() {
  FindSignalDlg* dlg = new FindSignalDlg(this);
  connect(dlg, &FindSignalDlg::openMessage, message_list_, &MessageList::selectMessage);
  dlg->show();
}

void MainWindow::onlineHelp() {
  if (auto guide = findChild<GuideOverlay*>()) {
    guide->close();
  } else {
    guide = new GuideOverlay(this);
    guide->setGeometry(rect());
    guide->show();
    guide->raise();
  }
}

void MainWindow::toggleFullScreen() {
  if (isFullScreen()) {
    menuBar()->show();
    statusBar()->show();
    showNormal();
    showMaximized();
  } else {
    menuBar()->hide();
    statusBar()->hide();
    showFullScreen();
  }
}

void MainWindow::saveSessionState() {
  settings.recent_dbc_file = "";
  settings.active_msg_id = "";
  settings.selected_msg_ids.clear();
  settings.active_charts.clear();

  for (const auto& f : GetDBC()->allFiles())
    if (!f->isEmpty()) {
      settings.recent_dbc_file = f->filename;
      break;
    }

  if (auto* detail = inspector_widget_->getMessageView()) {
    auto [active_id, ids] = detail->serializeMessageIds();
    settings.active_msg_id = active_id;
    settings.selected_msg_ids = ids;
  }
  if (charts_panel) settings.active_charts = charts_panel->serializeChartIds();
}

void MainWindow::restoreSessionState() {
  if (settings.recent_dbc_file.isEmpty() || GetDBC()->nonEmptyFileCount() == 0) return;

  QString dbc_file;
  for (const auto& f : GetDBC()->allFiles())
    if (!f->isEmpty()) {
      dbc_file = f->filename;
      break;
    }
  if (dbc_file != settings.recent_dbc_file) return;

  if (!settings.selected_msg_ids.isEmpty()) {
    inspector_widget_->getMessageView()->restoreTabs(settings.active_msg_id, settings.selected_msg_ids);
    inspector_widget_->setMessage(MessageId::fromString(settings.active_msg_id));
  }

  if (charts_panel != nullptr && !settings.active_charts.empty())
    charts_panel->restoreChartsFromIds(settings.active_charts);
}

void MainWindow::changeEvent(QEvent* ev) {
  if (ev->type() == QEvent::ApplicationPaletteChange) {
    utils::setTheme(0);
  }
  QMainWindow::changeEvent(ev);
}
