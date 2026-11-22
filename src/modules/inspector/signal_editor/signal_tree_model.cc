#include "signal_tree_model.h"

#include <algorithm>
#include <map>

#include <QMessageBox>
#include <QtConcurrent>

#include "core/commands/commands.h"
#include "core/dbc/dbc_manager.h"
#include "modules/inspector/binary/binary_model.h"
#include "modules/settings/settings.h"
#include "modules/system/stream_manager.h"

static const QStringList SIGNAL_PROPERTY_LABELS = {
    "Name",   "Size", "Receiver Nodes",  "Little Endian", "Signed", "Offset",
    "Factor", "Type", "Multiplex Value", "Extra Info",    "Unit",   "Comment",
    "Min",    "Max",  "Value Table"};

QString signalTypeToString(dbc::Signal::Type type) {
  switch (type) {
    case dbc::Signal::Type::Multiplexor: return QStringLiteral("Multiplexor Signal");
    case dbc::Signal::Type::Multiplexed: return QStringLiteral("Multiplexed Signal");
    default: return QStringLiteral("Normal Signal");
  }
}

SignalTreeModel::SignalTreeModel(QObject* parent, bool pinned_only)
    : QAbstractItemModel(parent), pinned_only_(pinned_only) {
  root = std::make_unique<Item>(Item::Root, "", nullptr, nullptr);

  connect(GetDBC(), &dbc::Manager::DBCFileChanged, this, &SignalTreeModel::rebuild);
  connect(GetDBC(), &dbc::Manager::msgUpdated, this, &SignalTreeModel::handleMsgChanged);
  connect(GetDBC(), &dbc::Manager::msgRemoved, this, &SignalTreeModel::handleMsgChanged);
  connect(GetDBC(), &dbc::Manager::signalAdded, this, &SignalTreeModel::handleSignalAdded);
  connect(GetDBC(), &dbc::Manager::signalUpdated, this, &SignalTreeModel::handleSignalUpdated);
  connect(GetDBC(), &dbc::Manager::signalRemoved, this, &SignalTreeModel::handleSignalRemoved);
}

void SignalTreeModel::insertItem(int pos, const MessageId& id, const dbc::Signal* sig) {
  auto* item = new Item(Item::Sig, sig->name, sig, root.get());
  item->id = id;
  root->children.insert(pos, item);
}

void SignalTreeModel::setMessage(const MessageId& id, const QString& filter) {
  msg_id = id;
  if (pinned_only_) return;
  filter_str = filter;
  rebuild();
}

std::span<const uint8_t> latestSignalPayload(const MessageId& id, const dbc::Signal& signal) {
  const auto* stream = StreamManager::stream();
  if (!stream) return {};
  const auto* snapshot = stream->snapshot(id);
  if (!snapshot->size) return {};
  if (signal.isPresent(snapshot->data.data(), snapshot->size)) {
    return {snapshot->data.data(), snapshot->size};
  }
  if (!signal.multiplexor) return {};

  const auto& events = stream->events(id);
  const auto cursor = stream->toMonoNs(stream->currentSec());
  const auto end = std::upper_bound(events.begin(), events.end(), cursor,
      [](uint64_t ns, const CanEvent* event) { return ns < event->mono_ns; });
  int examined = 0;
  for (auto it = std::make_reverse_iterator(end); it != events.rend() && examined < 2000; ++it) {
    const auto* event = *it;
    ++examined;
    if (signal.isPresent(event->dat, event->size)) return {event->dat, event->size};
  }
  return {};
}

std::optional<double> latestSignalValue(const MessageId& id, const dbc::Signal* signal) {
  if (!signal) return std::nullopt;
  auto payload = latestSignalPayload(id, *signal);
  if (payload.empty()) return std::nullopt;
  return signal->toPhysical(payload.data(), payload.size());
}

void SignalTreeModel::updateValues(const std::set<MessageId>* ids) {
  bool changed = false;
  const auto* stream = StreamManager::stream();
  for (auto* item : root->children) {
    if (ids && !ids->contains(item->id)) continue;
    QString value = item->sig_val;
    if (!stream->snapshot(item->id)->size) {
      value = QStringLiteral("-");
    } else if (auto val = latestSignalValue(item->id, item->sig)) {
      value = item->sig->formatValue(*val);
    }
    if (value != item->sig_val) {
      item->sig_val = std::move(value);
      changed = true;
    }
  }
  if (changed) {
    emit dataChanged(index(0, ValueColumn), index(rowCount() - 1, ValueColumn), {Qt::DisplayRole, Qt::ToolTipRole});
  }
}

void SignalTreeModel::updateSparklines(int first_row, int last_row, const QSize& size) {
  struct HistoryWindow {
    uint64_t current_ns;
    std::pair<CanEventIter, CanEventIter> events;
  };
  struct Update {
    Item* item;
    const HistoryWindow* window;
  };
  const auto* stream = StreamManager::stream();
  const int seconds = settings.sparkline_range;
  std::map<MessageId, HistoryWindow> windows;
  QVector<Update> updates;
  bool cleared_history = false;
  for (int i = first_row; i <= last_row; ++i) {
    auto* item = root->children[i];
    const auto* snapshot = stream->snapshot(item->id);
    if (!snapshot->size) {
      cleared_history |= !item->sparkline->isEmpty();
      item->sparkline->clearHistory();
      continue;
    }

    const uint64_t current_ns = stream->toMonoNs(snapshot->ts);
    if (item->sparkline->isUpToDate(current_ns, size)) continue;
    auto [window, inserted] = windows.try_emplace(item->id);
    if (inserted) {
      window->second = {current_ns, stream->eventsInRange(item->id, std::pair{snapshot->ts - seconds, snapshot->ts})};
    }
    updates.append({item, &window->second});
  }

  if (!updates.isEmpty()) {
    QtConcurrent::blockingMap(updates, [&](const Update& update) {
      const auto& [current_ns, events] = *update.window;
      update.item->sparkline->update(update.item->sig, events.first, events.second, current_ns, seconds, size);
    });
  }
  if (cleared_history || !updates.isEmpty()) emit dataChanged(index(first_row, HistoryColumn), index(last_row, HistoryColumn));
}

void SignalTreeModel::updateChartedSignals(const QMap<MessageId, QSet<const dbc::Signal*>>& opened) {
  charted_signals_ = opened;
  if (rowCount() > 0) {
    emit dataChanged(index(0, ValueColumn), index(rowCount() - 1, ValueColumn), {IsChartedRole});
  }
}

void SignalTreeModel::setFilter(const QString& txt) {
  filter_str = txt;
  rebuild();
}

void SignalTreeModel::rebuild() {
  beginResetModel();
  root = std::make_unique<Item>(Item::Root, "", nullptr, nullptr);
  if (pinned_only_) {
    for (const auto& key : settings.pinned_signals) {
      const auto separator = key.indexOf('|');
      if (separator < 0) continue;
      const auto id = MessageId::fromString(key.left(separator));
      if (auto* msg = GetDBC()->msg(id)) {
        if (auto* sig = msg->sig(key.mid(separator + 1))) insertItem(root->children.size(), id, sig);
      }
    }
  } else if (auto* msg = GetDBC()->msg(msg_id)) {
    for (auto* sig : msg->getSignals()) {
      if (filter_str.isEmpty() || sig->name.contains(filter_str, Qt::CaseInsensitive)) {
        insertItem(root->children.size(), msg_id, sig);
      }
    }
  }
  endResetModel();
}

bool SignalTreeModel::movePinnedSignal(int from, int to) {
  if (!pinned_only_ || from == to || from < 0 || from >= rowCount() || to < 0 || to >= rowCount()) return false;
  auto pinIndex = [this](int row) {
    const auto* item = root->children.at(row);
    return settings.pinned_signals.indexOf(item->id.toString() + "|" + item->sig->name);
  };
  const auto source = pinIndex(from);
  const auto destination = pinIndex(to);
  if (source < 0 || destination < 0 || !beginMoveRows({}, from, from, {}, to + (from < to))) return false;
  settings.pinned_signals.move(source, destination);
  root->children.move(from, to);
  endMoveRows();
  return true;
}

SignalTreeModel::Item* SignalTreeModel::itemFromIndex(const QModelIndex& index) const {
  return index.isValid() ? static_cast<Item*>(index.internalPointer()) : root.get();
}

int SignalTreeModel::rowCount(const QModelIndex& parent) const {
  if (parent.column() > 0) return 0;
  return itemFromIndex(parent)->children.size();
}

bool SignalTreeModel::hasChildren(const QModelIndex& parent) const {
  if (!parent.isValid()) return true;
  Item* item = itemFromIndex(parent);
  return item->type == Item::Sig || item->type == Item::ExtraInfo;
}

bool SignalTreeModel::canFetchMore(const QModelIndex& parent) const {
  if (!parent.isValid()) return false;
  Item* item = itemFromIndex(parent);

  return (item->type == Item::Sig || item->type == Item::ExtraInfo) && item->children.isEmpty();
}

void SignalTreeModel::fetchMore(const QModelIndex& parent) {
  if (!parent.isValid()) return;

  Item* item = itemFromIndex(parent);
  static constexpr std::array kSigChildren = {Item::Name,           Item::Size,     Item::Node,   Item::Endian,
                                              Item::Signed,         Item::Offset,   Item::Factor, Item::SignalType,
                                              Item::MultiplexValue, Item::ExtraInfo};
  static constexpr std::array kExtraInfoChildren = {Item::Unit, Item::Comment, Item::Min, Item::Max, Item::ValueTable};

  std::span<const Item::Type> types;
  if (item->type == Item::Sig) {
    types = kSigChildren;
  } else if (item->type == Item::ExtraInfo) {
    types = kExtraInfoChildren;
  } else {
    return;
  }

  beginInsertRows(parent, 0, types.size() - 1);
  item->children.reserve(types.size());
  for (auto t : types) {
    QString label = SIGNAL_PROPERTY_LABELS[t - Item::Name];
    item->children.push_back(new Item(t, label, item->sig, item));
  }
  endInsertRows();
}

Qt::ItemFlags SignalTreeModel::flags(const QModelIndex& index) const {
  if (!index.isValid()) return Qt::NoItemFlags;

  const Item* item = itemFromIndex(index);
  Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;

  if (index.column() == ValueColumn && item->type != Item::Sig && item->type != Item::ExtraInfo) {
    if (item->type == Item::Endian || item->type == Item::Signed) {
      f |= Qt::ItemIsUserCheckable;
    } else {
      f |= Qt::ItemIsEditable;
    }
  }

  // Business logic: disable multiplex settings if the signal isn't multiplexed
  if (item->type == Item::MultiplexValue && item->sig->type != dbc::Signal::Type::Multiplexed) {
    f &= ~Qt::ItemIsEnabled;
  }

  return f;
}

int SignalTreeModel::signalRow(const dbc::Signal* sig) const {
  auto it = std::ranges::find_if(root->children, [&](const Item* item) { return item->id == msg_id && item->sig == sig; });
  return it != root->children.end() ? std::distance(root->children.begin(), it) : -1;
}

QModelIndex SignalTreeModel::valueTableIndex(QModelIndex signal) {
  if (!signal.isValid()) return {};
  while (signal.parent().isValid()) signal = signal.parent();
  signal = signal.siblingAtColumn(NameColumn);

  if (canFetchMore(signal)) fetchMore(signal);
  for (int row = 0; row < rowCount(signal); ++row) {
    const QModelIndex details = index(row, NameColumn, signal);
    if (itemFromIndex(details)->type != Item::ExtraInfo) continue;
    if (canFetchMore(details)) fetchMore(details);
    for (int child = 0; child < rowCount(details); ++child) {
      const QModelIndex property = index(child, ValueColumn, details);
      if (itemFromIndex(property)->type == Item::ValueTable) return property;
    }
    break;
  }
  return {};
}

QModelIndex SignalTreeModel::index(int row, int column, const QModelIndex& parent) const {
  if (parent.isValid() && parent.column() != 0) return {};

  auto* parent_item = itemFromIndex(parent);
  if (row >= 0 && row < parent_item->children.size() && column >= 0 && column < columnCount(parent)) {
    return createIndex(row, column, parent_item->children[row]);
  }
  return {};
}

QModelIndex SignalTreeModel::parent(const QModelIndex& index) const {
  if (!index.isValid()) return {};

  Item* parent_item = itemFromIndex(index)->parent;
  if (!parent_item || parent_item == root.get()) return {};

  return createIndex(parent_item->row(), 0, parent_item);
}

QVariant SignalTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
    if (section == NameColumn) return tr("Signal");
    if (section == HistoryColumn) return tr("History");
    if (section == ValueColumn) return tr("Value");
  }
  return {};
}

QVariant SignalTreeModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid()) return {};

  const Item* item = itemFromIndex(index);
  if (role == Qt::DisplayRole || role == Qt::EditRole) {
    if (index.column() == NameColumn) {
      return item->type == Item::Sig ? item->sig->name : item->title;
    }
    if (index.column() != ValueColumn) return {};

    switch (item->type) {
      case Item::Sig: return item->sig_val;
      case Item::Name: return item->sig->name;
      case Item::Size: return item->sig->size;
      case Item::Node: return item->sig->receiver_name;
      case Item::SignalType: return signalTypeToString(item->sig->type);
      case Item::MultiplexValue: return item->sig->multiplex_value;
      case Item::Offset: return utils::doubleToString(item->sig->offset);
      case Item::Factor: return utils::doubleToString(item->sig->factor);
      case Item::Unit: return item->sig->unit;
      case Item::Comment: return item->sig->comment;
      case Item::Min: return utils::doubleToString(item->sig->min);
      case Item::Max: return utils::doubleToString(item->sig->max);
      case Item::ValueTable: {
        QStringList value_table;
        for (auto& [val, desc] : item->sig->value_table) {
          value_table << QString::number(val) + " \"" + desc + "\"";
        }
        return value_table.join(" ");
      }
      default: break;
    }
    return {};
  }

  if (role == Qt::CheckStateRole && index.column() == ValueColumn) {
    if (item->type == Item::Endian) return item->sig->is_little_endian ? Qt::Checked : Qt::Unchecked;
    if (item->type == Item::Signed) return item->sig->is_signed ? Qt::Checked : Qt::Unchecked;
  }

  if (role == Qt::ToolTipRole && item->type == Item::Sig) {
    if (index.column() == NameColumn) {
      auto tooltip = formatSignalToolTip(item->sig);
      if (pinned_only_) tooltip.prepend(tr("Pinned · %1").arg(item->id.toString()).toHtmlEscaped() + "<br/>");
      return tooltip;
    }
    if (index.column() == ValueColumn) return item->sig_val.toHtmlEscaped().prepend("<qt>").append("</qt>");
  }

  if (role == PinDragHandleRole) {
    return pinned_only_ && item->type == Item::Sig && index.column() == PinHandleColumn;
  }

  if (role == PinnedSignalRole && item->type == Item::Sig && index.column() == NameColumn) {
    return !pinned_only_ && !settings.pinned_signals.isEmpty() &&
           settings.pinned_signals.contains(item->id.toString() + "|" + item->sig->name);
  }

  if (role == SignalNumberRole && item->type == Item::Sig && index.column() == NameColumn) {
    if (!pinned_only_ && filter_str.isEmpty()) return index.row() + 1;
    if (const auto* message = GetDBC()->msg(item->id)) return message->indexOf(item->sig) + 1;
  }

  if (role == IsChartedRole && item->type == Item::Sig) {
    auto it = charted_signals_.find(item->id);
    return (it != charted_signals_.end()) && it.value().contains(item->sig);
  }

  return {};
}

bool SignalTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (role != Qt::EditRole && role != Qt::CheckStateRole) return false;

  Item* item = itemFromIndex(index);
  dbc::Signal s = *item->sig;
  switch (item->type) {
    case Item::Name: s.name = value.toString(); break;
    case Item::Size: s.size = value.toInt(); break;
    case Item::Node: s.receiver_name = value.toString().trimmed(); break;
    case Item::SignalType: s.type = static_cast<dbc::Signal::Type>(value.toInt()); break;
    case Item::MultiplexValue: s.multiplex_value = value.toInt(); break;
    case Item::Endian: s.is_little_endian = value.toBool(); break;
    case Item::Signed: s.is_signed = value.toBool(); break;
    case Item::Offset: s.offset = value.toDouble(); break;
    case Item::Factor: s.factor = value.toDouble(); break;
    case Item::Unit: s.unit = value.toString(); break;
    case Item::Comment: s.comment = value.toString(); break;
    case Item::Min: s.min = value.toDouble(); break;
    case Item::Max: s.max = value.toDouble(); break;
    case Item::ValueTable: s.value_table = value.value<ValueTable>(); break;
    default: return false;
  }
  QPersistentModelIndex edited(index);
  const bool saved = saveSignalForMessage(item->id, item->sig, s);
  if (edited.isValid()) emit dataChanged(edited, edited, {Qt::DisplayRole, Qt::EditRole, Qt::CheckStateRole});
  return saved;
}

void SignalTreeModel::highlightSignalRow(const dbc::Signal* sig) {
  for (int i = 0; i < root->children.size(); ++i) {
    const bool highlight = root->children[i]->id == msg_id && root->children[i]->sig == sig;
    if (root->children[i]->highlight != highlight) {
      root->children[i]->highlight = highlight;
      emit dataChanged(index(i, NameColumn), index(i, ValueColumn), {Qt::DecorationRole, Qt::DisplayRole});
    }
  }
}

bool SignalTreeModel::saveSignal(const dbc::Signal* origin_s, dbc::Signal& s) {
  return saveSignalForMessage(msg_id, origin_s, s);
}

bool SignalTreeModel::saveSignalForMessage(const MessageId& id, const dbc::Signal* origin_s, dbc::Signal& s) {
  auto* msg = GetDBC()->msg(id);
  if (!msg) return false;
  if (s.name != origin_s->name && msg->sig(s.name) != nullptr) {
    QString text = tr("There is already a signal with the same name '%1'").arg(s.name);
    QMessageBox::warning(nullptr, tr("Failed to save signal"), text);
    return false;
  }

  if (s.is_little_endian != origin_s->is_little_endian) {
    s.start_bit = dbc::Signal::flipBitPos(s.start_bit);
  }
  UndoStack::push(new EditSignalCommand(id, origin_s, s));
  return true;
}

void SignalTreeModel::handleMsgChanged(MessageId id) {
  if (pinned_only_ ? !settings.pinned_signals.isEmpty() : id.address == msg_id.address) rebuild();
}

void SignalTreeModel::handleSignalAdded(MessageId id, const dbc::Signal* sig) {
  if (pinned_only_ ? !settings.pinned_signals.isEmpty() : id == msg_id && !filter_str.isEmpty()) {
    rebuild();
  } else if (!pinned_only_ && id == msg_id) {
    int row = GetDBC()->msg(msg_id)->indexOf(sig);
    beginInsertRows({}, row, row);
    insertItem(row, id, sig);
    endInsertRows();
  }
}

void SignalTreeModel::handleSignalUpdated(const dbc::Signal* sig) {
  bool pin_renamed = false;
  for (int row = 0; pinned_only_ && row < rowCount(); ++row) {
    auto* item = root->children[row];
    if (item->sig != sig || item->title == sig->name) continue;
    const auto prefix = item->id.toString() + "|";
    const auto old_key = prefix + item->title;
    const auto new_key = prefix + sig->name;
    if (const auto pos = settings.pinned_signals.indexOf(old_key); pos >= 0) {
      settings.pinned_signals[pos] = new_key;
      pin_renamed = true;
    }
  }
  if (pin_renamed) settings.pinned_signals.removeDuplicates();
  if (pin_renamed || !filter_str.isEmpty()) {
    rebuild();
    return;
  }

  const int row = signalRow(sig);
  if (!pinned_only_ && row >= 0) {
    const auto* msg = GetDBC()->msg(msg_id);
    const int to = msg->indexOf(sig);
    if (to != row) {
      beginMoveRows({}, row, row, {}, to > row ? to + 1 : to);
      root->children.move(row, to);
      endMoveRows();
    }
  }

  if (pinned_only_ && rowCount() > 0) {
    emit dataChanged(index(0, NameColumn), index(rowCount() - 1, NameColumn), {SignalNumberRole});
  }
  for (int i = 0; i < rowCount(); ++i) {
    auto* item = root->children[i];
    if (item->sig != sig) continue;
    item->title = sig->name;
    item->sparkline->clearHistory();
    emit dataChanged(index(i, NameColumn), index(i, ValueColumn), {Qt::DisplayRole, Qt::EditRole, Qt::CheckStateRole});
  }
}

void SignalTreeModel::handleSignalRemoved(const dbc::Signal* sig) {
  for (int row = rowCount() - 1; row >= 0; --row) {
    const auto* item = root->children[row];
    if (item->sig != sig) continue;
    beginRemoveRows({}, row, row);
    delete root->children.takeAt(row);
    endRemoveRows();
  }
  if ((pinned_only_ || !filter_str.isEmpty()) && rowCount() > 0) {
    emit dataChanged(index(0, NameColumn), index(rowCount() - 1, NameColumn), {SignalNumberRole});
  }
}

void SignalTreeModel::resetSparklines() {
  for (auto* item : root->children) {
    item->sparkline->clearHistory();
  }
}
