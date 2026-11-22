#pragma once

#include <QAbstractItemModel>
#include <QSet>
#include <memory>
#include <set>
#include <span>

#include "core/dbc/dbc_message.h"
#include "modules/charts/sparkline.h"

enum SignalRole {
  IsChartedRole = Qt::UserRole + 10,
  SignalNumberRole = Qt::UserRole + 12,
  PinnedSignalRole,
  PinDragHandleRole,
};

class SignalTreeModel : public QAbstractItemModel {
  Q_OBJECT
 public:
  enum Column { NameColumn, HistoryColumn, ValueColumn, PinHandleColumn, ColumnCount };

  struct Item {
    enum Type {
      Root,
      Sig,
      Name,
      Size,
      Node,
      Endian,
      Signed,
      Offset,
      Factor,
      SignalType,
      MultiplexValue,
      ExtraInfo,
      Unit,
      Comment,
      Min,
      Max,
      ValueTable
    };
    explicit Item(Type t, const QString& title, const dbc::Signal* sig, Item* p)
        : type(t), parent(p), id(p ? p->id : MessageId{}), sig(sig), title(title) {
      if (t == Type::Sig) sparkline = std::make_unique<Sparkline>();
    }
    ~Item() { qDeleteAll(children); }
    inline int row() {
      if (parent) return parent->children.indexOf(this);
      return 0;
    }

    Type type = Type::Root;
    Item* parent = nullptr;
    QList<Item*> children;

    MessageId id;
    const dbc::Signal* sig = nullptr;
    QString title;
    bool highlight = false;
    QString sig_val = "-";
    std::unique_ptr<Sparkline> sparkline;
  };

  SignalTreeModel(QObject* parent, bool pinned_only = false);
  void resetSparklines();
  inline const MessageId& messageId() const { return msg_id; }
  void highlightSignalRow(const dbc::Signal* sig);
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override { return pinned_only_ ? ColumnCount : PinHandleColumn; }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
  QModelIndex parent(const QModelIndex& index) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

  void setMessage(const MessageId& id, const QString& filter = {});
  void updateValues(const std::set<MessageId>* ids);
  void updateSparklines(int first_row, int last_row, const QSize& size);

  void setFilter(const QString& txt);
  bool saveSignal(const dbc::Signal* origin_s, dbc::Signal& s);
  Item* itemFromIndex(const QModelIndex& index) const;
  int signalRow(const dbc::Signal* sig) const;
  QModelIndex valueTableIndex(QModelIndex signal);
  void rebuild();
  bool movePinnedSignal(int from, int to);
  void updateChartedSignals(const QMap<MessageId, QSet<const dbc::Signal*>>& opened);
  void fetchMore(const QModelIndex& parent) override;
  bool canFetchMore(const QModelIndex& parent) const override;

 private:
  bool hasChildren(const QModelIndex& parent) const override;
  void insertItem(int pos, const MessageId& id, const dbc::Signal* sig);
  bool saveSignalForMessage(const MessageId& id, const dbc::Signal* origin_s, dbc::Signal& s);
  void handleSignalAdded(MessageId id, const dbc::Signal* sig);
  void handleSignalUpdated(const dbc::Signal* sig);
  void handleSignalRemoved(const dbc::Signal* sig);
  void handleMsgChanged(MessageId id);

  MessageId msg_id;
  QString filter_str;

  QMap<MessageId, QSet<const dbc::Signal*>> charted_signals_;
  std::unique_ptr<Item> root;

  const bool pinned_only_;
};

QString signalTypeToString(dbc::Signal::Type type);

std::span<const uint8_t> latestSignalPayload(const MessageId& id, const dbc::Signal& signal);
std::optional<double> latestSignalValue(const MessageId& id, const dbc::Signal* signal);
