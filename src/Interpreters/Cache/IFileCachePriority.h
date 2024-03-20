#pragma once

#include <memory>
#include <Core/Types.h>
#include <Common/Exception.h>
#include <Interpreters/Cache/FileSegmentInfo.h>
#include <Interpreters/Cache/Guards.h>
#include <Interpreters/Cache/IFileCachePriority.h>
#include <Interpreters/Cache/FileCache_fwd_internal.h>
#include <Interpreters/Cache/UserInfo.h>

namespace DB
{
struct FileCacheReserveStat;
class EvictionCandidates;

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

class IFileCachePriority : private boost::noncopyable
{
public:
    using Key = FileCacheKey;
    using QueueEntryType = FileCacheQueueEntryType;
    using UserInfo = FileCacheUserInfo;
    using UserID = UserInfo::UserID;

    struct Entry
    {
        Entry(const Key & key_, size_t offset_, size_t size_, KeyMetadataPtr key_metadata_);
        Entry(const Entry & other);

        const Key key;
        const size_t offset;
        const KeyMetadataPtr key_metadata;

        std::atomic<size_t> size;
        size_t hits = 0;

        std::string toString() const { return fmt::format("{}:{}:{}", key, offset, size); }

        bool isEvicting(const CachePriorityGuard::Lock &) const { return evicting; }
        bool isEvicting(const LockedKey &) const { return evicting; }
        /// This does not look good to have isEvicting with two options for locks,
        /// but still it is valid as we do setEvicting always under both of them.
        /// (Well, not always - only always for setting it to True,
        /// but for False we have lower guarantees and allow a logical race,
        /// physical race is not possible because the value is atomic).
        /// We can avoid this ambiguity for isEvicting by introducing
        /// a separate lock `EntryGuard::Lock`, it will make this part of code more coherent,
        /// but it will introduce one more mutex while it is avoidable.
        /// Introducing one more mutex just for coherency does not win the trade-off (isn't it?).
        void setEvicting(bool evicting_, const LockedKey * locked_key, const CachePriorityGuard::Lock * lock) const
        {
            if (evicting_ && (!locked_key || !lock))
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                                "Setting evicting state to `true` can be done only under lock");

            chassert(evicting.load() != evicting_);
            evicting.store(evicting_);
        }

    private:
        mutable std::atomic<bool> evicting = false;
    };
    using EntryPtr = std::shared_ptr<Entry>;

    class Iterator
    {
    public:
        virtual ~Iterator() = default;

        virtual EntryPtr getEntry() const = 0;

        virtual size_t increasePriority(const CachePriorityGuard::Lock &) = 0;

        /// Note: IncrementSize unlike decrementSize requires a cache lock, because
        /// it requires more consistency guarantees for eviction.

        virtual void incrementSize(size_t size, const CachePriorityGuard::Lock &) = 0;

        virtual void decrementSize(size_t size) = 0;

        virtual void remove(const CachePriorityGuard::Lock &) = 0;

        virtual void invalidate() = 0;

        virtual QueueEntryType getType() const = 0;
    };
    using IteratorPtr = std::shared_ptr<Iterator>;

    virtual ~IFileCachePriority() = default;

    size_t getElementsLimit(const CachePriorityGuard::Lock &) const { return max_elements; }

    size_t getSizeLimit(const CachePriorityGuard::Lock &) const { return max_size; }

    virtual size_t getSize(const CachePriorityGuard::Lock &) const = 0;

    virtual size_t getSizeApprox() const = 0;

    virtual size_t getElementsCount(const CachePriorityGuard::Lock &) const = 0;

    virtual size_t getElementsCountApprox() const = 0;

    virtual QueueEntryType getDefaultQueueEntryType() const = 0;

    virtual std::string getStateInfoForLog(const CachePriorityGuard::Lock &) const = 0;

    virtual void check(const CachePriorityGuard::Lock &) const;

    /// Throws exception if there is not enough size to fit it.
    virtual IteratorPtr add( /// NOLINT
        KeyMetadataPtr key_metadata,
        size_t offset,
        size_t size,
        const UserInfo & user,
        const CachePriorityGuard::Lock &,
        bool best_effort = false) = 0;

    /// `reservee` is the entry for which are reserving now.
    /// It does not exist, if it is the first space reservation attempt
    /// for the corresponding file segment.
    virtual bool canFit( /// NOLINT
        size_t size,
        size_t elements,
        const CachePriorityGuard::Lock &,
        IteratorPtr reservee = nullptr,
        bool best_effort = false) const = 0;

    virtual void shuffle(const CachePriorityGuard::Lock &) = 0;

    struct IPriorityDump
    {
        virtual ~IPriorityDump() = default;
    };
    using PriorityDumpPtr = std::shared_ptr<IPriorityDump>;

    virtual PriorityDumpPtr dump(const CachePriorityGuard::Lock &) = 0;

    virtual bool collectCandidatesForEviction(
        size_t size,
        FileCacheReserveStat & stat,
        EvictionCandidates & res,
        IteratorPtr reservee,
        const UserID & user_id,
        bool & reached_size_limit,
        bool & reached_elements_limit,
        const CachePriorityGuard::Lock &) = 0;

    virtual bool modifySizeLimits(size_t max_size_, size_t max_elements_, double size_ratio_, const CachePriorityGuard::Lock &) = 0;

    struct HoldSpace : private boost::noncopyable
    {
        HoldSpace(
            size_t size_,
            size_t elements_,
            QueueEntryType queue_entry_type_,
            IFileCachePriority & priority_,
            const CachePriorityGuard::Lock & lock)
            : size(size_), elements(elements_), queue_entry_type(queue_entry_type_), priority(priority_)
        {
            priority.holdImpl(size, elements, queue_entry_type, lock);
        }

        void release()
        {
            if (released)
                return;
            released = true;
            priority.releaseImpl(size, elements, queue_entry_type);
        }

        ~HoldSpace()
        {
            if (!released)
                release();
        }

    private:
        const size_t size;
        const size_t elements;
        const QueueEntryType queue_entry_type;
        IFileCachePriority & priority;
        bool released = false;
    };
    HoldSpace takeHold();

protected:
    IFileCachePriority(size_t max_size_, size_t max_elements_);

    virtual void holdImpl(
        size_t size,
        size_t elements,
        QueueEntryType queue_entry_type,
        const CachePriorityGuard::Lock & lock) = 0;

    virtual void releaseImpl(size_t size, size_t elements, QueueEntryType queue_entry_type) = 0;

    size_t max_size = 0;
    size_t max_elements = 0;
};

}
