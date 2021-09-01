#include <DataStreams/CountingBlockOutputStream.h>
#include <Common/ProfileEvents.h>


namespace ProfileEvents
{
    extern const Event InsertedRows;
    extern const Event InsertedBytes;
}


namespace DB
{

void CountingTransform::transform(Chunk & chunk)
{
    Progress local_progress(chunk.getNumRows(), chunk.bytes(), 0);
    progress.incrementPiecewiseAtomically(local_progress);

    ProfileEvents::increment(ProfileEvents::InsertedRows, local_progress.read_rows);
    ProfileEvents::increment(ProfileEvents::InsertedBytes, local_progress.read_bytes);

    if (process_elem)
        process_elem->updateProgressOut(local_progress);

    if (progress_callback)
        progress_callback(local_progress);
}

}
