#include "TS_Heap.h"
#include "TimeScheduler.h"
#include <stddef.h>
#include <stdint.h>

static TsRegisterData_t s_MinHeap[TS_MIN_HEAP_MAX];
static uint8_t s_HeapCnt = 0;

static bool TS_HEAP_GetElem(TsRegisterData_t* Data, uint8_t Idx)
{
    if (s_HeapCnt <= Idx)
    {
        return false;
    }
    *Data = s_MinHeap[Idx];
    return true;
}

static void TS_HEAP_Drop(TsRegisterData_t* Elems, int Cnt, int Idx)
{
    while (Idx < Cnt)
    {
        int smallest = Idx;
        int left = Idx * 2 + 1;
        int right = Idx * 2 + 2;
        if (left < Cnt && Elems[left].DueLocalEpoch < Elems[Idx].DueLocalEpoch)
        {
            smallest = left;
        }
        if (right < Cnt && Elems[right].DueLocalEpoch < Elems[smallest].DueLocalEpoch)
        {
            smallest = right;
        }

        if (Idx == smallest)
        {
            return;
        }

        TsRegisterData_t temp = Elems[Idx];
        Elems[Idx] = Elems[smallest];
        Elems[smallest] = temp;

        Idx = smallest;
    }
}

static void TS_HEAP_HeapConstr(TsRegisterData_t* Elems, int Cnt)
{
    for (int i = Cnt / 2 - 1; i >= 0; i--)
    {
        TS_HEAP_Drop(Elems, Cnt, i);
    }
}

bool TS_HEAP_Empty(void)
{
    return s_HeapCnt == 0;
}

bool TS_HEAP_Top(TsRegisterData_t* Data)
{
    return TS_HEAP_GetElem(Data, 0);
}

bool TS_HEAP_Push(TsRegisterData_t* Data)
{
    if (s_HeapCnt >= TS_MIN_HEAP_MAX)
    {
        return false;
    }

    s_MinHeap[s_HeapCnt] = *Data;
    int idx = s_HeapCnt++;

    if (s_HeapCnt == 1)
    {
        return true;
    }

    while (idx > 0)
    {
        int i = (idx - 1) / 2;
        if (s_MinHeap[i].DueLocalEpoch > s_MinHeap[idx].DueLocalEpoch)
        {
            TsRegisterData_t temp = s_MinHeap[idx];
            s_MinHeap[idx] = s_MinHeap[i];
            s_MinHeap[i] = temp;
            idx = i;
        }
        else 
        {
            break;
        }
    }
    return true;
}

void TS_HEAP_Pop(void)
{
    if (s_HeapCnt == 0)
    {
        return;
    }

    s_MinHeap[0] = s_MinHeap[s_HeapCnt - 1];
    s_HeapCnt--;

    if (s_HeapCnt > 1)
    {
        TS_HEAP_Drop(s_MinHeap, s_HeapCnt, 0);
    }
}

bool TS_HEAP_RemoveByHandles(esp_event_base_t Source, uint32_t* Handles, uint8_t Cnt)
{
    int write = 0;
    for (int read = 0; read < s_HeapCnt; read++)
    {
        bool del = false;
        for (int i = 0; i < Cnt; i++)
        {
            /* 身份键 = (Source, Handle)：条目只归登记源管理，异源同句柄不删 */
            if (s_MinHeap[read].Head.Source == Source &&
                s_MinHeap[read].Handle == Handles[i])
            {
                del = true;
                break;
            }
        }
        if (!del)
        {
            s_MinHeap[write++] = s_MinHeap[read];
        }
    }

    bool removed = write != s_HeapCnt;
    s_HeapCnt = write;

    if (removed && s_HeapCnt > 1)
    {
        TS_HEAP_HeapConstr(s_MinHeap, s_HeapCnt);
    }

    return removed;
}

void TS_HEAP_Clear(void)
{
    s_HeapCnt = 0;
}
