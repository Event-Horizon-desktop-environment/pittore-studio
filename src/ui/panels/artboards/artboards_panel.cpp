// Artboards panel: one aspect-correct thumbnail plus name per board, in board
// order, matching Affinity's Pages > Artboards list. Clicking a row makes that
// board active; double-click renames it.
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "ui/app_state.h"
#include "ui/panels.h"
#include "ui/theme.h"

namespace pittore::ui {
namespace {

constexpr int kThumbMax = 112;  // longest side of the thumbnail frame
constexpr int kRowHeight = 156; // thumbnail band + name line
constexpr int kNameHeight = 18; // name line beneath the thumbnail

// Paints a board row as a centred, aspect-correct thumbnail over its name.
// The thumbnail image is carried on Qt::UserRole by buildRows().
class ArtboardDelegate final : public QStyledItemDelegate {
public:
    explicit ArtboardDelegate(const AppState* state) : state_(state) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        // Width 0 = "fill the viewport" in ListMode; only the height is fixed.
        return QSize(0, kRowHeight);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override
    {
        const ThemeColors c = colorsFor(state_->theme());
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);

        const QRect r = opt.rect;
        const bool selected = opt.state & QStyle::State_Selected;
        p->fillRect(r, selected ? c.accent : c.chromeAlt);

        const QImage thumb = idx.data(Qt::UserRole).value<QImage>();
        const QSize as = idx.data(Qt::UserRole + 1).toSize();
        if (!thumb.isNull() && as.width() > 0 && as.height() > 0) {
            // Letterbox the board's aspect into a kThumbMax square, centred.
            const int band = r.width() - 12;
            int w = band;
            int h = qRound(w * double(as.height()) / double(as.width()));
            if (h > band) {
                h = band;
                w = qRound(h * double(as.width()) / double(as.height()));
            }
            if (h > kThumbMax - kNameHeight) {
                h = kThumbMax - kNameHeight;
                w = qRound(h * double(as.width()) / double(as.height()));
            }
            const QRect target(r.left() + (r.width() - w) / 2, r.top() + 4, w, h);
            p->fillRect(target.adjusted(-1, -1, 1, 1), c.border);
            p->drawImage(target, thumb);
        }

        const QColor nameCol = selected ? c.accentText : c.text;
        p->setPen(nameCol);
        QFont f = p->font();
        p->setFont(f);
        QRect nameBox(r.left() + 4, r.bottom() - kNameHeight, r.width() - 8,
                      kNameHeight);
        p->drawText(nameBox, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine,
                    idx.data(Qt::DisplayRole).toString());
        p->restore();
    }

private:
    const AppState* state_;
};

} // namespace

QWidget* createArtboardsPanel(AppState* state, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* col = new QVBoxLayout(w);
    col->setContentsMargins(0, 4, 0, 0);

    auto* list = new QListWidget(w);
    list->setItemDelegate(new ArtboardDelegate(state));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setWordWrap(true);

    const auto buildRows = [state, list] {
        list->clear();
        const DocumentItem* d = state->activeDocument();
        if (!d) return;
        // Stack order: Lee's Card first, Bryan's Card second — the same order
        // the Layers panel shows. Walking layers[] forward gives it.
        for (int i = 0; i < d->layers.size(); ++i) {
            const LayerItem& L = d->layers[i];
            if (!L.isBoard()) continue;
            const QSize as(qMax(1, qRound(L.boardRect.width())),
                           qMax(1, qRound(L.boardRect.height())));
            // `i` IS this board's row. Routing through boardRowIndex() was an
            // unnecessary indirection that gave both rows the same thumbnail.
            const QImage img = groupThumbnail(*d, i, kThumbMax);
            auto* item = new QListWidgetItem(list);
            item->setData(Qt::DisplayRole, L.name);
            item->setData(Qt::UserRole, img);
            item->setData(Qt::UserRole + 1, as);
            item->setData(Qt::UserRole + 2, QVariant::fromValue<qulonglong>(L.boardId));
            // No per-item setSizeHint: the viewport has no width yet when this
            // runs (panel constructed before layout), which collapses every row
            // to 0px. The delegate's sizeHint() owns the row height instead.
            item->setToolTip(L.name);
            if (L.boardId == d->activeBoard) list->setCurrentItem(item);
        }
    };

    // Selecting a row makes its board active. Guard on the id so the
    // setCurrentItem() inside buildRows() is a no-op instead of round-tripping
    // through setActiveBoard() -> boardsChanged -> buildRows().
    const auto activate = [state, list] {
        const QListWidgetItem* item = list->currentItem();
        if (!item) return;
        const quint64 bid = item->data(Qt::UserRole + 2).toULongLong();
        const DocumentItem* d = state->activeDocument();
        if (!d || d->activeBoard == bid) return;
        state->setActiveBoard(bid);
    };

    buildRows();

    QObject::connect(list, &QListWidget::currentRowChanged, list,
                     [activate](int) { activate(); });
    QObject::connect(list, &QListWidget::itemDoubleClicked, list,
                     [state, list, buildRows](QListWidgetItem* item) {
                         const quint64 id = item->data(Qt::UserRole + 2).toULongLong();
                         bool ok = false;
                         const QString name = QInputDialog::getText(
                             list, QObject::tr("Rename Artboard"),
                             QObject::tr("Name:"), QLineEdit::Normal,
                             item->text(), &ok);
                         if (!ok || name.trimmed().isEmpty()) return;
                         state->renameBoard(id, name.trimmed());
                         buildRows();
                     });
    QObject::connect(state, &AppState::boardsChanged, list, buildRows);
    // The .af/.psc import path rebuilds the tree and emits layersChanged only
    // (never boardsChanged), so this is the signal that actually populates the
    // panel after opening a document.
    QObject::connect(state, &AppState::layersChanged, list, buildRows);
    QObject::connect(state, &AppState::activeDocumentChanged, list, buildRows);
    QObject::connect(state, &AppState::themeChanged, list,
                     [list] { list->viewport()->update(); });

    col->addWidget(list, 1);
    col->addWidget(makePanelFooter(
        state,
        {{QStringLiteral("artboards-rename"), QObject::tr("Rename")},
         {QStringLiteral("artboards-new"), QObject::tr("New Artboard")}},
        w, [state, list, buildRows](const QString& id) {
            const DocumentItem* d = state->activeDocument();
            if (!d) {
                state->setStatusHint(QObject::tr("Open a document first."));
                return;
            }
            if (id == QLatin1String("artboards-rename")) {
                const QListWidgetItem* item = list->currentItem();
                if (!item) return;
                const quint64 bid = item->data(Qt::UserRole + 2).toULongLong();
                bool ok = false;
                const QString name = QInputDialog::getText(
                    list, QObject::tr("Rename Artboard"), QObject::tr("Name:"),
                    QLineEdit::Normal, item->text(), &ok);
                if (!ok || name.trimmed().isEmpty()) return;
                state->renameBoard(bid, name.trimmed());
            } else {
                // Place it under the active board (same size), or at the
                // origin when the document has no boards yet.
                QRectF rect(0, 0, 800, 600);
                const DocumentItem* cur = state->activeDocument();
                if (cur && cur->activeBoard != 0) {
                    const int row = cur->boardRowIndex(cur->activeBoard);
                    if (row >= 0) {
                        const QRectF b = cur->layers[row].boardRect;
                        rect = b.translated(0, b.height() + 48);
                    }
                }
                state->createBoard(rect, QString());
            }
            buildRows();
        }));
    return w;
}

} // namespace pittore::ui
