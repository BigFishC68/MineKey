# =============================================================================
# MineKey CH552 固件 —— SDCC 构建
#
# 需要：SDCC 4.2+（sdcc / packihx / makebin 在 PATH 上）
#   Windows 安装包：https://sourceforge.net/projects/sdcc/files/sdcc-win64/
#   本机实测安装在 D:\Keyboard_Frame\SDCC，build.bat 会自动找
#
#   make            生成 minekey.hex / minekey.bin
#   make clean
#   make size       打印 RAM / Flash 占用
# =============================================================================

PROJ    = minekey
SDCC    = sdcc
PACKIHX = packihx
MAKEBIN = makebin
BUILD   = build

SRCS = src/app.c src/usb.c
HDRS = src/board.h src/ch552.h src/usb.h src/app.h
OBJS = $(BUILD)/app.rel $(BUILD)/usb.rel

# --xram-size 0x380 : 0x380..0x3FF 留给 USB 端点缓冲区（见 usb.c 的 Ep0Buffer/Ep1Buffer）
# --code-size 0x3800: 0x3800 起是 WCH Bootloader，不能覆盖
# 注意：sdcc 一次只能编译一个 .c，必须 -c 成 .rel 再链接
CFLAGS = -mmcs51 \
         --model-small \
         --std-c99 \
         --opt-code-size \
         --xram-loc 0x0000 \
         --xram-size 0x380 \
         --iram-size 0x100 \
         --code-size 0x3800 \
         --Werror

LDFLAGS = -mmcs51 \
          --model-small \
          --xram-loc 0x0000 \
          --xram-size 0x380 \
          --iram-size 0x100 \
          --code-size 0x3800

all: $(PROJ).hex $(PROJ).bin

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/app.rel: src/app.c $(HDRS) | $(BUILD)
	$(SDCC) $(CFLAGS) -c -o $@ $<

$(BUILD)/usb.rel: src/usb.c $(HDRS) | $(BUILD)
	$(SDCC) $(CFLAGS) -c -o $@ $<

$(BUILD)/$(PROJ).ihx: $(OBJS)
	$(SDCC) $(LDFLAGS) -o $@ $(OBJS)

$(PROJ).hex: $(BUILD)/$(PROJ).ihx
	$(PACKIHX) $< > $@

$(PROJ).bin: $(BUILD)/$(PROJ).ihx
	$(MAKEBIN) -p $< $@

size: $(BUILD)/$(PROJ).ihx
	@echo "--- 资源占用 ---"
	@grep -E "ROM/EPROM/FLASH|EXTERNAL RAM" $(BUILD)/$(PROJ).mem || true

clean:
	rm -rf $(BUILD) $(PROJ).hex $(PROJ).bin

flash: $(PROJ).hex
	@echo "用 WCHISPTool 载入 $(PROJ).hex 烧录（设备需处于 4348:55E0 下载模式）"

.PHONY: all clean size flash
