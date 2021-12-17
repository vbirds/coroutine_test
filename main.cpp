#include <iostream>

#include "common/coroutine.h"
#include <set>

using namespace pebble;

std::set<int64_t > coWaitSet;

int32_t MakeCoroutine(CoroutineSchedule *pSchedule, const cxx::function<void()>& routine)
{
    if (!pSchedule)
    {
        return -1;
    }

    if (!routine)
    {
        return -2;
    }

    CommonCoroutineTask* task = pSchedule->NewTask<pebble::CommonCoroutineTask>();
    if (NULL == task)
    {
        return -3;
    }

    task->Init(routine);
    int64_t coid = task->Start(true);
    return coid < 0 ? -1 : 0;
}

int Test(int i, CoroutineSchedule *pSchedule)
{
    // 运行在协程里
    printf("begin MakeCoroutine task id:%d\n", i);
    for (int k = 0; k < 5; k++)
    {
        printf("task id %d loop idx %d\n",i , k);
        coWaitSet.insert(pSchedule->CurrentTaskId());
        pSchedule->Yield();
    }
    printf("end MakeCoroutine task id: %d\n", i);
    return 0;
}

int main()
{
    CoroutineSchedule schedule;
    schedule.Init();

    for (int i = 0; i < 5; i++)
    {
        MakeCoroutine(&schedule, cxx::bind(Test, i, &schedule));
    }

    while (!coWaitSet.empty())
    {

    }

    return 0;
}
