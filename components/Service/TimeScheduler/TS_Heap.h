#ifndef __TS_HEAP_H
#define __TS_HEAP_H
#include "TimeScheduler.h"

#define TS_MIN_HEAP_MAX 30

bool TS_HEAP_Empty(void);

bool TS_HEAP_Top(TsRegisterData_t* Data);

bool TS_HEAP_Push(TsRegisterData_t* Data);

void TS_HEAP_Pop(void);

bool TS_HEAP_RemoveByHandles(esp_event_base_t Source, uint32_t* Handles, uint8_t Cnt);

void TS_HEAP_Clear(void);

#endif
