#ifndef OPENKJ_TABLELAYOUT_H
#define OPENKJ_TABLELAYOUT_H

#include <QHeaderView>
#include <QObject>
#include <QTableView>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <utility>

namespace okj {
// Coalesce requests and wait for Qt's normal event loop to settle widget geometry.
// The owned timer also cancels pending work when its context is destroyed.
class DeferredLayout : public QObject {
    QTimer m_timer{this};
public:
    DeferredLayout(QObject *parent, std::function<void()> apply) : QObject(parent)
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, std::move(apply));
    }
    void schedule() { m_timer.start(0); }
    bool isPending() const { return m_timer.isActive(); }
};

// Explicit widths avoid model-row measurements and nested event processing.
inline void sizeFlexibleColumns(QTableView *table, int artistColumn, int titleColumn,
                                double artistFraction = 0.30)
{
    auto *header = table->horizontalHeader();
    header->setSectionResizeMode(artistColumn, QHeaderView::Interactive);
    header->setSectionResizeMode(titleColumn, QHeaderView::Interactive);
    int available = table->viewport()->width();
    for (int column = 0; column < header->count(); ++column) {
        if (column != artistColumn && column != titleColumn && !header->isSectionHidden(column))
            available -= header->sectionSize(column);
    }
    constexpr int minimumWidth = 150;
    const int artistWidth = std::max(minimumWidth, static_cast<int>(available * artistFraction));
    header->resizeSection(artistColumn, artistWidth);
    header->resizeSection(titleColumn, std::max(minimumWidth, available - artistWidth));
}

struct ColumnWidth { int column, width; };
inline void sizeTableColumns(QTableView *table, std::initializer_list<ColumnWidth> columns,
                             int artistColumn, int titleColumn, double artistFraction = 0.30)
{
    auto *header = table->horizontalHeader();
    for (const auto &column : columns) {
        header->setSectionResizeMode(column.column, QHeaderView::Interactive);
        header->resizeSection(column.column, column.width);
    }
    sizeFlexibleColumns(table, artistColumn, titleColumn, artistFraction);
}
}

#endif
