#include "mouse_move_accumulator.h"

#include <cstdio>
#include <cstdlib>

namespace {

void ExpectPending(const char* name,
                   const MouseMoveAccumulator& accumulator,
                   int expected_x,
                   int expected_y,
                   int expected_wheel) {
    if (accumulator.pending_x() != expected_x ||
        accumulator.pending_y() != expected_y ||
        accumulator.pending_wheel() != expected_wheel) {
        std::fprintf(stderr,
                     "%s: expected pending (%d, %d, %d), got (%ld, %ld, %ld)\n",
                     name,
                     expected_x,
                     expected_y,
                     expected_wheel,
                     static_cast<long>(accumulator.pending_x()),
                     static_cast<long>(accumulator.pending_y()),
                     static_cast<long>(accumulator.pending_wheel()));
        std::exit(1);
    }
}

void ExpectChunk(const char* name,
                 MouseMoveChunk chunk,
                 int expected_x,
                 int expected_y,
                 int expected_wheel) {
    if (chunk.x != expected_x || chunk.y != expected_y ||
        chunk.wheel != expected_wheel) {
        std::fprintf(stderr,
                     "%s: expected chunk (%d, %d, %d), got (%d, %d, %d)\n",
                     name,
                     expected_x,
                     expected_y,
                     expected_wheel,
                     chunk.x,
                     chunk.y,
                     chunk.wheel);
        std::exit(1);
    }
}

}  // namespace

int main() {
    MouseMoveAccumulator accumulator;
    if (accumulator.HasPending()) {
        std::fprintf(stderr, "new accumulator should be empty\n");
        return 1;
    }

    accumulator.Add(40, -30, 2);
    accumulator.Add(20, -10, 1);
    ExpectPending("coalesces samples", accumulator, 60, -40, 3);
    MouseMoveChunk first = accumulator.PeekChunk();
    ExpectChunk("small pending fits one report", first, 60, -40, 3);
    accumulator.Commit(first);
    ExpectPending("successful report drains pending", accumulator, 0, 0, 0);

    accumulator.Add(300, -280, 0);
    MouseMoveChunk large = accumulator.PeekChunk();
    ExpectChunk("large pending is chunked", large, 127, -127, 0);
    accumulator.Commit(large);
    ExpectPending("chunk leaves remainder", accumulator, 173, -153, 0);
    MouseMoveChunk second = accumulator.PeekChunk();
    accumulator.Commit(second);
    MouseMoveChunk third = accumulator.PeekChunk();
    accumulator.Commit(third);
    ExpectPending("multiple chunks preserve total distance", accumulator, 0, 0, 0);

    accumulator.Add(80, 40, 0);
    accumulator.Add(-30, -60, 0);
    ExpectPending("opposite movement cancels backlog", accumulator, 50, -20, 0);
    accumulator.Reset();
    ExpectPending("reset clears backlog", accumulator, 0, 0, 0);

    const bool clamped = accumulator.Add(5000, -5000, 5000);
    if (!clamped) {
        std::fprintf(stderr, "oversized pending should report clamping\n");
        return 1;
    }
    ExpectPending("pending is bounded",
                  accumulator,
                  MouseMoveAccumulator::kMaximumPending,
                  -MouseMoveAccumulator::kMaximumPending,
                  MouseMoveAccumulator::kMaximumPending);

    return 0;
}
