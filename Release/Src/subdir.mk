################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Src/adc.c \
../Src/clock_port_display.c \
../Src/clock_port_font.c \
../Src/clock_port_i2c3.c \
../Src/clock_port_math.c \
../Src/clock_port_rtc.c \
../Src/clock_port_sensors.c \
../Src/clock_port_ui.c \
../Src/flash.c \
../Src/gpdma.c \
../Src/gpio.c \
../Src/i2c.c \
../Src/icache.c \
../Src/main.c \
../Src/rtc.c \
../Src/spi.c \
../Src/stm32u3xx_hal_msp.c \
../Src/stm32u3xx_it.c \
../Src/syscalls.c \
../Src/sysmem.c \
../Src/system_stm32u3xx.c 

OBJS += \
./Src/adc.o \
./Src/clock_port_display.o \
./Src/clock_port_font.o \
./Src/clock_port_i2c3.o \
./Src/clock_port_math.o \
./Src/clock_port_rtc.o \
./Src/clock_port_sensors.o \
./Src/clock_port_ui.o \
./Src/flash.o \
./Src/gpdma.o \
./Src/gpio.o \
./Src/i2c.o \
./Src/icache.o \
./Src/main.o \
./Src/rtc.o \
./Src/spi.o \
./Src/stm32u3xx_hal_msp.o \
./Src/stm32u3xx_it.o \
./Src/syscalls.o \
./Src/sysmem.o \
./Src/system_stm32u3xx.o 

C_DEPS += \
./Src/adc.d \
./Src/clock_port_display.d \
./Src/clock_port_font.d \
./Src/clock_port_i2c3.d \
./Src/clock_port_math.d \
./Src/clock_port_rtc.d \
./Src/clock_port_sensors.d \
./Src/clock_port_ui.d \
./Src/flash.d \
./Src/gpdma.d \
./Src/gpio.d \
./Src/i2c.d \
./Src/icache.d \
./Src/main.d \
./Src/rtc.d \
./Src/spi.d \
./Src/stm32u3xx_hal_msp.d \
./Src/stm32u3xx_it.d \
./Src/syscalls.d \
./Src/sysmem.d \
./Src/system_stm32u3xx.d 


# Each subdirectory must supply rules for building sources it contributes
Src/%.o Src/%.su Src/%.cyclo: ../Src/%.c Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -DUSE_HAL_DRIVER -DSTM32U375xx -c -I../Inc -I../Drivers/STM32U3xx_HAL_Driver/Inc -I../Drivers/STM32U3xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32U3xx/Include -I../Drivers/CMSIS/Include -Ofast -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Src

clean-Src:
	-$(RM) ./Src/adc.cyclo ./Src/adc.d ./Src/adc.o ./Src/adc.su ./Src/clock_port_display.cyclo ./Src/clock_port_display.d ./Src/clock_port_display.o ./Src/clock_port_display.su ./Src/clock_port_font.cyclo ./Src/clock_port_font.d ./Src/clock_port_font.o ./Src/clock_port_font.su ./Src/clock_port_i2c3.cyclo ./Src/clock_port_i2c3.d ./Src/clock_port_i2c3.o ./Src/clock_port_i2c3.su ./Src/clock_port_math.cyclo ./Src/clock_port_math.d ./Src/clock_port_math.o ./Src/clock_port_math.su ./Src/clock_port_rtc.cyclo ./Src/clock_port_rtc.d ./Src/clock_port_rtc.o ./Src/clock_port_rtc.su ./Src/clock_port_sensors.cyclo ./Src/clock_port_sensors.d ./Src/clock_port_sensors.o ./Src/clock_port_sensors.su ./Src/clock_port_ui.cyclo ./Src/clock_port_ui.d ./Src/clock_port_ui.o ./Src/clock_port_ui.su ./Src/flash.cyclo ./Src/flash.d ./Src/flash.o ./Src/flash.su ./Src/gpdma.cyclo ./Src/gpdma.d ./Src/gpdma.o ./Src/gpdma.su ./Src/gpio.cyclo ./Src/gpio.d ./Src/gpio.o ./Src/gpio.su ./Src/i2c.cyclo ./Src/i2c.d ./Src/i2c.o ./Src/i2c.su ./Src/icache.cyclo ./Src/icache.d ./Src/icache.o ./Src/icache.su ./Src/main.cyclo ./Src/main.d ./Src/main.o ./Src/main.su ./Src/rtc.cyclo ./Src/rtc.d ./Src/rtc.o ./Src/rtc.su ./Src/spi.cyclo ./Src/spi.d ./Src/spi.o ./Src/spi.su ./Src/stm32u3xx_hal_msp.cyclo ./Src/stm32u3xx_hal_msp.d ./Src/stm32u3xx_hal_msp.o ./Src/stm32u3xx_hal_msp.su ./Src/stm32u3xx_it.cyclo ./Src/stm32u3xx_it.d ./Src/stm32u3xx_it.o ./Src/stm32u3xx_it.su ./Src/syscalls.cyclo ./Src/syscalls.d ./Src/syscalls.o ./Src/syscalls.su ./Src/sysmem.cyclo ./Src/sysmem.d ./Src/sysmem.o ./Src/sysmem.su ./Src/system_stm32u3xx.cyclo ./Src/system_stm32u3xx.d ./Src/system_stm32u3xx.o ./Src/system_stm32u3xx.su

.PHONY: clean-Src

