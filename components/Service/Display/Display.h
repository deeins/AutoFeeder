#ifndef __DISPLAY_H
#define __DISPLAY_H

/*
 * 显示模块（状态显示服务）—— 系统状态/故障的 OLED 展示
 * 设计见《模块设计/服务/显示模块.md》；外部只经本模块控制 OLED
 */

/* 初始化：OLED 驱动 + 队列/任务 + 事件订阅（app_main 调用一次） */
void Display_Init(void);

#endif
