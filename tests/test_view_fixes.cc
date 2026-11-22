#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <atomic>
#include <iostream>

#include "core/commands/commands.h"
#include "core/streams/live_stream.h"
#include "modules/dbc/dbc_controller.h"
#include "modules/inspector/signal_editor/signal_editor.h"
#include "modules/message_list/message_list.h"
#include "modules/settings/settings.h"
#include "modules/system/stream_manager.h"

class TestStream : public LiveStream {
 public:
  TestStream(QObject* parent, bool& stopped) : LiveStream(parent, false), stopped_(stopped) {}
  bool checkEventGrowth() {
    size_t reallocations = 0, capacity = 0;
    const uint8_t data[] = {1, 120};
    for (uint64_t i = 0; i < 4096; ++i) {
      mergeEvents({newEvent(i + 1, 0, 42, data, 2)});
      const auto next = events({0, 42}).capacity();
      if (next != capacity) ++reallocations;
      capacity = next;
    }
    return events({0, 42}).size() == 4096 && reallocations < 32;
  }
  QString routeName() const override { return "test"; }
  void push(uint64_t ns, uint8_t mux, uint8_t value, uint8_t source = 0) {
    MessageBuilder message;
    auto event = message.initEvent();
    event.setLogMonoTime(ns);
    auto can = event.initCan(1);
    can[0].setSrc(source);
    can[0].setAddress(42);
    uint8_t data[2] = {mux, value};
    can[0].setDat(kj::arrayPtr(data, 2));
    handleEvent(capnp::messageToFlatArray(message));
  }

 protected:
  void streamThread() override {
    while (!QThread::currentThread()->isInterruptionRequested()) QThread::msleep(1);
    stopped_ = true;
  }

 private:
  bool& stopped_;
};

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir temp(QDir::currentPath() + "/build/view-fixes-XXXXXX");
  const QString file = temp.filePath("mux.dbc");
  const QByteArray dbc =
      "VERSION \"\"\nNS_ :\nBS_:\nBU_: NODE\nBO_ 42 Test: 2 NODE\n SG_ Page M : 0|8@1+ (1,0) [0|255] \"\" NODE\n SG_ "
      "BrickVoltage m1 : 8|8@1+ (1,0) [0|255] \"V\" NODE\n";
  QFile f(file);
  if (!f.open(QIODevice::WriteOnly)) return 7;
  f.write(dbc);
  f.close();
  GetDBC()->open({0, 1}, file);
  bool growth_stopped = false;
  auto* growth_stream = new TestStream(&app, growth_stopped);
  if (!growth_stream->checkEventGrowth()) return 12;
  delete growth_stream;
  const auto saved = settings.pinned_signals;
  const auto saved_messages = settings.pinned_messages;
  settings.pinned_messages.clear();
  settings.pinned_signals = {MessageId(0, 42).toString() + "|BrickVoltage"};
  bool stopped = false;
  auto* stream = new TestStream(&app, stopped);
  StreamManager::instance().setStream(stream);
  stream->push(1000000000, 1, 120);
  stream->push(1001000000, 2, 200);
  stream->push(1002000000, 1, 80, 1);
  QTimer::singleShot(250, &app, [&] {
    const auto* signal = GetDBC()->msg({0, 42})->sig("BrickVoltage");
    if (latestSignalValue({0, 42}, signal) != 120.0) {
      app.exit(1);
      return;
    }
    QLineSeries series;
    ChartSignal chart_signal({0, 42}, signal, &series);
    int replacements = 0;
    QObject::connect(&series, &QXYSeries::pointsReplaced, &app, [&] { ++replacements; });
    chart_signal.prepareData(nullptr, 0, 10);
    chart_signal.updateSeries(SeriesType::Line);
    chart_signal.updateSeries(SeriesType::Line);
    if (replacements != 1 || series.count() != 1 || series.at(0).y() != 120) {
      app.exit(13);
      return;
    }
    chart_signal.updateSeries(SeriesType::StepLine);
    if (replacements != 2) { app.exit(14); return; }
    QLineSeries window_series;
    ChartSignal window_signal({0, 42}, signal, &window_series);
    for (int i = 0; i < 100000; ++i) window_signal.vals.emplace_back(i, i % 17);
    window_signal.updateSeries(SeriesType::Line, 50000, 50010);
    if (window_series.count() != 13 || window_series.at(0) != QPointF(49999, 49999 % 17) ||
        window_series.at(12) != QPointF(50011, 50011 % 17)) { app.exit(15); return; }
    window_signal.updateSeries(SeriesType::Line, 0, 99999);
    if (window_series.count() != 100000 || window_signal.vals.size() != 100000) { app.exit(16); return; }
    window_signal.updateSeries(SeriesType::Line, 0, 99999, 250);
    bool has_min = false, has_max = false;
    for (const auto& point : window_series.points()) {
      has_min |= point.y() == 0;
      has_max |= point.y() == 16;
    }
    if (window_series.count() > 1008 || !has_min || !has_max || window_signal.vals.size() != 100000) {
      app.exit(18); return;
    }
    window_signal.updateSeries(SeriesType::Scatter, 0, 99999, 250);
    if (window_series.count() != 100000) { app.exit(19); return; }
    window_signal.step_vals = {{0, 1}, {1, 1}, {1, 2}, {2, 2}, {2, 3}, {3, 3}};
    window_signal.updateSeries(SeriesType::StepLine, 1, 2);
    if (window_series.count() != 6) { app.exit(17); return; }
    ChartsPanel charts;
    SignalEditor editor(&charts, nullptr);
    editor.setMessage({0, 42});
    editor.resize(1000, 600);
    editor.show();
    app.processEvents();
    editor.updateState();
    auto* model = editor.model;
    SignalTree* regular_tree = nullptr;
    SignalTree* pinned_tree = nullptr;
    for (auto* tree : editor.findChildren<SignalTree*>()) {
      if (tree->model() == model) regular_tree = tree;
      else pinned_tree = tree;
    }
    if (!regular_tree || !pinned_tree || !pinned_tree->isVisible() || model->rowCount() != 2 ||
        model->index(0, SignalTreeModel::NameColumn).data().toString() != "Page" ||
        model->index(1, SignalTreeModel::NameColumn).data().toString() != "BrickVoltage") {
      app.exit(2);
      return;
    }
    auto* pinned_model = static_cast<SignalTreeModel*>(pinned_tree->model());
    if (pinned_model->rowCount() != 1 || pinned_tree->viewport()->height() < 20) {
      app.exit(25);
      return;
    }
    auto* divider = editor.findChild<QSplitter*>();
    editor.resize(1000, 365);
    app.processEvents();
    int minimum = 0, maximum = 0;
    divider->getRange(1, &minimum, &maximum);
    if (maximum - minimum < 140) {
      app.exit(30);
      return;
    }
    editor.resize(1000, 600);
    app.processEvents();
    const auto pinned_name = pinned_model->index(0, SignalTreeModel::NameColumn);
    const auto original_name = model->index(1, SignalTreeModel::NameColumn);
    const auto pinned_handle = pinned_model->index(0, SignalTreeModel::PinHandleColumn);
    if (!pinned_handle.data(PinDragHandleRole).toBool() || pinned_name.data(PinDragHandleRole).toBool() ||
        model->index(0, SignalTreeModel::PinHandleColumn).isValid() ||
        pinned_tree->treePosition() != SignalTreeModel::NameColumn ||
        pinned_tree->visualRect(pinned_handle).right() >= pinned_tree->visualRect(pinned_name).left()) {
      app.exit(34);
      return;
    }
    if (pinned_name.data(SignalNumberRole).toInt() != 2 ||
        original_name.data(SignalNumberRole) != pinned_name.data(SignalNumberRole) ||
        model->itemFromIndex(original_name)->sig != pinned_model->itemFromIndex(pinned_name)->sig ||
        !original_name.data(PinnedSignalRole).toBool() || pinned_name.data(PinnedSignalRole).toBool()) {
      app.exit(28);
      return;
    }
    regular_tree->setCurrentIndex(original_name);
    pinned_tree->setCurrentIndex(pinned_name);
    if (!pinned_tree->selectionModel()->isSelected(pinned_name) ||
        regular_tree->selectionModel()->hasSelection() || regular_tree->currentIndex().isValid()) {
      app.exit(31);
      return;
    }
    editor.selectSignal(signal);
    if (!regular_tree->selectionModel()->isSelected(original_name) ||
        pinned_tree->selectionModel()->hasSelection() || pinned_tree->currentIndex().isValid()) {
      app.exit(32);
      return;
    }
    model->setFilter("BrickVoltage");
    if (model->index(0, SignalTreeModel::NameColumn).data(SignalNumberRole).toInt() != 2) {
      app.exit(29);
      return;
    }

    // The same DBC definition can be visible on two buses with different live values.
    editor.setMessage({1, 42});
    const std::set<MessageId> updated_bus_zero{{0, 42}};
    editor.updateState(&updated_bus_zero);
    const auto pin = pinned_model->index(0, SignalTreeModel::NameColumn);
    const auto* pinned_item = pinned_model->itemFromIndex(pin);
    const auto property = pinned_model->valueTableIndex(pin);
    if (model->rowCount() != 2 || pinned_item->id != MessageId(0, 42) ||
        !pin.siblingAtColumn(SignalTreeModel::ValueColumn).data().toString().contains("120") ||
        pinned_item->sparkline->isEmpty() || !property.isValid() ||
        pinned_model->itemFromIndex(property)->id != MessageId(0, 42)) {
      app.exit(20);
      return;
    }
    const auto active = model->index(model->signalRow(signal), SignalTreeModel::ValueColumn);
    if (!active.isValid() || !active.data().toString().contains("80")) {
      app.exit(21);
      return;
    }

    const auto original_pins = settings.pinned_signals;
    const QString page_pin = MessageId(1, 42).toString() + "|Page";
    const QString unavailable_pin = MessageId(0, 42).toString() + "|Unavailable";
    const QString brick_pin = original_pins.front();
    settings.pinned_signals = {brick_pin, unavailable_pin, page_pin};
    pinned_model->rebuild();
    const QPersistentModelIndex moved_pin(pinned_model->index(0, SignalTreeModel::NameColumn));
    const bool reordered = pinned_model->movePinnedSignal(0, 1) && moved_pin.row() == 1 &&
                           settings.pinned_signals == QStringList{unavailable_pin, page_pin, brick_pin};
    pinned_model->rebuild();
    const bool restored_order = pinned_model->index(0, SignalTreeModel::NameColumn).data().toString() == "Page";
    settings.pinned_signals = original_pins;
    pinned_model->rebuild();
    if (!reordered || !restored_order) {
      app.exit(33);
      return;
    }

    MessageList list(nullptr);
    list.resize(640, 440);
    list.show();
    MessageModel* messages = nullptr;
    MessageModel* pinned_messages = nullptr;
    MessageTable* pinned_table = nullptr;
    for (auto* view : list.findChildren<QTreeView*>()) {
      auto* table = dynamic_cast<MessageTable*>(view);
      if (!table) continue;
      auto* m = static_cast<MessageModel*>(table->model());
      if (table->isSortingEnabled()) messages = m;
      else {
        pinned_table = table;
        pinned_messages = m;
      }
    }
    if (!messages || !pinned_messages || pinned_table->isVisible()) {
      app.exit(26);
      return;
    }
    messages->rebuild();
    messages->sort(MessageModel::SOURCE, Qt::AscendingOrder);
    pinned_messages->setPinned({1, 42}, true);
    app.processEvents();
    const auto* first = messages->getItem(messages->index(0, 0));
    const auto message_pin = pinned_messages->index(0, MessageModel::NAME);
    const auto message_original = messages->index(messages->getRowForMessageId({1, 42}), MessageModel::NAME);
    const auto* copy = pinned_messages->getItem(message_pin);
    const auto* original = messages->getItem(message_original);
    if (!first || first->id != MessageId(0, 42) || messages->getRowForMessageId({1, 42}) != 1 ||
        !copy || !original || copy->id != original->id || copy->data != original->data || !copy->data ||
        !message_original.data(MessageModel::PinnedRole).toBool() || message_pin.data(MessageModel::PinnedRole).toBool() ||
        !pinned_table->isVisible() ||
        pinned_table->viewport()->height() < 20 || settings.pinned_messages != QStringList{"1:2A"}) {
      app.exit(22);
      return;
    }
    std::optional<MessageId> selected;
    QObject::connect(&list, &MessageList::msgSelectionChanged, [&](const MessageId& id) { selected = id; });
    pinned_table->setCurrentIndex(pinned_messages->index(0, 0));
    if (selected != MessageId(1, 42)) {
      app.exit(27);
      return;
    }
    messages->setInactiveMessagesVisible(false);
    pinned_messages->setPinned({2, 42}, true);
    const auto* missing = pinned_messages->getItem(pinned_messages->index(1, 0));
    if (!missing || missing->id != MessageId(2, 42) || missing->data || messages->getRowForMessageId({2, 42}) != -1) {
      app.exit(23);
      return;
    }
    pinned_messages->setPinned({2, 42}, false);
    pinned_messages->setPinned({1, 42}, false);
    if (!settings.pinned_messages.isEmpty() || pinned_table->isVisible() || messages->getRowForMessageId({1, 42}) != 1 ||
        message_original.data(MessageModel::PinnedRole).toBool()) {
      app.exit(24);
      return;
    }
    auto* tabs = new TabBar(nullptr);
    for (int i = 0; i < 20; i++) tabs->addTab(QString("Long signal tab %1").arg(i));
    auto* area = static_cast<QScrollArea*>(createScrollableTabBar(tabs, nullptr));
    area->resize(250, 50);
    area->show();
    app.processEvents();
    QWheelEvent wheel(QPointF(10, 10), QPointF(10, 10), QPoint(-37, 0), QPoint(), Qt::NoButton, Qt::NoModifier,
                      Qt::ScrollUpdate, false);
    QApplication::sendEvent(tabs, &wheel);
    if (area->horizontalScrollBar()->value() != 74 || tabs->currentIndex() != 0) {
      app.exit(3);
      return;
    }
    // Trackpads send zero-delta begin/end events as well as momentum updates.
    for (auto phase : {Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollMomentum, Qt::ScrollEnd}) {
      QWheelEvent empty(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(), Qt::NoButton,
                        Qt::NoModifier, phase, false);
      QApplication::sendEvent(tabs, &empty);
      QApplication::sendEvent(area->viewport(), &empty);
    }
    if (area->horizontalScrollBar()->value() != 74 || tabs->currentIndex() != 0) {
      app.exit(10);
      return;
    }
    QWheelEvent angle(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(-120, 0), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(tabs, &angle);
    if (area->horizontalScrollBar()->value() <= 74 || tabs->currentIndex() != 0) {
      app.exit(11);
      return;
    }
    delete area;
    dbc::Signal modified = *signal;
    modified.factor = 2;
    UndoStack::push(new EditSignalCommand({0, 42}, signal, modified));
    DbcController controller(&editor);
    QTimer::singleShot(0, [] {
      auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
      if (box) box->button(QMessageBox::Cancel)->click();
    });
    if (controller.remindSaveChanges() || GetDBC()->msg({0, 42})->sig("BrickVoltage")->factor != 2) {
      app.exit(9);
      return;
    }
    QTimer::singleShot(0, [] {
      auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
      if (box) box->button(QMessageBox::Discard)->click();
    });
    if (!controller.remindSaveChanges() || GetDBC()->msg({0, 42})->sig("BrickVoltage")->factor != 1) {
      app.exit(4);
      return;
    }
    QFile disk(file);
    if (!disk.open(QIODevice::ReadOnly)) {
      app.exit(8);
      return;
    }
    if (disk.readAll() != dbc) {
      app.exit(5);
      return;
    }
    StreamManager::instance().setStream(new DummyStream(&app));
    if (!stopped) {
      app.exit(6);
      return;
    }
    settings.pinned_signals = saved;
    std::cout << "Pixel tab scrolling, pinned signal and message rows, DBC discard and synchronous stream shutdown passed\n";
    app.quit();
  });
  int result = app.exec();
  settings.pinned_signals = saved;
  settings.pinned_messages = saved_messages;
  return result;
}
