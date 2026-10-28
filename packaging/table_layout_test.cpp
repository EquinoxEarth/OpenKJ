#include "tablelayout.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QPointer>
#include <QTabWidget>
#include <QTest>
#include <memory>

class LargeLibraryModel : public QAbstractTableModel {
public:
    mutable int reads = 0;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 600000; }
    int columnCount(const QModelIndex & = {}) const override { return 8; }
    QVariant data(const QModelIndex &, int role) const override
    {
        ++reads;
        return role == Qt::DisplayRole ? QVariant("A song with a long artist and title") : QVariant{};
    }
};

class TableLayoutTest : public QObject {
    Q_OBJECT
private slots:
    void sizingDoesNotReadRowsOrProcessEvents()
    {
        LargeLibraryModel model;
        QTableView table;
        table.setModel(&model);
        table.hideColumn(0);
        table.hideColumn(4);
        table.resize(1800, 400);
        table.show();
        QTest::qWait(20);
        for (int fontSize : {13, 24, 36}) {
            auto font = table.font();
            font.setPointSize(fontSize);
            table.setFont(font);
            QTest::qWait(10);
            bool queuedCallbackRan = false;
            QTimer::singleShot(0, &table, [&] { queuedCallbackRan = true; });
            model.reads = 0;
            okj::sizeTableColumns(&table, {{3, 145}, {5, 75}, {6, 75}, {7, 170}}, 1, 2);
            QCOMPARE(model.reads, 0);
            QVERIFY(!queuedCallbackRan);
            auto *header = table.horizontalHeader();
            const int available = table.viewport()->width() - header->sectionSize(3) -
                header->sectionSize(5) - header->sectionSize(6) - header->sectionSize(7);
            QCOMPARE(header->sectionSize(1), std::max(150, static_cast<int>(available * 0.30)));
            QCOMPARE(header->sectionSize(2), std::max(150, available - header->sectionSize(1)));
            for (int column : {1,2,3,5,6,7})
                QCOMPARE(header->sectionResizeMode(column), QHeaderView::Interactive);
            const int resized = header->sectionSize(1) + 37;
            header->resizeSection(1, resized);
            QTest::qWait(10);
            QCOMPARE(header->sectionSize(1), resized);
            QVERIFY(queuedCallbackRan);
        }
    }

    void layoutRequestsCoalesceAndCancelWithOwner()
    {
        auto owner = std::make_unique<QObject>();
        int layouts = 0;
        QPointer<okj::DeferredLayout> layout = new okj::DeferredLayout(owner.get(), [&] { ++layouts; });
        for (int request = 0; request < 100; ++request)
            layout->schedule();
        QCOMPARE(layouts, 0);
        QVERIFY(layout->isPending());
        QTRY_COMPARE(layouts, 1);
        QVERIFY(!layout->isPending());
        layout->schedule();
        owner.reset();
        QVERIFY(layout.isNull());
        QTest::qWait(10);
        QCOMPARE(layouts, 1);
    }

    void tabLayoutUsesLatestVisibleGeometry()
    {
        LargeLibraryModel model;
        QTabWidget tabs;
        auto *queue = new QTableView;
        auto *history = new QTableView;
        for (auto *table : {queue, history}) {
            table->setModel(&model);
            table->hideColumn(0);
            table->hideColumn(4);
        }
        tabs.addTab(queue, "Queue");
        tabs.addTab(history, "History");
        tabs.resize(1500, 450);
        int layouts = 0, applied = -1, measuredWidth = 0;
        okj::DeferredLayout deferred(&tabs, [&] {
            if (!tabs.isVisible())
                return;
            auto *table = qobject_cast<QTableView *>(tabs.currentWidget());
            applied = tabs.currentIndex();
            measuredWidth = table->viewport()->width();
            okj::sizeTableColumns(table, {{3, 145}, {5, 75}, {6, 75}, {7, 170}}, 1, 2);
            ++layouts;
        });
        connect(&tabs, &QTabWidget::currentChanged, &tabs, [&] { deferred.schedule(); });
        deferred.schedule();
        QTest::qWait(10);
        QCOMPARE(layouts, 0);
        tabs.show();
        deferred.schedule();
        QTRY_COMPARE(layouts, 1);
        QCOMPARE(applied, 0);
        QCOMPARE(measuredWidth, queue->viewport()->width());
        tabs.setCurrentIndex(1);
        tabs.setCurrentIndex(0);
        tabs.setCurrentIndex(1);
        QCOMPARE(layouts, 1);
        QTRY_COMPARE(layouts, 2);
        QCOMPARE(applied, 1);
        QCOMPARE(measuredWidth, history->viewport()->width());
        QVERIFY(measuredWidth > 1000);
        tabs.hide();
        deferred.schedule();
        QTest::qWait(10);
        QCOMPARE(layouts, 2);
    }
};

QTEST_MAIN(TableLayoutTest)
#include "table_layout_test.moc"
