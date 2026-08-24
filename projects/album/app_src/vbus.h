/**
 * 偵測 CN18（USB1）上有沒有 VBUS —— 也就是「USB 線插到電腦了沒」。
 */
#ifndef VBUS_H
#define VBUS_H

#include <stdbool.h>
#include <stdint.h>

void     vbus_init(void);       /* 只需呼叫一次；失敗不會當掉，之後一律回 0 */
uint32_t vbus_mv(void);         /* VBUS 電壓（毫伏）。沒插線約 0 */
bool     vbus_present(void);    /* 超過門檻視為「插上主機了」 */

/* 交棒前把 ADC 收乾淨。要跳去別的 app 時一定要呼叫 ——
 * 留著 ADC2 在連續轉換，對方的 ADC 初始化會失敗。 */
void     vbus_deinit(void);

#endif /* VBUS_H */
