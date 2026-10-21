#pragma once
#include <cstdint>

void ax_init(uint32_t ucode_hash);
void ax_process_cmdlist(uint32_t addr, uint16_t size);
