// Copyright (C) 2016 basysKom GmbH.
// SPDX-License-Identifier: GPL-3.0-only
// Adapted from Qt's allocationDuringSweepKeepsAllItsSlots regression:
// https://github.com/qt/qtdeclarative/commit/059ee9db91507b8b86cdf4bf033985c9ea41e0d4
#include <QTest>
#include <QScopeGuard>
#include <QJSEngine>
#include <private/qv4engine_p.h>
#include <private/qv4mm_p.h>
#include <private/qv4object_p.h>
#include <private/qv4persistent_p.h>
#include <vector>
class QtRuntimeTests: public QObject {
    Q_OBJECT
private slots:
    void allocationDuringSweepKeepsAllItsSlots();
};
static void gc(QV4::ExecutionEngine &engine) {
    engine.memoryManager->runFullGC();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}
QT_BEGIN_NAMESPACE

namespace QV4 {

static std::vector<PersistentValue> allocatedDuringSweep;
static int allocatedInSweptWord = 0;

namespace Heap {

struct AllocateDuringSweep : Object {
    void init() { Object::init(); }

    void destroy()
    {
        /*
            Allocating from destroy() is allowed - see sweepTriggeringChunkAllocation -
            and stock destroy() implementations do it, for instance by way of the
            Component.onDestruction handlers that ~QQmlContextData emits.

            The free lists we allocate from are still the ones the previous sweep built,
            so some of these objects land in the very bitmap word Chunk::sweep() is
            working on. Remember those; they are the interesting ones.
        */
        auto v4 = internalClass->engine;
        const HeapItem *self = reinterpret_cast<const HeapItem *>(this);
        Chunk *chunk = self->chunk();
        const size_t word = size_t(self - chunk->realBase()) / Chunk::Bits;

        for (int i = 0; i < 256; ++i) {
            Heap::Object *o = v4->newObject();
            const HeapItem *item = reinterpret_cast<const HeapItem *>(o);
            if (item->chunk() != chunk
                    || size_t(item - chunk->realBase()) / Chunk::Bits != word) {
                continue;
            }

            ++allocatedInSweptWord;
            allocatedDuringSweep.emplace_back(v4, Value::fromHeapObject(o));
        }

        Object::destroy();
    }
};

} // namespace Heap

struct AllocateDuringSweep : Object {
    V4_OBJECT2(AllocateDuringSweep, Object)
    V4_NEEDS_DESTROY
};

DEFINE_OBJECT_VTABLE(AllocateDuringSweep);

namespace Heap {

// Small enough to be served from a single orphaned slot.
struct OneSlot : Object {
    void init() { Object::init(); }
};

} // namespace Heap

struct OneSlot : Object {
    V4_OBJECT2(OneSlot, Object)
    enum { NInlineProperties = 0 };
};

DEFINE_OBJECT_VTABLE(OneSlot);

} // namespace QV4

QT_END_NAMESPACE

void QtRuntimeTests::allocationDuringSweepKeepsAllItsSlots()
{
    QJSEngine jsEngine;
    QV4::ExecutionEngine &engine = *jsEngine.handle();
    QV4::MemoryManager *mm = engine.memoryManager;

    // Leave holes all over the heap, so that the allocations the sweep triggers below
    // are served from free list entries rather than from a fresh chunk.
    std::vector<QV4::PersistentValue> keepAlive;
    mm->gcBlocked = QV4::MemoryManager::InCriticalSection;
    for (int i = 0; i < 2048; ++i) {
        QV4::Heap::Object *o = engine.newObject();
        if (i % 2)
            keepAlive.emplace_back(&engine, QV4::Value::fromHeapObject(o));
    }
    mm->gcBlocked = QV4::MemoryManager::Unblocked;
    gc(engine);

    QV4::allocatedDuringSweep.clear();
    QV4::allocatedInSweptWord = 0;
    const auto cleanup = qScopeGuard([] { QV4::allocatedDuringSweep.clear(); });

    mm->allocate<QV4::AllocateDuringSweep>();
    gc(engine);

    // The scenario has to actually occur, otherwise the test proves nothing.
    QVERIFY(QV4::allocatedInSweptWord > 0);

    // Rooting the new objects in a PersistentValue works: PersistentValue::set runs a
    // write barrier, the objects are marked, and Chunk::sweep() keeps the object bits
    // of everything that is black.
    int stillAllocated = 0;
    for (const QV4::PersistentValue &value : std::as_const(QV4::allocatedDuringSweep)) {
        const QV4::HeapItem *item
                = reinterpret_cast<const QV4::HeapItem *>(value.asManaged()->heapObject());
        QV4::Chunk *chunk = item->chunk();
        if (QV4::Chunk::testBit(chunk->objectBitmap, item - chunk->realBase()))
            ++stillAllocated;
    }
    QCOMPARE(stillAllocated, QV4::allocatedInSweptWord);

    /*
        Their extends bits have to survive too. Chunk::sweep() clears the object and
        extends bits of a whole 64 slot word once that word's destructors have run, and
        it must clear exactly the bits it decided to free - not assign the words from
        values it captured beforehand, which would drop whatever setAllocatedSlots() set
        in the meantime. Otherwise sortIntoBins(), which derives the free lists from
        objectBitmap | extendsBitmap, treats the tail slots of these live objects as
        free.
    */
    int freeSlotsInsideLiveObjects = 0;
    for (const QV4::PersistentValue &value : std::as_const(QV4::allocatedDuringSweep)) {
        const QV4::HeapItem *owner
                = reinterpret_cast<const QV4::HeapItem *>(value.asManaged()->heapObject());
        const size_t nValues = value.asManaged()->vtable()->nInlineProperties
                + value.asManaged()->vtable()->inlinePropertyOffset;
        const size_t nSlots = nValues * sizeof(QV4::Value) / QV4::Chunk::SlotSize;

        for (int bin = 0; bin < QV4::BlockAllocator::NumBins; ++bin) {
            for (const QV4::HeapItem *item = mm->blockAllocator.freeBins[bin]; item;
                 item = item->freeData.next) {
                if (item > owner && item < owner + nSlots)
                    ++freeSlotsInsideLiveObjects;
            }
        }
    }

    QCOMPARE(freeSlotsInsideLiveObjects, 0);

    // Which is not merely a bookkeeping problem: a request small enough to be served
    // from one of those slots gets handed a piece of a live object.
    int allocatedInsideLiveObject = 0;
    mm->gcBlocked = QV4::MemoryManager::InCriticalSection;
    for (int i = 0; i < 512; ++i) {
        const QV4::HeapItem *item = reinterpret_cast<const QV4::HeapItem *>(
                mm->allocate<QV4::OneSlot>());
        for (const QV4::PersistentValue &value : std::as_const(QV4::allocatedDuringSweep)) {
            const QV4::HeapItem *owner
                    = reinterpret_cast<const QV4::HeapItem *>(value.asManaged()->heapObject());
            const size_t nValues = value.asManaged()->vtable()->nInlineProperties
                    + value.asManaged()->vtable()->inlinePropertyOffset;
            const size_t nSlots = nValues * sizeof(QV4::Value) / QV4::Chunk::SlotSize;
            if (item > owner && item < owner + nSlots)
                ++allocatedInsideLiveObject;
        }
    }
    mm->gcBlocked = QV4::MemoryManager::Unblocked;
    QCOMPARE(allocatedInsideLiveObject, 0);

    QV4::allocatedDuringSweep.clear();
}

QTEST_GUILESS_MAIN(QtRuntimeTests)
#include "qt_runtime_tests.moc"
