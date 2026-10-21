// eats-empty-catch-all

#include <exception>

void Work();
void Log();

void EmptyCatchAll() {
    try {
        Work();
    } catch (...) { // EXPECT-WARNING: swallows every exception
    }
}

void EmptyCatchAllWithComment() {
    try {
        Work();
    } catch (...) { // EXPECT-WARNING: a comment is not a handler
        // ignored
    }
}

void CatchAllRethrows() {
    try {
        Work();
    } catch (...) { // EXPECT-NO-WARNING
        throw;
    }
}

void CatchAllLogs() {
    try {
        Work();
    } catch (...) { // EXPECT-NO-WARNING
        Log();
    }
}

void EmptyTypedCatch() {
    try {
        Work();
    } catch (const std::exception&) { // EXPECT-NO-WARNING: only catch (...) is checked
    }
}

void SuppressedEmptyCatchAll() {
    try {
        Work();
    } catch (...) { // NOLINT(eats-empty-catch-all) EXPECT-NO-WARNING: suppressed
    }
}

void SuppressedNextLine() {
    try {
        Work();
    // NOLINTNEXTLINE(eats-empty-catch-all)
    } catch (...) { // EXPECT-NO-WARNING: suppressed
    }
}

template <typename T>
void EmptyCatchAllInTemplate() {
    try {
        T::Work();
    } catch (...) { // EXPECT-WARNING: reported once, not per instantiation
    }
}

struct Worker {
    static void Work();
};

struct OtherWorker {
    static void Work();
};

template void EmptyCatchAllInTemplate<Worker>();
template void EmptyCatchAllInTemplate<OtherWorker>();

struct Resource {
    Resource() try : value_(0) {
    } catch (...) { // EXPECT-NO-WARNING: constructor handler rethrows implicitly
    }

    ~Resource() try {
    } catch (...) { // EXPECT-NO-WARNING: destructor handler rethrows implicitly
    }

    int value_;
};

void FunctionTryBlock() try {
    Work();
} catch (...) { // EXPECT-WARNING: an ordinary function handler does swallow
}
