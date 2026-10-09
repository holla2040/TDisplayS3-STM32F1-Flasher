.DEFAULT_GOAL := bin
ip=STM32Flasher.local
port ?= /dev/ttyACM0
BIN := $(wildcard /tmp/arduino-build-stm32/*.ino.bin)


# Push a target image (.bin or .hex) to the running flasher, e.g.:
#   make image IMG=path/to/app.ino.bin
image:
	curl -f -F 'image=@$(IMG)' http://$(ip)/upload

# T-Display S3: 16MB flash, OPI PSRAM (per Xinyuan-LilyGO/T-Display-S3 README)
bin:
	@ arduino-cli compile --build-path /tmp/arduino-build-stm32 -b esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi .


flash:
	make ota

usb:
	make serial

serial:
	- pkill -9 -f microcom
	arduino-cli upload -p $(port) --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc /tmp/arduino-build-stm32 --input-dir /tmp/arduino-build-stm32

clean:
	rm -rf /tmp/arduino-build-stm32

ota:
	python3 "/home/holla/.arduino15/packages/esp32/hardware/esp32/3.3.0/tools/espota.py" -r -i $(ip) -p 3232 --auth=admin -f "$(BIN)"
