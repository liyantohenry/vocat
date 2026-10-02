/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start Wi-Fi. Connects with the network the Wi-Fi driver has saved. With none, or when it keeps
 *        failing, opens the open setup network "VoCat-Setup-XXXX" with a captive page where any phone or
 *        laptop picks a network and enters its password. No accounts or apps involved.
 */
esp_err_t wifi_setup_start(void);

/** @brief Forget the saved network and open the setup network. Safe from any task, including the gfx task. */
void wifi_setup_forget(void);

#ifdef __cplusplus
}
#endif
