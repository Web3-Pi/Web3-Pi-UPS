# Non-destructive forensic dump of the ESP32 wups_link state over USB-JTAG.
# Usage:  xtensa-esp-elf-gdb -q build/firmware-ESP32-LTE-M.elf -x wups_rx_dump.gdb
# Requires openocd running:  openocd -f board/esp32s3-builtin.cfg
set pagination off
set confirm off
set logging file wups_rx_dump.log
set logging overwrite on
set logging enabled on
target extended-remote :3333
echo \n===== wups_link counters =====\n
print s_frames_tx
print s_bytes_tx
print s_frames_rx
print s_bytes_rx
print s_rx_resync
print s_rx_reinstalls
print s_last_frame_ms
print (unsigned)(esp_timer_get_time()/1000)
print s_rx
echo \n===== mqtt state =====\n
print s_connected
print s_client->state
print s_client->outbox->size
print s_client->mqtt_state.pending_msg_count
echo \n===== UART2 registers (DR_REG_UART2_BASE 0x6002E000) =====\n
x/1wx 0x6002E004
echo   ^ UART_INT_RAW\n
x/1wx 0x6002E008
echo   ^ UART_INT_ST\n
x/1wx 0x6002E00C
echo   ^ UART_INT_ENA (bit0 RXFIFO_FULL, bit4 RXFIFO_OVF, bit8 RXFIFO_TOUT)\n
x/1wx 0x6002E01C
echo   ^ UART_STATUS (bits 0-9 RXFIFO_CNT, 16-25 TXFIFO_CNT)\n
x/1wx 0x6002E068
echo   ^ UART_MEM_RX_STATUS\n
echo \n===== tasks =====\n
info threads
echo \n===== per-task backtraces =====\n
thread apply all bt 12
detach
quit
