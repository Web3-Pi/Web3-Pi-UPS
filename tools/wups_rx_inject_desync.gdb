# Deterministic bench test of the deframer sync-recovery fix (esp32 >= 0.8.8).
# Forces the ESP32 deframer into a mid-frame state so it swallows the next
# ~200 bytes of the RP2040 stream as bogus payload, fails the checksum and has
# to re-lock. Pre-0.8.8 firmware falls into the one-frame-behind trap here
# (only inner frames decode, no MQTT relay); 0.8.8 must re-lock within one
# frame: watch logs/serial.log for `bad=` +1 and `(N to us)` still advancing.
#
#   openocd -f board/esp32s3-builtin.cfg &          # keep the halt SHORT (seconds)
#   xtensa-esp32s3-elf-gdb -q -batch build/firmware-ESP32-LTE-M.elf -x tools/wups_rx_inject_desync.gdb
#   printf 'resume\n' | nc localhost 4444           # ALWAYS resume + verify `targets` = running
set pagination off
set confirm off
target extended-remote :3333
print s_rx.state
set var s_rx.state = WUPS_RX_PAYLOAD
set var s_rx.len = 200
set var s_rx.pidx = 0
print s_rx.state
print s_rx_bad
print s_frames_rx_unicast
detach
quit
