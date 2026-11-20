#include "signal_tree_delegate.h"

#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>
#include <QToolTip>

#include "core/commands/commands.h"
#include "value_table_editor.h"
#include "widgets/validators.h"

SignalTreeDelegate::SignalTreeDelegate(QObject* parent) : QStyledItemDelegate(parent) {
  name_validator = new NameValidator(this);
  node_validator = new QRegularExpressionValidator(QRegularExpression("^\\w+(,\\w+)*$"), this);
  double_validator = new DoubleValidator(this);
  signal_text_color = utils::isDarkTheme() ? QColor(20, 20, 20) : QColor(255, 255, 255);

  label_font.setPointSize(8);
  value_font = qApp->font();
}

QSize SignalTreeDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
  // Use toolbar icon size + padding for row height; width determined by header
  const int icon_size = option.widget ? option.widget->style()->pixelMetric(QStyle::PM_ToolBarIconSize) : kBtnSize;
  int height = icon_size + kPadding;
  return QSize(-1, height);
}

void SignalTreeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
  auto item = static_cast<SignalTreeModel::Item*>(index.internalPointer());

  // Selection background
  if (option.state & QStyle::State_Selected) {
    painter->fillRect(option.rect, option.palette.highlight());
  } else if (option.state & QStyle::State_MouseOver || item->highlight) {
    painter->fillRect(option.rect, option.palette.color(QPalette::AlternateBase));
  }

  QRect rect = option.rect.adjusted(kPadding, 0, -kPadding, 0);

  if (index.column() == SignalTreeModel::NameColumn) {
    drawNameColumn(painter, rect, option, item, index);
  } else if (index.column() == SignalTreeModel::HistoryColumn) {
    if (item->type == SignalTreeModel::Item::Sig) {
      drawHistoryColumn(painter, rect, option.state & QStyle::State_Selected, item);
    }
  } else if (item->type == SignalTreeModel::Item::Sig) {
    drawValueColumn(painter, rect, option, item, index);
  } else {
    QStyledItemDelegate::paint(painter, option, index);
  }
}

void SignalTreeDelegate::drawNameColumn(QPainter* p, QRect r, const QStyleOptionViewItem& opt,
                                        SignalTreeModel::Item* item, const QModelIndex& idx) const {
  if (item->type == SignalTreeModel::Item::Sig) {
    // 1. Color Label / Row Number
    QRect iconRect(r.left(), r.top() + (r.height() - kBtnSize) / 2, kColorLabelW, kBtnSize);
    p->setPen(Qt::NoPen);
    p->setBrush(item->sig->color);
    p->drawRoundedRect(iconRect, 3, 3);  // Fixed: Direct drawing instead of roundedPath

    p->setFont(label_font);
    p->setPen(signal_text_color);
    p->drawText(iconRect, Qt::AlignCenter, QString::number(item->row() + 1));

    r.setLeft(iconRect.right() + kPadding);

    // 2. Multiplexer Indicator (Badge)
    if (item->sig->type != dbc::Signal::Type::Normal) {
      QString m_text =
          (item->sig->type == dbc::Signal::Type::Multiplexor) ? "M" : QString("m%1").arg(item->sig->multiplex_value);

      int m_width = opt.fontMetrics.horizontalAdvance(m_text) + 8;
      QRect m_rect(r.left(), iconRect.top(), m_width, kBtnSize);

      p->setPen(Qt::NoPen);
      p->setBrush(opt.palette.dark());
      p->drawRoundedRect(m_rect, 2, 2);

      p->setPen(Qt::white);
      p->setFont(label_font);
      p->drawText(m_rect, Qt::AlignCenter, m_text);

      r.setLeft(m_rect.right() + kPadding);
    }
  }

  // 3. Signal Name / Property Label
  p->setFont(opt.font);
  p->setPen(opt.palette.color((opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));

  QString text = idx.data(Qt::DisplayRole).toString();
  QString elidedText = opt.fontMetrics.elidedText(text, Qt::ElideRight, r.width());

  p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, elidedText);
}

void SignalTreeDelegate::drawHistoryColumn(QPainter* p, const QRect& rect, bool selected,
                                           SignalTreeModel::Item* item) const {
  item->sparkline->setHighlight(selected);
  const QImage& image = item->sparkline->image();
  if (!image.isNull()) {
    p->save();
    p->setClipRect(rect, Qt::IntersectClip);
    p->drawImage(rect.left(), rect.top() + (rect.height() - image.deviceIndependentSize().height()) / 2, image);
    p->restore();
  }
}

void SignalTreeDelegate::drawValueColumn(QPainter* p, QRect rect, const QStyleOptionViewItem& opt,
                                         SignalTreeModel::Item* item, const QModelIndex& idx) const {
  rect.setRight(getButtonRect(opt.rect, PlotButton).left() - kPadding);
  p->setFont(value_font);
  p->setPen(opt.palette.color((opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
  if (rect.width() > 0) {
    const QString text = p->fontMetrics().elidedText(item->sig_val, Qt::ElideRight, rect.width());
    p->drawText(rect, Qt::AlignRight | Qt::AlignVCenter, text);
  }
  drawButtons(p, opt, item, idx);
}

void SignalTreeDelegate::openValueTable(QWidget* parent, const QModelIndex& index, bool show_extra_info) const {
  if (!index.isValid()) return;
  QPersistentModelIndex target(index);
  auto* item = static_cast<SignalTreeModel::Item*>(index.internalPointer());
  ValueTableEditor dialog(item->sig->value_table, parent, show_extra_info ? item->sig : nullptr);
  dialog.setWindowTitle(show_extra_info ? tr("Extra Info — %1").arg(item->sig->name) : item->sig->name);
  if (dialog.exec() == QDialog::Accepted && target.isValid()) {
    auto* model = const_cast<QAbstractItemModel*>(target.model());
    model->setData(target, QVariant::fromValue(dialog.value_table));
  }
}

QWidget* SignalTreeDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                          const QModelIndex& idx) const {
  auto item = static_cast<SignalTreeModel::Item*>(idx.internalPointer());
  using T = SignalTreeModel::Item::Type;

  if (item->type == T::ValueTable) {
    openValueTable(parent, idx);
    return nullptr;
  }

  if (item->type == T::SignalType) {
    auto* c = new QComboBox(parent);
    c->addItem(signalTypeToString(dbc::Signal::Type::Normal), static_cast<int>(dbc::Signal::Type::Normal));
    auto* msg = GetDBC()->msg(static_cast<const SignalTreeModel*>(idx.model())->messageId());
    if (!msg || !msg->multiplexor)
      c->addItem(signalTypeToString(dbc::Signal::Type::Multiplexor), static_cast<int>(dbc::Signal::Type::Multiplexor));
    else if (item->sig->type != dbc::Signal::Type::Multiplexor)
      c->addItem(signalTypeToString(dbc::Signal::Type::Multiplexed), static_cast<int>(dbc::Signal::Type::Multiplexed));
    return c;
  }

  if (item->type == T::Size) {
    auto* s = new QSpinBox(parent);
    s->setFrame(false);
    s->setRange(1, MAX_CAN_LEN);
    return s;
  }

  auto* e = new QLineEdit(parent);
  e->setFrame(false);
  if (item->type == T::Name) {
    e->setValidator(name_validator);
    auto* comp = new QCompleter(GetDBC()->signalNames(), e);
    comp->setCaseSensitivity(Qt::CaseInsensitive);
    e->setCompleter(comp);
  } else {
    e->setValidator(item->type == T::Node ? node_validator : double_validator);
  }
  return e;
}

void SignalTreeDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const {
  auto item = static_cast<SignalTreeModel::Item*>(index.internalPointer());
  if (item->type == SignalTreeModel::Item::SignalType) {
    model->setData(index, static_cast<QComboBox*>(editor)->currentData().toInt());
    return;
  }
  QStyledItemDelegate::setModelData(editor, model, index);
}

int SignalTreeDelegate::buttonAt(const QPoint& pos, const QRect& rect) const {
  for (int i = 0; i < 2; ++i) {
    if (getButtonRect(rect, i).contains(pos)) return i;
  }
  return -1;
}

QRect SignalTreeDelegate::getButtonRect(const QRect& colRect, int btnIdx) const {
  // btnIdx 0: Remove (far right), btnIdx 1: Plot (left of remove)
  int x = colRect.right() - kPadding - kBtnSize - (btnIdx * (kBtnSize + kBtnSpacing));
  int y = colRect.top() + (colRect.height() - kBtnSize) / 2;
  return QRect(x, y, kBtnSize, kBtnSize);
}

void SignalTreeDelegate::drawButtons(QPainter* p, const QStyleOptionViewItem& opt, SignalTreeModel::Item* item,
                                     const QModelIndex& idx) const {
  bool chart_opened = idx.data(IsChartedRole).toBool();

  auto drawBtn = [&](int btnIdx, const QString& iconName, bool active) {
    QRect rect = getButtonRect(opt.rect, btnIdx);
    bool hovered = (hoverIndex == idx && hoverButton == btnIdx);
    bool selected = opt.state & QStyle::State_Selected;

    if (hovered || active) {
      p->setRenderHint(QPainter::Antialiasing, true);
      // Background: Highlight if active, light overlay if hovered
      QColor bg = active ? opt.palette.color(QPalette::Highlight) : opt.palette.color(QPalette::Button);
      bg.setAlpha(active ? 255 : 100);
      p->setBrush(bg);
      QPen borderPen;
      if (active && selected) {
        // Bright white/light border for dark themes, or a distinct edge
        borderPen = QPen(opt.palette.color(QPalette::Base), 1.5);
      } else if (active) {
        borderPen = QPen(opt.palette.color(QPalette::Highlight).darker(150), 1);
      } else {
        borderPen = QPen(opt.palette.color(QPalette::Mid), 1);
      }
      p->setPen(borderPen);
      p->drawRoundedRect(rect.adjusted(1, 1, -1, -1), 4, 4);
      p->setRenderHint(QPainter::Antialiasing, false);
    }

    // Icon rendering logic
    int iconPadding = 4;
    QSize iconSize = QSize(kBtnSize - (iconPadding * 2), kBtnSize - (iconPadding * 2));

    QColor icon_color;
    if (btnIdx == RemoveButton && hovered) {
      icon_color = QColor(220, 53, 69);  // Soft Red
    } else {
      icon_color =
          (active || selected) ? opt.palette.color(QPalette::HighlightedText) : opt.palette.color(QPalette::Text);
    }
    QPixmap pix = utils::icon(iconName, iconSize, icon_color);
    p->drawPixmap(rect.left() + iconPadding, rect.top() + iconPadding, pix);
  };

  drawBtn(RemoveButton, "circle-minus", false);
  drawBtn(PlotButton, chart_opened ? "chart-area" : "chart-line", chart_opened);
}

bool SignalTreeDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                                   const QModelIndex& index) {
  auto item = static_cast<SignalTreeModel::Item*>(index.internalPointer());
  if (!item || item->type != SignalTreeModel::Item::Sig) {
    return QStyledItemDelegate::helpEvent(event, view, option, index);
  }
  if (index.column() == SignalTreeModel::HistoryColumn && !item->sparkline->isEmpty()) {
    const QString tooltip = tr("Session Min: %1\nSession Max: %2")
                                .arg(QString::number(item->sparkline->minVal(), 'f', 3),
                                     QString::number(item->sparkline->maxVal(), 'f', 3));
    QToolTip::showText(event->globalPos(), tooltip, view);
    return true;
  }
  if (index.column() != SignalTreeModel::ValueColumn) {
    return QStyledItemDelegate::helpEvent(event, view, option, index);
  }

  // Button Hit-Testing
  int btnIdx = buttonAt(event->pos(), option.rect);
  if (btnIdx != -1) {
    if (btnIdx == PlotButton) {
      bool opened = index.data(IsChartedRole).toBool();
      QToolTip::showText(event->globalPos(),
                         opened ? tr("Close Plot") : tr("Show Plot\nSHIFT click to add to previous opened plot"), view);
    } else {
      QToolTip::showText(event->globalPos(), tr("Remove Signal"), view);
    }
    return true;
  }

  return QStyledItemDelegate::helpEvent(event, view, option, index);
}

bool SignalTreeDelegate::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& opt,
                                     const QModelIndex& idx) {
  auto item = static_cast<SignalTreeModel::Item*>(idx.internalPointer());

  if (!item || idx.column() != SignalTreeModel::ValueColumn || item->type != SignalTreeModel::Item::Sig) {
    return QStyledItemDelegate::editorEvent(event, model, opt, idx);
  }

  const auto type = event->type();
  if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease) {
    return QStyledItemDelegate::editorEvent(event, model, opt, idx);
  }

  const auto* mouseEvent = static_cast<QMouseEvent*>(event);
  const int btn = buttonAt(mouseEvent->pos(), opt.rect);

  if (type == QEvent::MouseMove) {
    if (hoverIndex != idx || hoverButton != btn) {
      QPersistentModelIndex oldIdx = hoverIndex;
      hoverIndex = idx;
      hoverButton = btn;

      if (auto* view = qobject_cast<QAbstractItemView*>(const_cast<QWidget*>(opt.widget))) {
        if (oldIdx.isValid()) view->update(oldIdx);
        if (hoverIndex.isValid()) view->update(hoverIndex);
      }
    }
    return false;
  }

  if (btn != -1) {
    if (type == QEvent::MouseButtonRelease) {
      if (btn == PlotButton) {
        bool isCharted = idx.data(IsChartedRole).toBool();
        emit plotRequested(item->sig, !isCharted, mouseEvent->modifiers() & Qt::ShiftModifier);
      } else if (btn == RemoveButton) {
        clearHoverState();    // Reset before model potentially deletes 'item'
        emit removeRequested(item->sig);
      }
    }
    // Accept all mouse events on buttons to prevent row selection/expansion
    return true;
  }

  return QStyledItemDelegate::editorEvent(event, model, opt, idx);
}

int SignalTreeDelegate::nameColumnWidth(const dbc::Signal* sig) const {
  // Start with fixed decorative widths: Padding + Icon + Name Padding
  int width = kPadding + kColorLabelW + kPadding;

  if (sig->type != dbc::Signal::Type::Normal) {
    QString m_text = (sig->type == dbc::Signal::Type::Multiplexor) ? "M" : "m00";
    QFontMetrics badgeFm(label_font);
    width += badgeFm.horizontalAdvance(m_text) + 8 + kPadding;
  }

  QFontMetrics nameFm(QApplication::font());
  return width + nameFm.horizontalAdvance(sig->name) + (kPadding * 2);
}

void SignalTreeDelegate::clearHoverState() {
  QPersistentModelIndex old = hoverIndex;
  hoverIndex = QPersistentModelIndex();
  hoverButton = -1;
  if (old.isValid()) {
    if (auto* view = qobject_cast<QAbstractItemView*>(parent())) {
      view->update(old);
    }
  }
}
