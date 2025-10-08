#include <library/cpp/coroutine/engine/impl.h>


TCont* WaitingCont;
TCont* ReadyCont;


void Wait(TCont* cont) {
    cont->SleepI();
}


void BreakpointFunc() {
    [[maybe_unused]] volatile int dummy;
    dummy = 0;
}


void Dispatcher(TCont* cont) {
    cont->SleepT(TDuration::MilliSeconds(100));
    ReadyCont->ReSchedule();
    BreakpointFunc();
    WaitingCont->ReSchedule();
}


int main() {
    TContExecutor executor(65536);
    WaitingCont = executor.Create(Wait, "waiting coroutine");
    ReadyCont = executor.Create(Wait, "ready coroutine");
    executor.Create(Dispatcher, "dispatching coroutine");
    executor.Execute();
}
