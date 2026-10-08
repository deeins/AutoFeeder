#ifndef __NET_INIT_H
#define __NET_INIT_H

/*
 * NetInit —— 网络模块唯一组装根（§11.1）
 *   认识全体、按序 init；对外只暴露这一个初始化入口。
 *   设计文档：模块设计/服务/网络模块.md §零 / §11.1
 */

void Network_Init(void);

#endif /* __NET_INIT_H */
